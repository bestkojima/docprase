#ifndef DOCOCR_CONFIG_HPP
#define DOCOCR_CONFIG_HPP
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dococr {
struct ProcessingStep {
    std::string id, owner;
    bool enabled = true;
};
struct ArtifactInfo { std::string model, path, sha256, contract_status; };
struct ExecutionPlan {
    std::string config_hash, backend, mode, device, json;
    int threads = 1;
    uint64_t max_page_pixels = 16000000;
    uint64_t max_output_bytes = 1048576;
    uint64_t max_new_tokens = 4096;
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
