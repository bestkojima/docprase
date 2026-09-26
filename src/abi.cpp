#include "dococr/dococr.h"
#include "dococr/inference.hpp"
#include "backend_factory.hpp"
#include "config.hpp"
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {
struct Engine {
    std::string config;
    std::shared_ptr<const dococr::ExecutionPlan> plan;
    std::unique_ptr<dococr::IInferenceEngine> backend;
    size_t jobs = 0;
    bool running = false;
    ~Engine() { if (backend) backend->unload(); }
};
struct Job {
    DocOcrHandle engine = 0;
    std::shared_ptr<const dococr::ExecutionPlan> plan;
    bool started = false;
    bool running = false;
    std::atomic_bool cancelled{false};
    std::string state = "created";
    std::string error_code;
    std::string error_stage;
    std::string error_message;
    dococr::JobOutput output;
    bool has_result = false;
    std::string manifest;
};
thread_local std::string last_error;
std::mutex registry_mutex;
std::map<DocOcrHandle, std::shared_ptr<Engine>> engines;
std::map<DocOcrJob, std::shared_ptr<Job>> jobs;
std::map<void*, uint64_t> allocations;
uint64_t next_handle = 1;
uint64_t next_allocation = 1;

template<class F> DocOcrStatus guarded(F&& callback) noexcept {
    try { return callback(); }
    catch (...) { return DOCOCR_FAILED; }
}

DocOcrBytes copy_bytes(const void* data, size_t size) {
    auto* pointer = static_cast<uint8_t*>(std::malloc(size ? size : 1));
    if (!pointer) throw std::bad_alloc();
    if (size) std::memcpy(pointer, data, size);
    const uint64_t allocation_id = next_allocation++;
    try { allocations.emplace(pointer, allocation_id); }
    catch (...) { std::free(pointer); throw; }
    return {pointer, size, allocation_id};
}

std::string event_json(const Job& job) {
    std::string out = "{\"state\":\"" + job.state + "\"";
    if (!job.error_code.empty())
        out += ",\"error\":{\"request_id\":\"p0001\",\"stage\":\"" + job.error_stage +
               "\",\"code\":\"" + job.error_code + "\",\"message\":\"" + job.error_message + "\"}";
    return out + "}";
}
std::string manifest_json(const dococr::ExecutionPlan& plan, const dococr::RunResult& result,
                          const std::string& actual_backend) {
    std::string out = "{\"schema_version\":\"1.0\",\"config_hash\":" + dococr::json_quote(plan.config_hash) +
        ",\"actual_backend\":" + dococr::json_quote(actual_backend) +
        ",\"backend_id\":" + dococr::json_quote(plan.backend) +
        ",\"actual_device\":\"cpu\",\"runtime_version\":\"fixture-only\","
        "\"effective_parameters\":{\"threads\":1,\"max_new_tokens\":" + std::to_string(plan.max_new_tokens) +
        ",\"max_page_pixels\":" + std::to_string(plan.max_page_pixels) +
        ",\"max_output_bytes\":" + std::to_string(plan.max_output_bytes) + "},\"artifacts\":[";
    for (size_t i = 0; i < plan.artifacts.size(); ++i) {
        const auto& a = plan.artifacts[i];
        if (i) out += ',';
        out += "{\"model\":" + dococr::json_quote(a.model) + ",\"path\":" + dococr::json_quote(a.path) +
            ",\"sha256\":" + dococr::json_quote(a.sha256) +
            ",\"contract_status\":" + dococr::json_quote(a.contract_status) + "}";
    }
    out += "],\"processing\":[";
    for (size_t i = 0; i < plan.processing.size(); ++i) {
        const auto& step = plan.processing[i];
        std::string status, reason;
        if (!step.enabled) { status = "skipped_disabled"; reason = "optional_disabled"; }
        else if (step.id == "decode") { status = result.did_decode ? "executed" : "failed"; reason = "image_decoded"; }
        else if (step.id == "normalize") {
            status = step.owner == "adapter" ? (result.did_normalize ? "executed" : "failed") :
                     step.owner == "runtime" ? (result.did_layout ? "delegated_runtime" : "failed") :
                     (result.did_layout ? "provided_by_graph" : "failed");
            reason = step.owner == "adapter" ? "rgb8_to_float32_0_1" :
                     step.owner == "runtime" ? "fixture_runtime_normalized" : "fixture_graph_normalized";
        }
        else if (step.id == "crop") { status = result.did_crop ? "executed" : "identity_validated"; reason = result.did_crop ? "region_crop" : "no_regions"; }
        else if (step.id == "session_reset") {
            status = result.reset_failed ? "failed" : result.did_reset ? "delegated_runtime" : "identity_validated";
            reason = result.reset_failed ? "fixture_reset_failed" : result.did_reset ? "fixture_reset_succeeded" : "no_recognition_regions";
        }
        else { status = "identity_validated"; reason = "owned_rgb8_source"; }
        if (i) out += ',';
        out += "{\"id\":" + dococr::json_quote(step.id) + ",\"owner\":" + dococr::json_quote(step.owner) +
            ",\"status\":" + dococr::json_quote(status) + ",\"reason\":" + dococr::json_quote(reason) + "}";
    }
    out += "],\"timings_ms\":{\"decode\":" + std::to_string(result.decode_ms) +
        ",\"layout\":" + std::to_string(result.layout_ms) +
        ",\"recognition\":" + std::to_string(result.recognition_ms) +
        ",\"export\":" + std::to_string(result.export_ms) +
        "},\"metrics\":{\"peak_memory_bytes\":{\"status\":\"unavailable\",\"reason\":\"portable sampler not implemented\"}}}";
    return out;
}
}

extern "C" {
uint32_t dococr_abi_version(void) { return DOCOCR_ABI_VERSION; }

DocOcrStatus dococr_create(DocOcrStringView config, DocOcrHandle* out) {
    return guarded([&] {
        if (!out || (!config.data && config.size) || config.size > 1024 * 1024) return DOCOCR_INVALID_ARGUMENT;
        *out = 0;
        last_error.clear();
        std::string value(config.data ? config.data : "", config.size);
        std::shared_ptr<const dococr::ExecutionPlan> plan;
        auto first = value.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && value[first] == '{') {
            try { plan = dococr::build_plan(value, dococr::config_supported("fixture:sample")); }
            catch (const dococr::ConfigError& error) {
                last_error = "{\"code\":" + dococr::json_quote(error.code) +
                    ",\"detail\":" + dococr::json_quote(error.detail) + "}";
                return DOCOCR_CONFIG_ERROR;
            }
        }
        const std::string backend_name = plan ? plan->backend : value;
        if (!dococr::config_supported(backend_name)) {
            if (!plan) return DOCOCR_UNSUPPORTED;
            last_error = "{\"code\":\"unsupported_backend\",\"detail\":" +
                dococr::json_quote(backend_name) + "}";
            return DOCOCR_CONFIG_ERROR;
        }
        auto engine = std::make_shared<Engine>();
        engine->config = backend_name;
        engine->plan = std::move(plan);
        engine->backend = dococr::make_backend(backend_name);
        if (engine->plan && (!engine->backend || !engine->backend->capabilities().tensor ||
            !engine->backend->capabilities().generation ||
            engine->backend->capabilities().max_concurrent_requests != 1)) {
            last_error = "{\"code\":\"missing_capability\",\"detail\":\"tensor/generation/serial required\"}";
            return DOCOCR_CONFIG_ERROR;
        }
        if (engine->backend && !engine->backend->load()) return DOCOCR_FAILED;
        std::lock_guard<std::mutex> lock(registry_mutex);
        *out = next_handle++;
        engines[*out] = std::move(engine);
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_last_error(DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        *out_json = copy_bytes(last_error.data(), last_error.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_reconfigure(DocOcrHandle handle, DocOcrStringView config) {
    return guarded([&] {
        if (!config.data || !config.size || config.size > 1024 * 1024) return DOCOCR_INVALID_ARGUMENT;
        last_error.clear();
        std::shared_ptr<const dococr::ExecutionPlan> plan;
        try { plan = dococr::build_plan(std::string(config.data, config.size), dococr::config_supported("fixture:sample")); }
        catch (const dococr::ConfigError& error) {
            last_error = "{\"code\":" + dococr::json_quote(error.code) +
                ",\"detail\":" + dococr::json_quote(error.detail) + "}";
            return DOCOCR_CONFIG_ERROR;
        }
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->config != plan->backend) return DOCOCR_UNSUPPORTED;
        it->second->plan = std::move(plan);
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_execution_plan(DocOcrHandle handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        if (!it->second->plan) return DOCOCR_NO_RESULT;
        const auto& text = it->second->plan->json;
        *out_json = copy_bytes(text.data(), text.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_capabilities(DocOcrHandle handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        std::string content = dococr::backend_capabilities(it->second->config);
        *out_json = copy_bytes(content.data(), content.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_create(DocOcrHandle handle, DocOcrJob* out) {
    return guarded([&] {
        if (!out) return DOCOCR_INVALID_ARGUMENT;
        *out = 0;
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        auto job = std::make_shared<Job>();
        job->engine = handle;
        job->plan = it->second->plan;
        *out = next_handle++;
        jobs[*out] = std::move(job);
        ++it->second->jobs;
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_run(DocOcrJob handle, const DocOcrInput* input) {
    return guarded([&] {
        if (!input || input->struct_size < sizeof(DocOcrInput)) return DOCOCR_INVALID_ARGUMENT;
        std::shared_ptr<Job> job;
        std::shared_ptr<Engine> engine;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            auto it = jobs.find(handle);
            if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
            job = it->second;
            engine = engines.at(job->engine);
            if (job->running || engine->running) return DOCOCR_BUSY;
            if (job->started) return DOCOCR_INVALID_ARGUMENT;
            job->started = true;
            job->running = true;
            engine->running = true;
            job->state = "running";
        }
        dococr::RunResult result;
        try {
            dococr::InputView view{input->data, input->size, input->format,
                                   input->width, input->height, input->row_stride};
            result = dococr::run_page(engine->backend.get(), view, job->cancelled, job->plan.get());
        } catch (...) { result.code = dococr::RunCode::Failed; }
        std::lock_guard<std::mutex> lock(registry_mutex);
        job->running = false;
        engine->running = false;
        switch (result.code) {
        case dococr::RunCode::Ok: job->state = "completed"; break;
        case dococr::RunCode::Partial: job->state = "partial"; break;
        case dococr::RunCode::Blank: job->state = "blank"; break;
        case dococr::RunCode::InputError:
            job->state = "failed"; job->error_stage = "decode";
            job->error_code = "input_error"; job->error_message = "invalid image";
            return DOCOCR_INPUT_ERROR;
        case dococr::RunCode::Unsupported:
            job->state = "failed"; job->error_stage = "inference";
            job->error_code = "unsupported_backend"; job->error_message = "no production model backend configured";
            return DOCOCR_UNSUPPORTED;
        case dococr::RunCode::Cancelled:
            job->state = "cancelled"; job->error_stage = "inference";
            job->error_code = "cancelled"; job->error_message = "job cancelled";
            return DOCOCR_CANCELLED;
        case dococr::RunCode::Failed:
            job->state = "failed"; job->error_stage = "inference";
            job->error_code = "inference_error"; job->error_message = "inference contract failed";
            return DOCOCR_FAILED;
        case dococr::RunCode::BudgetExceeded:
            job->state = "failed"; job->error_stage = "budget";
            job->error_code = "budget_exceeded"; job->error_message = "input or output budget exceeded";
            return DOCOCR_BUDGET_EXCEEDED;
        }
        if (job->plan) job->manifest = manifest_json(*job->plan, result, engine->backend->profile());
        job->output = std::move(result.output);
        job->has_result = true;
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_manifest(DocOcrJob handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (!it->second->has_result || it->second->manifest.empty()) return DOCOCR_NO_RESULT;
        *out_json = copy_bytes(it->second->manifest.data(), it->second->manifest.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_result(DocOcrJob handle, DocOcrResult* out) {
    return guarded([&] {
        if (!out || out->struct_size < sizeof(DocOcrResult)) return DOCOCR_INVALID_ARGUMENT;
        out->json = {}; out->markdown = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (!it->second->has_result) return DOCOCR_NO_RESULT;
        const auto& output = it->second->output;
        out->json = copy_bytes(output.json.data(), output.json.size());
        try { out->markdown = copy_bytes(output.markdown.data(), output.markdown.size()); }
        catch (...) { allocations.erase(out->json.data); std::free(out->json.data); out->json = {}; throw; }
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_asset_count(DocOcrJob handle, size_t* out_count) {
    return guarded([&] {
        if (!out_count) return DOCOCR_INVALID_ARGUMENT;
        *out_count = 0;
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (!it->second->has_result) return DOCOCR_NO_RESULT;
        *out_count = it->second->output.assets.size();
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_asset(DocOcrJob handle, size_t index,
                               DocOcrBytes* out_name, DocOcrBytes* out_data) {
    return guarded([&] {
        if (!out_name || !out_data || out_name == out_data) return DOCOCR_INVALID_ARGUMENT;
        *out_name = {}; *out_data = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (!it->second->has_result) return DOCOCR_NO_RESULT;
        const auto& assets = it->second->output.assets;
        if (index >= assets.size()) return DOCOCR_INVALID_ARGUMENT;
        *out_name = copy_bytes(assets[index].name.data(), assets[index].name.size());
        try { *out_data = copy_bytes(assets[index].png.data(), assets[index].png.size()); }
        catch (...) { allocations.erase(out_name->data); std::free(out_name->data); *out_name = {}; throw; }
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_cancel(DocOcrJob handle) {
    return guarded([&] {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        it->second->cancelled = true;
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_poll_events(DocOcrJob handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        std::string content = event_json(*it->second);
        *out_json = copy_bytes(content.data(), content.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_destroy(DocOcrJob handle) {
    return guarded([&] {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->running) return DOCOCR_BUSY;
        --engines.at(it->second->engine)->jobs;
        jobs.erase(it);
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_destroy(DocOcrHandle handle) {
    return guarded([&] {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->jobs || it->second->running) return DOCOCR_BUSY;
        engines.erase(it);
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_bytes_free(DocOcrBytes* bytes) {
    return guarded([&] {
        if (!bytes || !bytes->data) return DOCOCR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = allocations.find(bytes->data);
        if (it == allocations.end() || it->second != bytes->allocation_id) return DOCOCR_INVALID_ARGUMENT;
        std::free(it->first);
        allocations.erase(it);
        *bytes = {};
        return DOCOCR_OK;
    });
}
}
