#ifndef DOCOCR_CONFIG_HPP
#define DOCOCR_CONFIG_HPP
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "dococr/inference.hpp"
#include "label_export_policy.hpp"

namespace dococr {
inline constexpr uint64_t max_layout_source_pixels = 64000000;
struct ProcessingStep {
    std::string id, owner;
    bool enabled = true;
};
struct ExecutionPlan {
    std::string config_hash, backend, mode, device, json;
    bool layout_only = false;
    std::string layout_preprocess = "auto";
    double layout_score_threshold = 0.5;
    LabelExportPolicy label_export;
    bool uses_doclayout() const {
        return layout_only || backend == "mnn:pp-doclayout-v3+ovisocr2" ||
               backend.rfind("fixture:printed_page", 0) == 0;
    }
    int threads = 1;
    uint64_t max_page_pixels = 16000000;
    uint64_t max_output_bytes = 1048576;
    uint64_t max_new_tokens = 4096;
    uint64_t generation_timeout_ms = 120000;
    std::vector<ProcessingStep> processing;
    std::vector<ArtifactInfo> artifacts;
};
struct ConfigError : std::exception {
    std::string code, detail;
    ConfigError(std::string c, std::string d) : code(std::move(c)), detail(std::move(d)) {}
    const char* what() const noexcept override { return detail.c_str(); }
};
std::shared_ptr<const ExecutionPlan> build_plan(const std::string& text, bool fixture_build);
std::string sha256(const std::string& bytes);
std::string sha256_file(const std::string& path);
std::string json_quote(const std::string& s);
} // namespace dococr
#endif
