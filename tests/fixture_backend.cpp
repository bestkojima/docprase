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
            return {GenerationOutput{"x^2+1", "x^2+1", "complete", ""}};
        if (generation.task == "table")
            return {GenerationOutput{"<table><tr><td>甲</td></tr></table>", "<table><tr><td>甲</td></tr></table>", "complete", ""}};
        return {GenerationOutput{"测试文字", "测试文字", "complete", ""}};
    }
    bool reset() override { return scenario_ != "reset_failure"; }
    void unload() override {}
private:
    EngineCapabilities capabilities_{true, true, true, 1};
    std::string scenario_;
};
bool config_supported(const std::string& config) {
    return config == "fixture:sample" || config == "fixture:blank" ||
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
