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
enum class DataType { UInt8, Float32 };
enum class TensorLayout { HWC, Matrix };
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
};
struct InferenceRequest { std::string request_id; std::variant<TensorRequest, GenerationRequest> payload; };
struct TensorOutput { std::vector<Tensor> outputs; };
struct GenerationOutput {
    std::string text;
    std::string raw_output;
    std::string finish_reason; // complete, truncated, failed
    std::string error;
};
struct InferenceResponse { std::variant<TensorOutput, GenerationOutput> payload; };
struct ExecutionContext { std::atomic_bool& cancelled; };
struct EngineCapabilities {
    bool tensor = false;
    bool generation = false;
    bool isolated_sessions = false;
    int max_concurrent_requests = 1;
};
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;
    virtual bool load() = 0;
    virtual const EngineCapabilities& capabilities() const = 0;
    virtual std::string profile() const = 0;
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
enum class RunCode { Ok, Partial, Blank, InputError, Unsupported, Failed, Cancelled };
struct RunResult { RunCode code; JobOutput output; };
struct InputView {
    const uint8_t* data;
    size_t size;
    uint32_t format, width, height;
    size_t row_stride;
};
RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled);
} // namespace dococr
#endif
