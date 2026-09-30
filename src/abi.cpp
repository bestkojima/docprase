#include "dococr/dococr.h"
#include "dococr/inference.hpp"
#include "backend_factory.hpp"
#include "config.hpp"
#include "pdf_job.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sstream>
#include <iomanip>
#include <locale>

namespace {
struct Engine {
    std::string config;
    std::shared_ptr<const dococr::ExecutionPlan> plan;
    std::unique_ptr<dococr::IInferenceEngine> backend;
    size_t jobs = 0;
    bool running = false;
    bool ready = true;
    std::string recovery_error;
    ~Engine() { try { if (backend) backend->unload(); } catch (...) {} }
};
struct Job {
    DocOcrHandle engine = 0;
    std::shared_ptr<const dococr::ExecutionPlan> plan;
    bool started = false;
    bool running = false;
    std::atomic_bool cancelled{false};
    bool timed_out = false;
    bool execution_done = false;
    std::condition_variable cv;
    std::deque<std::string> events;
    uint64_t events_dropped = 0;
    uint32_t page_current = 0, page_completed = 0, page_total = 0;
    uint32_t region_completed = 0, region_total = 0;
    std::string state = "created";
    std::string error_code;
    std::string error_request_id = "p0001";
    std::string error_stage;
    std::string error_message;
    dococr::JobOutput output;
    bool has_result = false;
    std::string manifest;
};
extern std::mutex registry_mutex;
struct RunCleanup {
    Job& job;
    Engine& engine;
    std::thread& watcher;
    bool committed = false;
    ~RunCleanup() noexcept {
        if (committed) return;
        try {
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                job.execution_done = true;
                job.cv.notify_all();
            }
            if (watcher.joinable()) watcher.join();
            std::lock_guard<std::mutex> lock(registry_mutex);
            job.state = "failed";
            job.error_stage = "job";
            job.error_code = "failed";
            job.has_result = false;
            engine.ready = false;
            engine.recovery_error = "failed";
            try {
                if (job.events.size() == 64) { job.events.pop_front(); ++job.events_dropped; }
                job.events.emplace_back("{\"kind\":\"terminal\",\"state\":\"failed\"}");
            } catch (...) { ++job.events_dropped; }
            job.running = false;
            engine.running = false;
            job.cv.notify_all();
        } catch (...) {}
    }
};
thread_local std::string last_error;
std::shared_ptr<const dococr::ExecutionPlan> checked_plan(const std::string& value) {
    try { return dococr::build_plan(value, dococr::config_supported("fixture:sample")); }
    catch (const dococr::ConfigError& error) {
        last_error = "{\"code\":" + dococr::json_quote(error.code) +
            ",\"detail\":" + dococr::json_quote(error.detail) + "}";
        return {};
    }
}
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
        out += ",\"error\":{\"request_id\":" + dococr::json_quote(job.error_request_id) +
               ",\"stage\":" + dococr::json_quote(job.error_stage) +
               ",\"code\":" + dococr::json_quote(job.error_code) +
               ",\"message\":" + dococr::json_quote(job.error_message) + "}";
    return out + "}";
}
void push_event(Job& job, const char* kind, uint32_t page = 0,
                const std::string& request_id = {}) {
    std::string event = "{\"kind\":" + dococr::json_quote(kind) +
        ",\"state\":" + dococr::json_quote(job.state) +
        ",\"page\":" + std::to_string(page) +
        ",\"request_id\":" + (request_id.empty() ? "null" : dococr::json_quote(request_id)) +
        ",\"page_completed\":" + std::to_string(job.page_completed) +
        ",\"page_total\":" + std::to_string(job.page_total) +
        ",\"region_completed\":" + std::to_string(job.region_completed) +
        ",\"region_total\":" + std::to_string(job.region_total);
    if (!job.error_code.empty())
        event += ",\"error\":{\"code\":" + dococr::json_quote(job.error_code) +
            ",\"stage\":" + dococr::json_quote(job.error_stage) +
            ",\"message\":" + dococr::json_quote(job.error_message) + "}";
    event += "}";
    if (job.events.size() == 64) { job.events.pop_front(); ++job.events_dropped; }
    job.events.push_back(std::move(event));
}
std::string status_json(const Job& job, const Engine& engine) {
    std::string out = event_json(job);
    out.pop_back();
    out += ",\"running\":" + std::string(job.running ? "true" : "false") +
        ",\"terminal\":" + std::string(job.started && !job.running ? "true" : "false") +
        ",\"cancel_requested\":" + std::string(job.cancelled ? "true" : "false") +
        ",\"timeout_requested\":" + std::string(job.timed_out ? "true" : "false") +
        ",\"cancellation_granularity\":\"after_backend_call\"" +
        ",\"page_current\":" + std::to_string(job.page_current) +
        ",\"page_completed\":" + std::to_string(job.page_completed) +
        ",\"page_total\":" + std::to_string(job.page_total) +
        ",\"region_completed\":" + std::to_string(job.region_completed) +
        ",\"region_total\":" + std::to_string(job.region_total) +
        ",\"events_pending\":" + std::to_string(job.events.size()) +
        ",\"events_dropped\":" + std::to_string(job.events_dropped) +
        ",\"engine_ready\":" + std::string(engine.ready ? "true" : "false");
    if (!engine.recovery_error.empty())
        out += ",\"recovery_error\":" + dococr::json_quote(engine.recovery_error);
    return out + "}";
}
bool same_artifacts(const std::vector<dococr::ArtifactInfo>& a,
                    const std::vector<dococr::ArtifactInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].model != b[i].model || a[i].path != b[i].path ||
            a[i].sha256 != b[i].sha256 || a[i].contract_status != b[i].contract_status) return false;
    return true;
}
std::string manifest_json(const dococr::ExecutionPlan& plan, const dococr::RunResult& result,
                          const std::string& actual_backend) {
    std::ostringstream layout_threshold;
    layout_threshold.imbue(std::locale::classic());
    layout_threshold << std::setprecision(17) << plan.layout_score_threshold;
    std::string job_status = result.code == dococr::RunCode::Cancelled ? "cancelled" :
        result.code == dococr::RunCode::BudgetExceeded ? "budget_exceeded" :
        result.code == dococr::RunCode::Ok ? "ok" : result.code == dococr::RunCode::Partial ? "partial" :
        result.code == dococr::RunCode::Blank ? "blank" : "failed";
    std::string out = "{\"schema_version\":\"1.0\",\"config_hash\":" + dococr::json_quote(plan.config_hash) +
        ",\"job_status\":" + dococr::json_quote(job_status) +
        ",\"failure_code\":" + (result.error_code.empty() ? "null" : dococr::json_quote(result.error_code)) +
        ",\"failure_detail\":" + (result.error_message.empty() ? "null" : dococr::json_quote(result.error_message)) +
        ",\"budget_stage\":" + (result.budget_stage.empty() ? "null" : dococr::json_quote(result.budget_stage)) +
        ",\"actual_backend\":" + dococr::json_quote(actual_backend) +
        ",\"backend_id\":" + dococr::json_quote(plan.backend) +
        ",\"actual_device\":\"cpu\",\"runtime_version\":" +
        dococr::json_quote(plan.backend.rfind("mnn:", 0) == 0 ? actual_backend : "fixture-only") + ","
        "\"effective_parameters\":{\"threads\":1,\"max_new_tokens\":" + std::to_string(plan.max_new_tokens) +
        ",\"max_page_pixels\":" + std::to_string(plan.max_page_pixels) +
        ",\"max_output_bytes\":" + std::to_string(plan.max_output_bytes) +
        (plan.uses_doclayout() ? ",\"layout_preprocess\":" + dococr::json_quote(plan.layout_preprocess) +
         ",\"layout_score_threshold\":" + layout_threshold.str() : "") + "},\"artifacts\":[";
    for (size_t i = 0; i < plan.artifacts.size(); ++i) {
        const auto& a = plan.artifacts[i];
        if (i) out += ',';
        out += "{\"model\":" + dococr::json_quote(a.model) + ",\"path\":" + dococr::json_quote(a.path) +
            ",\"sha256\":" + dococr::json_quote(a.sha256) +
            ",\"contract_status\":" + dococr::json_quote(a.contract_status) + "}";
    }
    out += "]";
    if (plan.backend == "mnn:pp-doclayout-v3+ovisocr2") {
        out += ",\"runtime_configuration\":{\"layout_threads\":1,\"ovis_threads\":1,"
            "\"device\":\"cpu\",\"sampler\":\"greedy\",\"seed\":null,"
            "\"seed_status\":\"not_configured\",\"temperature\":0.8,\"top_k\":40,"
            "\"top_p\":0.9,\"min_p\":0.05,\"tfs_z\":1.0,\"typical\":0.95,"
            "\"repetition_penalty\":1.0,\"presence_penalty\":0.0,"
            "\"frequency_penalty\":0.0,\"penalty_window\":0,\"n_gram\":8,"
            "\"ngram_factor\":1.0,\"llm_precision\":\"low\",\"llm_memory\":\"low\","
            "\"vision_precision\":\"normal\",\"vision_memory\":\"low\","
            "\"reuse_kv\":false,"
            "\"prompt_cache\":false,\"use_mmap\":false,\"kvcache_mmap\":false,"
            "\"async\":false,\"timeout_ms\":120000,"
            "\"session_strategy\":\"shared_model_reset_before_each_region\","
            "\"prompt_sha256\":\"de9617f877f6110d22adf1a6ba2a96221189dc246fb1fef161e408d37bff5267\"}";
    }
    out += ",\"processing\":[";
    for (size_t i = 0; i < plan.processing.size(); ++i) {
        const auto& step = plan.processing[i];
        std::string status, reason;
        if (!step.enabled) { status = "skipped_disabled"; reason = "optional_disabled"; }
        else if (step.id == "decode") {
            status = result.did_decode ? "executed" : result.code == dococr::RunCode::InputError ? "failed" : "not_run";
            reason = result.did_decode ? "image_decoded" : "decode_unavailable";
        }
        else if (step.id == "normalize") {
            status = step.owner == "adapter" ? (result.did_normalize ? "executed" : "not_run") :
                     step.owner == "runtime" ? (result.did_layout ? "delegated_runtime" : "not_run") :
                     (result.did_layout ? "provided_by_graph" : "not_run");
            reason = step.owner == "adapter" ? (plan.backend.rfind("mnn:pp-doclayout-v3", 0) == 0 ? "torchvision_uint8_bicubic_800_rgb_nchw" : "rgb8_to_float32_0_1") :
                     step.owner == "runtime" ? "fixture_runtime_normalized" : "fixture_graph_normalized";
        }
        else if (step.id == "crop") {
            status = result.did_crop ? "executed" : result.did_layout ? "identity_validated" : "not_run";
            reason = result.did_crop ? "region_crop" : result.did_layout ? "no_regions" : "layout_not_completed";
        }
        else if (step.id == "session_reset") {
            status = result.reset_failed ? "failed" : result.did_reset ? "delegated_runtime" :
                     result.did_layout ? "identity_validated" : "not_run";
            reason = result.reset_failed ? "region_reset_failed" : result.did_reset ?
                     (plan.backend == "mnn:pp-doclayout-v3+ovisocr2" ? "ovis_same_instance_reset" : "fixture_reset_succeeded") :
                     result.did_layout ? "no_recognition_regions" : "layout_not_completed";
        }
        else {
            status = result.did_decode ? "identity_validated" : "not_run";
            reason = result.did_decode ? "owned_rgb8_source" : "decode_not_completed";
        }
        if (i) out += ',';
        out += "{\"id\":" + dococr::json_quote(step.id) + ",\"owner\":" + dococr::json_quote(step.owner) +
            ",\"status\":" + dococr::json_quote(status) + ",\"reason\":" + dococr::json_quote(reason) + "}";
    }
    out += "],\"regions\":[";
    for (size_t i = 0; i < result.regions.size(); ++i) {
        const auto& region = result.regions[i];
        if (i) out += ',';
        out += "{\"request_id\":" + dococr::json_quote(region.request_id) +
            ",\"status\":" + dococr::json_quote(region.status) +
            ",\"stop_reason\":" + dococr::json_quote(region.stop_reason) +
            ",\"elapsed_ms\":" + std::to_string(region.elapsed_ms) + "}";
    }
    out += "],\"timings_ms\":{\"decode\":" + std::to_string(result.decode_ms) +
        ",\"layout\":" + std::to_string(result.layout_ms) +
        ",\"recognition\":" + std::to_string(result.recognition_ms) +
        ",\"export\":" + std::to_string(result.export_ms) +
        "},\"timing_status\":{\"decode\":" + dococr::json_quote(result.did_decode ? "measured" : "not_run") +
        ",\"layout\":" + dococr::json_quote(result.did_layout || result.layout_attempted ? "measured" : "not_run") +
        ",\"recognition\":" + dococr::json_quote(result.did_recognition ? "measured" : "not_run") +
        ",\"export\":" + dococr::json_quote(result.did_export ? "measured" : "not_run") +
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
        if (value == "mnn:pp-doclayout-v3" || value == "mnn:pp-doclayout-v3+ovisocr2") {
            last_error = "{\"code\":\"configuration_required\",\"detail\":\"请通过 --config 指定已校验的版面模型工件\"}";
            return DOCOCR_CONFIG_ERROR;
        }
        if (!dococr::config_supported(value)) {
            plan = checked_plan(value);
            if (!plan) return DOCOCR_CONFIG_ERROR;
        }
        const std::string backend_name = plan ? plan->backend : value;
        if (!dococr::config_supported(backend_name)) {
            last_error = "{\"code\":\"unsupported_backend\",\"detail\":" +
                dococr::json_quote(backend_name) + "}";
            return DOCOCR_CONFIG_ERROR;
        }
        auto engine = std::make_shared<Engine>();
        engine->config = backend_name;
        engine->plan = std::move(plan);
        engine->backend = dococr::make_backend(backend_name);
        if (engine->plan && (!engine->backend || !engine->backend->capabilities().tensor ||
            (!engine->plan->layout_only && !engine->backend->capabilities().generation) ||
            engine->backend->capabilities().max_concurrent_requests != 1)) {
            last_error = "{\"code\":\"missing_capability\",\"detail\":\"所需推理后端或能力不可用\"}";
            return DOCOCR_CONFIG_ERROR;
        }
        dococr::BackendLoadSpec load_spec{backend_name, engine->plan ? engine->plan->config_hash : "",
            engine->plan ? engine->plan->device : "cpu", engine->plan ? engine->plan->artifacts :
            std::vector<dococr::ArtifactInfo>{}};
        if (engine->backend) {
            bool loaded = false;
            const bool pair = backend_name == "mnn:pp-doclayout-v3+ovisocr2";
            try { loaded = engine->backend->load(load_spec); }
            catch (const std::exception& error) {
                last_error = pair ?
                    "{\"code\":\"model_load_exception\",\"detail\":\"模型加载异常\","
                    "\"stage\":\"model_initialization\"}" :
                    "{\"code\":\"layout_model_load_exception\",\"detail\":" +
                    dococr::json_quote("版面模型加载异常：" + std::string(error.what())) +
                    ",\"stage\":\"layout_initialization\"}";
                return DOCOCR_FAILED;
            }
            if (!loaded) {
                last_error = "{\"code\":" + dococr::json_quote(engine->backend->last_error()) +
                    (pair ? ",\"stage\":\"model_initialization\"}" :
                            ",\"stage\":\"layout_initialization\"}");
                return DOCOCR_FAILED;
            }
        }
        if (engine->plan && !same_artifacts(engine->plan->artifacts, engine->backend->loaded_artifacts())) {
            last_error = "{\"code\":\"artifact_binding_mismatch\"}";
            return DOCOCR_CONFIG_ERROR;
        }
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
        auto plan = checked_plan(std::string(config.data, config.size));
        if (!plan) return DOCOCR_CONFIG_ERROR;
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = engines.find(handle);
        if (it == engines.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->config != plan->backend) {
            last_error = "{\"code\":\"backend_change_requires_new_engine\"}";
            return DOCOCR_CONFIG_ERROR;
        }
        if (!it->second->plan || !same_artifacts(it->second->plan->artifacts, plan->artifacts)) {
            last_error = "{\"code\":\"artifact_change_requires_new_engine\"}";
            return DOCOCR_CONFIG_ERROR;
        }
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
        if (!input || input->struct_size < offsetof(DocOcrInput, first_page)) return DOCOCR_INVALID_ARGUMENT;
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
            if (!engine->ready) return DOCOCR_FAILED;
            job->started = true;
            job->running = true;
            engine->running = true;
            job->state = "running";
            try { push_event(*job, "started"); } catch (...) { ++job->events_dropped; }
        }
        const bool has_timeout = input->struct_size >= offsetof(DocOcrInput, timeout_ms) + sizeof(input->timeout_ms);
        const uint32_t timeout_ms = has_timeout ? input->timeout_ms : 0;
        std::thread watcher;
        RunCleanup cleanup{*job, *engine, watcher};
        bool watchdog_ready = true;
        try {
            if (timeout_ms) watcher = std::thread([job, timeout_ms] {
                std::unique_lock<std::mutex> lock(registry_mutex);
                if (!job->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [&] { return job->execution_done; })) {
                    job->timed_out = true;
                    job->cancelled = true;
                    job->state = "cancelling";
                    try { push_event(*job, "timeout_requested", job->page_current); } catch (...) {}
                }
            });
        } catch (...) { watchdog_ready = false; }
        dococr::RunResult result{dococr::RunCode::Failed, {}};
        std::string pdf_manifest;
        auto progress = [job](const char* kind, uint32_t page, const std::string& request_id,
                              uint32_t done, uint32_t total) {
            std::lock_guard<std::mutex> lock(registry_mutex);
            job->page_current = page;
            if (std::strcmp(kind, "page_completed") == 0) job->page_completed = done;
            if (std::strcmp(kind, "page_started") == 0 || std::strcmp(kind, "page_completed") == 0)
                job->page_total = total;
            if (std::strcmp(kind, "region_started") == 0 || std::strcmp(kind, "region_completed") == 0) {
                job->region_completed = done; job->region_total = total;
            }
            try { push_event(*job, kind, page, request_id); } catch (...) { ++job->events_dropped; }
        };
        try {
            if (!watchdog_ready) {
                result.code = dococr::RunCode::Failed;
                result.error_code = "timeout_monitor_unavailable";
                result.error_message = "无法启动作业截止时间监视器";
            } else {
                dococr::InputView view{input->data, input->size, input->format,
                                       input->width, input->height, input->row_stride};
                if (input->format == DOCOCR_DOCUMENT_PDF) {
                    auto field = [&](size_t offset, size_t size) { return input->struct_size >= offset + size; };
                    uint32_t first = field(offsetof(DocOcrInput, first_page), sizeof(input->first_page)) ? input->first_page : 0;
                    uint32_t last = field(offsetof(DocOcrInput, last_page), sizeof(input->last_page)) ? input->last_page : 0;
                    uint32_t dpi = field(offsetof(DocOcrInput, dpi), sizeof(input->dpi)) ? input->dpi : 0;
                    uint64_t pixels = field(offsetof(DocOcrInput, max_page_pixels), sizeof(input->max_page_pixels)) ?
                                      input->max_page_pixels : 0;
                    auto pdf = dococr::run_pdf(engine->backend.get(), view, first, last, dpi ? dpi : 150,
                                               pixels, job->cancelled, job->plan.get(), progress);
                    result = std::move(pdf.run);
                    pdf_manifest = std::move(pdf.manifest_pdf);
                } else {
                    progress("page_started", 1, "", 0, 1);
                    result = dococr::run_page(engine->backend.get(), view, job->cancelled,
                                              job->plan.get(), 0, progress);
                    if (result.code != dococr::RunCode::Cancelled && !job->cancelled)
                        progress("page_completed", 1, "", 1, 1);
                }
            }
        } catch (const std::bad_alloc&) {
            result.code = dococr::RunCode::Failed;
            result.error_code = "out_of_memory";
        } catch (const std::exception& error) {
            result.code = dococr::RunCode::Failed;
            result.error_code = "job_exception";
            try { result.error_message = error.what(); } catch (...) {}
        } catch (...) {
            result.code = dococr::RunCode::Failed;
            result.error_code = "job_exception";
        }
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            job->execution_done = true;
            job->cv.notify_all();
        }
        if (watcher.joinable()) watcher.join();
        if (job->cancelled && (result.code == dococr::RunCode::Ok ||
            result.code == dococr::RunCode::Partial || result.code == dococr::RunCode::Blank)) {
            result.code = dococr::RunCode::Cancelled;
            result.error_code = job->timed_out ? "timeout" : "cancelled";
        }
        std::string executed_profile;
        try { if (engine->backend) executed_profile = engine->backend->profile(); }
        catch (...) { executed_profile = "profile_unavailable"; }
        const bool needs_rebuild = engine->backend && (result.code == dococr::RunCode::Cancelled ||
            result.code == dococr::RunCode::Failed || result.code == dococr::RunCode::Partial ||
            result.reset_failed);
        if (needs_rebuild) {
            std::lock_guard<std::mutex> lock(registry_mutex);
            job->state = "recovering";
            try { push_event(*job, "recovery_started", job->page_current); } catch (...) { ++job->events_dropped; }
        }
        if (needs_rebuild) {
            bool recovered = false;
            std::string recovery_error = "backend_rebuild_failed";
            try {
                engine->backend->unload();
                engine->backend.reset();
                auto replacement = dococr::make_backend(engine->config);
                dococr::BackendLoadSpec spec{engine->config,
                    job->plan ? job->plan->config_hash : "",
                    job->plan ? job->plan->device : "cpu",
                    job->plan ? job->plan->artifacts : std::vector<dococr::ArtifactInfo>{}};
                if (replacement && replacement->load(spec) &&
                    (!job->plan || same_artifacts(job->plan->artifacts, replacement->loaded_artifacts()))) {
                    engine->backend = std::move(replacement);
                    recovered = true;
                } else if (replacement && !replacement->last_error().empty())
                    recovery_error = replacement->last_error();
            } catch (const std::bad_alloc&) { recovery_error = "out_of_memory"; }
              catch (...) { recovery_error = "backend_rebuild_exception"; }
            std::lock_guard<std::mutex> lock(registry_mutex);
            engine->ready = recovered;
            engine->recovery_error = recovered ? "" : recovery_error;
            try { push_event(*job, recovered ? "recovery_completed" : "recovery_failed",
                             job->page_current); } catch (...) { ++job->events_dropped; }
        }
        std::lock_guard<std::mutex> lock(registry_mutex);
        DocOcrStatus status = DOCOCR_OK;
        try {
            if (input->format == DOCOCR_DOCUMENT_PDF) job->error_request_id = "document";
            if (job->plan)
                job->manifest = manifest_json(*job->plan, result,
                    engine->backend && engine->ready ? engine->backend->profile() : executed_profile);
            if (!pdf_manifest.empty() && job->manifest.empty()) {
                std::string manifest_status = result.code == dococr::RunCode::Ok ? "ok" :
                    result.code == dococr::RunCode::Blank ? "blank" :
                    result.code == dococr::RunCode::Partial ? "partial" :
                    result.code == dococr::RunCode::Cancelled ? "cancelled" :
                    result.code == dococr::RunCode::BudgetExceeded ? "budget_exceeded" : "failed";
                job->manifest = "{\"schema_version\":\"1.1\",\"job_status\":" +
                    dococr::json_quote(manifest_status) + ",\"pdf\":" + pdf_manifest + "}";
            }
            else if (!pdf_manifest.empty() && !job->manifest.empty()) {
                job->manifest.pop_back();
                job->manifest += ",\"pdf\":" + pdf_manifest + "}";
            }
            switch (result.code) {
            case dococr::RunCode::Ok: job->state = "completed"; break;
            case dococr::RunCode::Partial: job->state = "partial"; break;
            case dococr::RunCode::Blank: job->state = "blank"; break;
            case dococr::RunCode::InputError:
                job->state = "failed"; job->error_stage = input->format == DOCOCR_DOCUMENT_PDF ? "pdf" : "decode";
                job->error_code = result.error_code.empty() ? "input_error" : result.error_code;
                job->error_message = result.error_message.empty() ? "invalid input" : result.error_message;
                status = DOCOCR_INPUT_ERROR; break;
            case dococr::RunCode::Unsupported:
                job->state = "failed"; job->error_stage = input->format == DOCOCR_DOCUMENT_PDF ? "pdf" :
                    job->plan && job->plan->layout_only ? "layout" : "inference";
                job->error_code = result.error_code.empty() ? "unsupported_backend" : result.error_code;
                job->error_message = result.error_message.empty() ? "no production model backend configured" : result.error_message;
                status = DOCOCR_UNSUPPORTED; break;
            case dococr::RunCode::Cancelled:
                job->state = job->timed_out ? "timed_out" : "cancelled";
                job->error_stage = input->format == DOCOCR_DOCUMENT_PDF ? "pdf" : "inference";
                job->error_code = job->timed_out ? "timeout" : "cancelled";
                job->error_message = job->timed_out ? "job deadline exceeded" : "job cancelled";
                status = job->timed_out ? DOCOCR_TIMEOUT : DOCOCR_CANCELLED; break;
            case dococr::RunCode::Failed:
                job->state = "failed";
                job->error_stage = input->format == DOCOCR_DOCUMENT_PDF ? "pdf" :
                    result.error_code.rfind("layout_", 0) == 0 ||
                    (job->plan && job->plan->layout_only) ? "layout" : "inference";
                job->error_code = result.error_code.empty() ? "inference_error" : result.error_code;
                job->error_message = result.error_message.empty() ? "inference contract failed" : result.error_message;
                status = DOCOCR_FAILED; break;
            case dococr::RunCode::BudgetExceeded:
                job->state = "failed"; job->error_stage = "budget";
                job->error_code = input->format == DOCOCR_DOCUMENT_PDF && !result.error_code.empty() ?
                    result.error_code : "budget_exceeded";
                job->error_message = input->format == DOCOCR_DOCUMENT_PDF && !result.error_message.empty() ?
                    result.error_message : result.budget_stage;
                status = DOCOCR_BUDGET_EXCEEDED; break;
            }
            if (status == DOCOCR_OK) {
                job->output = std::move(result.output);
                job->has_result = true;
            }
            if (job->timed_out && !job->manifest.empty()) {
                job->manifest.pop_back();
                job->manifest += ",\"control_status\":\"timed_out\"}";
            }
            if (!engine->ready && !job->manifest.empty()) {
                job->manifest.pop_back();
                job->manifest += ",\"recovery\":{\"status\":\"failed\",\"error\":" +
                    dococr::json_quote(engine->recovery_error) + "}}";
            }
        } catch (const std::bad_alloc&) {
            status = DOCOCR_FAILED;
            job->state = "failed";
            job->error_code = "out_of_memory";
            job->error_stage = "finalization";
            job->has_result = false;
            engine->ready = false;
            engine->recovery_error = "oom";
        } catch (...) {
            status = DOCOCR_FAILED;
            job->state = "failed";
            job->error_code = "finalization_failed";
            job->error_stage = "finalization";
            job->has_result = false;
            engine->ready = false;
            engine->recovery_error = "failed";
        }
        try { push_event(*job, "terminal", job->page_current); } catch (...) { ++job->events_dropped; }
        job->running = false;
        engine->running = false;
        job->cv.notify_all();
        cleanup.committed = true;
        return status;
    });
}

DocOcrStatus dococr_job_manifest(DocOcrJob handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->manifest.empty()) return DOCOCR_NO_RESULT;
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
        if (it->second->started && !it->second->running) return DOCOCR_INVALID_ARGUMENT;
        if (it->second->execution_done) return DOCOCR_BUSY;
        it->second->cancelled = true;
        if (it->second->running) {
            it->second->state = "cancelling";
            try { push_event(*it->second, "cancel_requested", it->second->page_current); }
            catch (...) { ++it->second->events_dropped; }
        }
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_status(DocOcrJob handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        auto engine = engines.find(it->second->engine);
        if (engine == engines.end()) return DOCOCR_INVALID_HANDLE;
        std::string content = status_json(*it->second, *engine->second);
        *out_json = copy_bytes(content.data(), content.size());
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_next_event(DocOcrJob handle, DocOcrBytes* out_json) {
    return guarded([&] {
        if (!out_json) return DOCOCR_INVALID_ARGUMENT;
        *out_json = {};
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        if (it->second->events.empty()) return DOCOCR_NO_RESULT;
        const std::string& content = it->second->events.front();
        *out_json = copy_bytes(content.data(), content.size());
        it->second->events.pop_front();
        return DOCOCR_OK;
    });
}

DocOcrStatus dococr_job_wait(DocOcrJob handle, uint32_t timeout_ms) {
    return guarded([&] {
        std::unique_lock<std::mutex> lock(registry_mutex);
        auto it = jobs.find(handle);
        if (it == jobs.end()) return DOCOCR_INVALID_HANDLE;
        auto job = it->second;
        if (!job->started) return DOCOCR_NO_RESULT;
        if (!job->running) return DOCOCR_OK;
        if (!timeout_ms) return DOCOCR_BUSY;
        return job->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return !job->running; }) ? DOCOCR_OK : DOCOCR_BUSY;
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
