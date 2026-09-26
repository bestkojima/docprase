#include "backend_factory.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
namespace dococr {
class FixtureBackend final : public IInferenceEngine {
public:
    explicit FixtureBackend(std::string scenario) : scenario_(std::move(scenario)) {}
    bool load() override { return true; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override { return "test_fixture"; }
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        if (auto* layout = std::get_if<TensorRequest>(&request.payload)) {
            if (scenario_ == "slow") {
                for (int i = 0; i < 50 && !context.cancelled; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const Tensor& input = layout->inputs.at(0);
            if (scenario_ == "normalized") {
                if (input.name != "page_rgb_normalized" || input.dtype != DataType::Float32 ||
                    input.data.size() != size_t(input.shape.at(0) * input.shape.at(1) * 3) * sizeof(float))
                    throw std::runtime_error("normalized tensor contract mismatch");
                float red = 0;
                std::memcpy(&red, input.data.data(), sizeof(float));
                if (red < 0.99f || red > 1.01f)
                    throw std::runtime_error("normalized pixel value mismatch");
            }
            if (scenario_ == "runtime" || scenario_ == "graph") {
                if (input.name != "page_rgb" || input.dtype != DataType::UInt8 || input.data.empty())
                    throw std::runtime_error("raw tensor contract mismatch");
                float normalized_red = float(input.data[0]) / 255.0f;
                if (normalized_red < 0.99f || normalized_red > 1.01f)
                    throw std::runtime_error("provider normalize mismatch");
            }
            int w = int(input.shape.at(1)), h = int(input.shape.at(0));
            std::vector<float> rows;
            if (scenario_ == "failure" || scenario_ == "reset_failure" ||
                scenario_ == "generation_exception" || scenario_ == "wrong_response") rows = {
                0, .8f, 0, 0, float(w), float(h/2), 7,
                0, .8f, 0, float(h/2), float(w), float(h), 7};
            else if (scenario_ == "unknown") rows = {
                99, .8f, 0, 0, float(w), float(h/2), 7,
                0, .9f, 0, float(h/2), float(w), float(h), 7};
            else if (scenario_ == "formula_table") rows = {
                2, .8f, 0, float(h/2), float(w), float(h), 7,
                1, .9f, 0, 0, float(w), float(h/2), 7};
            else if (scenario_ != "blank") rows = {
                3, .7f, 0, float(h/2), float(w), float(h), 7,
                0, .9f, 0, 0, float(w), float(h/2), 7};
            Tensor result{"layout_candidates", DataType::Float32, TensorLayout::Matrix,
                          {int64_t(rows.size()/7), 7}, std::vector<uint8_t>(rows.size()*sizeof(float))};
            if (!rows.empty()) std::memcpy(result.data.data(), rows.data(), result.data.size());
            return {TensorOutput{{std::move(result)}}};
        }
        const auto& generation = std::get<GenerationRequest>(request.payload);
        auto bounded = [&](std::string value) -> InferenceResponse {
            size_t pos = 0;
            for (uint64_t token = 0; token < generation.max_new_tokens && pos < value.size(); ++token) {
                unsigned char lead = static_cast<unsigned char>(value[pos]);
                pos += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            }
            if (pos < value.size())
                return {GenerationOutput{value.substr(0, pos), value, "truncated", "fixture token budget"}};
            return {GenerationOutput{value, value, "complete", ""}};
        };
        if (scenario_ == "runtime") return bounded("运行时归一化");
        if (scenario_ == "graph") return bounded("图内归一化");
        if (scenario_ == "failure" && generation.source_box.y0 == 0)
            return {GenerationOutput{"", "partial raw output", "failed", "fixture controlled failure"}};
        if (scenario_ == "generation_exception" && generation.source_box.y0 == 0)
            throw std::runtime_error("controlled region exception");
        if (scenario_ == "wrong_response" && generation.source_box.y0 == 0)
            return {TensorOutput{}};
        if (scenario_ == "invalid_utf8")
            return {GenerationOutput{std::string("\xff", 1), std::string("\xfe", 1),
                                     "failed", std::string("\xfd", 1)}};
        if (generation.task == "formula")
            return bounded("x^2+1");
        if (generation.task == "table")
            return bounded("<table><tr><td>甲</td></tr></table>");
        return bounded("测试文字");
    }
    bool reset() override { return scenario_ != "reset_failure"; }
    void unload() override {}
private:
    EngineCapabilities capabilities_{true, true, true, 1};
    std::string scenario_;
};
bool config_supported(const std::string& config) {
    return config == "fixture:normalized" || config == "fixture:runtime" ||
           config == "fixture:graph" || config == "fixture:sample" || config == "fixture:blank" ||
           config == "fixture:failure" || config == "fixture:formula_table" ||
           config == "fixture:slow" || config == "fixture:invalid_utf8" ||
           config == "fixture:unknown" || config == "fixture:reset_failure" ||
           config == "fixture:generation_exception" || config == "fixture:wrong_response";
}
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config) {
    return std::make_unique<FixtureBackend>(config.substr(8));
}
std::string backend_capabilities(const std::string&) {
    return "{\"schema_version\":\"1.0\",\"backend\":\"test_fixture\",\"model_available\":false,\"tasks\":[\"layout\",\"text\",\"formula\",\"table\"]}";
}
}
