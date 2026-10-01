#include "backend_factory.hpp"
#include "config.hpp"
#include "visual_adaptation.hpp"
#include "llm/llm.hpp"
#include <MNN/Interpreter.hpp>
#define STB_IMAGE_WRITE_STATIC
#define STBIW_ONLY_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dococr {
std::unique_ptr<IInferenceEngine> make_layout_mnn_backend();
namespace {
using MNN::Transformer::Llm;
using MNN::Transformer::LlmStatus;
const char* prompt = "\nExtract all readable content from the image in natural human reading order and output the result as a single Markdown document. For charts or images, represent them using an HTML image tag: <img src=\"images/bbox_{left}_{top}_{right}_{bottom}.jpg\" />, where left, top, right, bottom are bounding box coordinates scaled to [0, 1000). Format formulas as LaTeX. Format tables as HTML: <table>...</table>. Transcribe all other text as standard Markdown. Preserve the original text without translation or paraphrasing.";
constexpr int image_pad_token = 248056; // pinned models/ovis/llm_config.json
const std::vector<std::pair<std::string, std::string>> ovis_hashes = {
    {"config.json", "b81ac7008ba5f894301b7b9265bba882889df52c6e25c86390514c1bd4afe0c4"},
    {"llm_config.json", "bb0d93883767c2c47de7f6965494c689c9890fffb79e3ca5e00e6f1fa5fd9770"},
    {"llm.mnn", "1da84439dec62e4f966833c54438859e1bdae8a07b03a55c643aabbeb48bc880"},
    {"llm.mnn.weight", "f09832b6ee9d63167ef456e83dea7f5df28e3983953e28b0a1e63e598cb45d32"},
    {"visual.mnn", "d85dbe1c24890bdd514cf35f23d2c0dfdb3d207489f50c05705782759881daa5"},
    {"visual.mnn.weight", "2a1b5138bdc1f6369df64f2b21b379b11f47ec51a46bb74a5013e4b2b9c4aa6d"},
    {"tokenizer.mtok", "1a0c1ee1d04a63791ea87be31bb8c1f6346ecdd9c3b55ccdbb894cea32fbedfa"},
    {"export_args.json", "953745b0456e0e4e3d2d4a0b31dd5a2d4dce632230a0b1d7c0ccfeead2de9334"}
};
class TempFile {
public:
    explicit TempFile(const char* prefix, const char* extension) {
        namespace fs = std::filesystem;
#ifdef _WIN32
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + std::to_string(random()) + extension);
            HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                path_ = candidate;
                utf8_path_ = path_.u8string();
                return;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                break;
        }
#else
        std::string name = (fs::temp_directory_path() / fs::u8path(std::string(prefix) +
                            "XXXXXX" + extension)).u8string();
        std::vector<char> chars(name.begin(), name.end()); chars.push_back(0);
        int suffix = static_cast<int>(std::strlen(extension));
        int fd = mkstemps(chars.data(), suffix);
        if (fd >= 0) {
            path_ = fs::u8path(chars.data());
            utf8_path_ = path_.u8string();
            close(fd);
            return;
        }
#endif
        throw std::runtime_error("temporary_file_create_failed");
    }
    ~TempFile() { if (!path_.empty()) { std::error_code ec; std::filesystem::remove(path_, ec); } }
    const std::string& path() const { return utf8_path_; }
    void write(const std::string& data) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out) throw std::runtime_error("temporary_file_write_failed");
    }
    void write(const std::vector<uint8_t>& data) const {
        write(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    }
private:
    std::filesystem::path path_;
    std::string utf8_path_;
};
void png_write(void* opaque, void* bytes, int size) {
    auto& data = *static_cast<std::vector<uint8_t>*>(opaque);
    auto* first = static_cast<uint8_t*>(bytes);
    data.insert(data.end(), first, first + size);
}
std::vector<uint8_t> png(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.rgb.size() != size_t(image.width) * image.height * 3)
        throw std::runtime_error("region_image_contract_mismatch");
    std::vector<uint8_t> bytes;
    if (!stbi_write_png_to_func(png_write, &bytes, image.width, image.height, 3,
                                image.rgb.data(), image.width * 3))
        throw std::runtime_error("region_png_encode_failed");
    return bytes;
}
class PrintedPageMnnBackend final : public IInferenceEngine {
public:
    bool load(const BackendLoadSpec& spec) override {
        unload(); error_.clear();
        if (sha256(prompt) != "de9617f877f6110d22adf1a6ba2a96221189dc246fb1fef161e408d37bff5267") {
            error_ = "ovis_prompt_contract_mismatch"; return false;
        }
        if (spec.backend_id != "mnn:pp-doclayout-v3+ovisocr2" || spec.device != "cpu" ||
            spec.artifacts.size() != ovis_hashes.size() + 1) {
            error_ = "printed_page_artifact_contract_mismatch"; return false;
        }
        std::vector<ArtifactInfo> layout_artifacts;
        std::filesystem::path root;
        std::set<std::string> seen_ovis;
        for (const auto& artifact : spec.artifacts) {
            if (artifact.contract_status != "contract_verified" ||
                sha256_file(artifact.path) != artifact.sha256) {
                error_ = "printed_page_artifact_contract_mismatch"; return false;
            }
            if (artifact.model == "layout") layout_artifacts.push_back(artifact);
            else if (artifact.model == "recognition") {
                auto filename = std::filesystem::path(artifact.path).filename().string();
                auto known = std::find_if(ovis_hashes.begin(), ovis_hashes.end(),
                    [&](const auto& item) { return item.first == filename && item.second == artifact.sha256; });
                if (known == ovis_hashes.end()) { error_ = "ovis_artifact_contract_mismatch"; return false; }
                if (!seen_ovis.insert(filename).second) {
                    error_ = "ovis_artifact_duplicate"; return false;
                }
                auto parent = std::filesystem::path(artifact.path).parent_path();
                if (root.empty()) root = parent;
                if (root != parent) { error_ = "ovis_artifact_root_mismatch"; return false; }
            } else { error_ = "printed_page_artifact_contract_mismatch"; return false; }
        }
        if (layout_artifacts.size() != 1 || root.empty() || seen_ovis.size() != ovis_hashes.size()) {
            error_ = "printed_page_artifact_contract_mismatch"; return false;
        }
        layout_ = make_layout_mnn_backend();
        BackendLoadSpec layout_spec{"mnn:pp-doclayout-v3", spec.config_hash, spec.device, layout_artifacts};
        if (!layout_->load(layout_spec)) { error_ = layout_->last_error(); unload(); return false; }
        // Pinned model configuration with explicit runtime overrides; each JSON key appears once.
        std::string effective = std::string(R"({"llm_model":"llm.mnn","llm_weight":"llm.mnn.weight",)") +
            R"("backend_type":"cpu","thread_num":1,"precision":"low","memory":"low",)"
            R"("sampler_type":"greedy","temperature":0.8,"top_k":40,"top_p":0.9,)"
            R"("min_p":0.05,"tfs_z":1.0,"typical":0.95,"repetition_penalty":1.0,)"
            R"("presence_penalty":0.0,"frequency_penalty":0.0,"penalty_window":0,)"
            R"("n_gram":8,"ngram_factor":1.0,"tokenizer_file":"tokenizer.mtok",)"
            R"("mllm":{"backend_type":"cpu","thread_num":1,"precision":"normal","memory":"low"},)"
            R"("reuse_kv":false,"prompt_cache":false,"use_mmap":false,"kvcache_mmap":false,"async":false,"timeout_ms":120000,)"
            "\"base_dir\":" + json_quote(root.u8string() + "/") + "}";
        TempFile temporary("dococr-ovis-config-", ".json");
        temporary.write(effective);
        llm_.reset(Llm::createLLM(temporary.path()));
        if (!llm_ || !llm_->load()) { error_ = "ovis_model_load_failed"; unload(); return false; }
        const std::string loaded_config = llm_->dump_config();
        auto count = [&](const std::string& key) {
            size_t result = 0, position = 0;
            while ((position = loaded_config.find(key, position)) != std::string::npos) {
                ++result; position += key.size();
            }
            return result;
        };
        if (count("\"backend_type\":\"cpu\"") != 2 || count("\"thread_num\":1") != 2 ||
            count("\"sampler_type\":\"greedy\"") != 1 || count("\"reuse_kv\":false") != 1 ||
            count("\"prompt_cache\":false") != 1 || count("\"timeout_ms\":120000") != 1) {
            error_ = "ovis_effective_config_mismatch"; unload(); return false;
        }
        runtime_config_hash_ = sha256(loaded_config);
        artifacts_ = spec.artifacts;
        return true;
    }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return artifacts_; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override {
        return "MNN/" + std::string(MNN::getVersion()) +
            "/PP-DocLayoutV3=5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67"
            "/OvisOCR2=f09832b6ee9d63167ef456e83dea7f5df28e3983953e28b0a1e63e598cb45d32"
            "/RuntimeConfig=" + runtime_config_hash_;
    }
    std::string last_error() const override { return error_; }
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<TensorRequest>(request.payload))
            return layout_->execute(request, context);
        auto* generation = std::get_if<GenerationRequest>(&request.payload);
        if (!llm_ || !generation) throw std::runtime_error("ovis_unavailable");
        using Clock = std::chrono::steady_clock;
        auto begin = Clock::now();
        GenerationOutput output;
        if (context.cancelled) {
            output.finish_reason = "failed"; output.stop_reason = "cancelled";
            output.error = "cancelled"; return {output};
        }
        if (generation->max_new_tokens == 0 || generation->max_new_tokens > 4096)
            throw std::runtime_error("ovis_token_budget_unsupported");
        if (generation->generation_timeout_ms == 0 || generation->generation_timeout_ms > 120000)
            throw std::runtime_error("ovis_time_budget_unsupported");
        if (!llm_->set_config("{\"timeout_ms\":" + std::to_string(generation->generation_timeout_ms) + "}"))
            throw std::runtime_error("ovis_time_budget_config_failed");
        AdaptedVisual adapted;
        try {
            adapted = adapt_visual(generation->image, {generation->visual_min_pixels, generation->visual_max_pixels});
        } catch (const std::exception& error) {
            output.finish_reason = "failed"; output.stop_reason = "visual_adaptation_failed";
            output.error = error.what(); output.visual_evidence = "adaptation_failed";
            return {output};
        }
        output.visual_transform = adapted.transform;
        TempFile image("dococr-ovis-region-", ".png");
        image.write(png(adapted.canvas));
        // Omni::tokenizer_encode runs visual forward and returns image-pad IDs.
        // Reject an empty visual result before the language model can generate.
        const std::string user_content = "<img>" + image.path() + "</img>" + prompt;
        std::string model_prompt = llm_->apply_chat_template(user_content);
        if (model_prompt.empty()) model_prompt = user_content;
        const auto tokens = llm_->tokenizer_encode(model_prompt);
        output.visual_tokens = uint32_t(std::count(tokens.begin(), tokens.end(), image_pad_token));
        if (output.visual_tokens == 0) {
            output.finish_reason = "failed"; output.stop_reason = "vision_missing";
            output.error = "ovis_visual_tokens_missing";
            output.visual_evidence = "no_visual_tokens";
            return {output};
        }
        output.visual_evidence = "image_pad_tokens";
        if (context.cancelled) {
            output.finish_reason = "failed"; output.stop_reason = "cancelled";
            output.error = "cancelled"; return {output};
        }
        std::ostringstream raw;
        const auto generation_start = Clock::now();
        llm_->response(tokens, &raw, nullptr, int(generation->max_new_tokens));
        output.generation_elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-generation_start).count());
        output.raw_output = raw.str();
        output.elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count());
        const auto* state = llm_->getContext();
        if (!state) {
            output.finish_reason = "failed"; output.stop_reason = "vision_missing";
            output.error = "ovis_visual_processing_missing";
        } else if (state->status == LlmStatus::NORMAL_FINISHED && !output.raw_output.empty()) {
            output.finish_reason = "complete"; output.stop_reason = "normal";
            output.text = output.raw_output;
        } else if (state->status == LlmStatus::NORMAL_FINISHED) {
            output.finish_reason = "failed"; output.stop_reason = "empty_output";
            output.error = "ovis_empty_output";
        } else if (state->status == LlmStatus::MAX_TOKENS_FINISHED) {
            output.finish_reason = "truncated"; output.stop_reason = "token_limit";
            output.text = output.raw_output; output.error = "ovis_token_limit";
        } else {
            output.finish_reason = "failed";
            output.stop_reason = state->status == LlmStatus::TIMEOUT ? "timeout" :
                state->status == LlmStatus::USER_CANCEL ? "cancelled" : "error";
            output.error = "ovis_runtime_" + output.stop_reason;
        }
        return {output};
    }
    bool reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!llm_) return false;
        llm_->reset();
        return true;
    }
    void unload() override {
        artifacts_.clear(); runtime_config_hash_.clear(); llm_.reset();
        if (layout_) layout_->unload(); layout_.reset();
    }
private:
    EngineCapabilities capabilities_{true, true, true, 1};
    std::unique_ptr<IInferenceEngine> layout_;
    std::unique_ptr<Llm, void(*)(Llm*)> llm_{nullptr, Llm::destroy};
    std::vector<ArtifactInfo> artifacts_;
    std::string error_;
    std::string runtime_config_hash_;
    std::mutex mutex_;
};
}
std::unique_ptr<IInferenceEngine> make_printed_page_mnn_backend() {
    return std::make_unique<PrintedPageMnnBackend>();
}
}
