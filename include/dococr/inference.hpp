#ifndef DOCOCR_INFERENCE_HPP
#define DOCOCR_INFERENCE_HPP
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dococr {
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
};
struct Box { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
enum class DataType { UInt8, Float32, Int32 };
enum class TensorLayout { HWC, Matrix, NCHW };
struct Tensor {
    std::string name;
    DataType dtype;
    TensorLayout layout;
    std::vector<int64_t> shape;
    std::vector<uint8_t> data; // owned host bytes; size must match dtype and shape
};
struct TensorRequest {
    std::vector<Tensor> inputs;
    std::vector<std::string> requested_outputs;
};
struct GenerationRequest {
    Image image;
    Box source_box;
    std::string task; // text, formula, table
    std::string request_id;
    uint64_t max_new_tokens = 4096;
};
struct InferenceRequest { std::string request_id; std::variant<TensorRequest, GenerationRequest> payload; };
struct TensorOutput { std::vector<Tensor> outputs; };
struct GenerationOutput {
    std::string text;
    std::string raw_output;
    std::string finish_reason; // complete, truncated, failed
    std::string error;
    std::string stop_reason;
    uint64_t elapsed_ms = 0;
};
struct InferenceResponse { std::variant<TensorOutput, GenerationOutput> payload; };
struct ExecutionContext { std::atomic_bool& cancelled; };
struct EngineCapabilities {
    bool tensor = false;
    bool generation = false;
    bool isolated_sessions = false;
    int max_concurrent_requests = 1;
};
struct ArtifactInfo { std::string model, path, sha256, contract_status; };
struct BackendLoadSpec {
    std::string backend_id, config_hash, device;
    std::vector<ArtifactInfo> artifacts;
};
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;
    virtual bool load(const BackendLoadSpec&) = 0;
    virtual std::vector<ArtifactInfo> loaded_artifacts() const = 0;
    virtual const EngineCapabilities& capabilities() const = 0;
    virtual std::string profile() const = 0;
    virtual std::string last_error() const { return {}; }
    virtual InferenceResponse execute(const InferenceRequest&, ExecutionContext&) = 0;
    virtual bool reset() = 0;
    virtual void unload() = 0;
};

struct Asset { std::string name; std::vector<uint8_t> png; };
struct JobOutput {
    std::string json;
    std::string markdown;
    std::vector<Asset> assets;
};
enum class RunCode { Ok, Partial, Blank, InputError, Unsupported, Failed, Cancelled, BudgetExceeded };
struct RunResult {
    RunCode code;
    JobOutput output;
    uint64_t page_pixels = 0;
    bool did_decode = false, did_layout = false, did_normalize = false, did_crop = false,
         did_reset = false, reset_failed = false, did_recognition = false, did_export = false;
    bool layout_attempted = false;
    std::string budget_stage;
    std::string error_code, error_message;
    uint64_t decode_ms = 0, layout_ms = 0, recognition_ms = 0, export_ms = 0;
    struct RegionRun { std::string request_id, status, stop_reason; uint64_t elapsed_ms = 0; };
    std::vector<RegionRun> regions;
};
struct InputView {
    const uint8_t* data;
    size_t size;
    uint32_t format, width, height;
    size_t row_stride;
};
struct ExecutionPlan;
RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled,
                   const ExecutionPlan* plan = nullptr, uint32_t source_page = 0);
} // namespace dococr
#endif
