#include "backend_factory.hpp"
#include "config.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
namespace dococr {
class FixtureBackend final : public IInferenceEngine {
public:
    explicit FixtureBackend(std::string scenario) : scenario_(std::move(scenario)) {
        if (scenario_ == "layout_contract" || scenario_ == "layout_table" ||
            scenario_ == "layout_inline_formula" ||
            scenario_ == "layout_empty" ||
            scenario_ == "layout_infer_failure")
            capabilities_.generation = false;
    }
    bool load(const BackendLoadSpec& spec) override {
        if (spec.backend_id != "fixture:" + scenario_) return false;
        loaded_.clear();
        for (const auto& artifact : spec.artifacts) {
            if (artifact.contract_status != "verified_fixture" ||
                sha256_file(artifact.path) != artifact.sha256) return false;
            loaded_.push_back(artifact);
        }
        if (!spec.config_hash.empty() && loaded_.size() !=
            ((scenario_ == "layout_contract" || scenario_ == "layout_table" ||
              scenario_ == "layout_inline_formula" ||
              scenario_ == "layout_empty" ||
              scenario_ == "layout_infer_failure") ? 1u : 2u)) return false;
        return true;
    }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return loaded_; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override {
        if (loaded_.empty()) return "test_fixture";
        std::string profile = "test_fixture";
        for (const auto& artifact : loaded_)
            profile += "/" + artifact.model + "=" + artifact.sha256;
        return profile;
    }
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        if (auto* layout = std::get_if<TensorRequest>(&request.payload)) {
            if (scenario_.rfind("printed_page", 0) == 0 ||
                scenario_ == "layout_contract" || scenario_ == "layout_table" ||
                scenario_ == "layout_inline_formula" ||
                scenario_ == "layout_empty" ||
                scenario_ == "layout_infer_failure") {
                if (scenario_ == "layout_infer_failure")
                    throw std::runtime_error("layout_inference_failed:controlled");
                if (layout->inputs.size() != 3 || layout->requested_outputs !=
                    std::vector<std::string>{"fetch_name_0", "fetch_name_1", "fetch_name_2"} ||
                    layout->inputs[0].name != "image" || layout->inputs[0].dtype != DataType::Float32 ||
                    layout->inputs[0].layout != TensorLayout::NCHW ||
                    layout->inputs[0].shape != std::vector<int64_t>{1,3,800,800} ||
                    layout->inputs[0].data.size() != 3*800*800*sizeof(float))
                    throw std::runtime_error("layout input mismatch");
                float geometry[2];
                std::memcpy(geometry, layout->inputs[1].data.data(), sizeof(geometry));
                if (geometry[0] != 800 || geometry[1] != 800)
                    throw std::runtime_error("layout im_shape mismatch");
                std::memcpy(geometry, layout->inputs[2].data.data(), sizeof(geometry));
                if (geometry[0] != 400 || geometry[1] != 400)
                    throw std::runtime_error("layout scale_factor mismatch");
                std::vector<float> rows(300*7);
                const float samples[][7] = {
                    {22,.9f,-1,0,2,2,7}, {22,.8f,0,0,1,1,7},
                    {99,.85f,1,1,2,2,9}, {21,.2f,0,0,2,2,10},
                    {14,.9f,3,0,4,1,11}, {5,.9f,1,1,1,2,12},
                    {22,.9f,-1e30f,0,1,1,13}};
                if (scenario_.rfind("printed_page", 0) == 0) {
                    const float page_samples[][7] = {
                        {22,.9f,0,0,2,1,0}, {22,.9f,0,1,1,2,1},
                        {5,.9f,1,1,2,2,2}, {21,.9f,1,0,2,1,3},
                        {99,.9f,1,1,2,2,4}, {14,.9f,0,0,1,1,5}};
                    std::memcpy(rows.data(), page_samples, sizeof(page_samples));
                    if (scenario_.rfind("printed_page_formula", 0) == 0) {
                        const float formula_samples[][7] = {
                            {22,.9f,0,0,2,1,0}, {5,.9f,1,0,2,1,1},
                            {5,.9f,0,1,1,2,2}, {16,.9f,1,1,2,2,3}};
                        std::memcpy(rows.data(), formula_samples, sizeof(formula_samples));
                    }
                    if (scenario_.rfind("printed_page_table", 0) == 0) {
                        const float table_samples[][7] = {
                            {22,.9f,0,0,2,1,0}, {21,.9f,0,1,2,2,1},
                            {22,.9f,0,1,1,2,2}, {5,.9f,1,1,2,2,3}};
                        std::memcpy(rows.data(), table_samples, sizeof(table_samples));
                    }
                    if (scenario_ == "printed_page_filtered") rows[1] = .2f;
                } else if (scenario_ == "layout_table") {
                    const float table_samples[][7] = {
                        {21,.9f,0,0,2,2,0}, {22,.9f,0,0,1,1,1}};
                    std::memcpy(rows.data(), table_samples, sizeof(table_samples));
                } else if (scenario_ == "layout_inline_formula") {
                    const float inline_samples[][7] = {
                        {22,.9f,0,0,2,2,0}, {5,.9f,0,0,1,1,1}};
                    std::memcpy(rows.data(), inline_samples, sizeof(inline_samples));
                } else if (scenario_ == "layout_contract")
                    std::memcpy(rows.data(), samples, sizeof(samples));
                int32_t count = scenario_ == "printed_page_filtered" || scenario_ == "printed_page_slow" ? 1 :
                    scenario_.rfind("printed_page_formula", 0) == 0 ? 4 :
                    scenario_.rfind("printed_page_table", 0) == 0 ? 4 :
                    scenario_.rfind("printed_page", 0) == 0 ? 6 :
                    scenario_ == "layout_contract" ? 7 :
                    scenario_ == "layout_table" ? 2 :
                    scenario_ == "layout_inline_formula" ? 2 : 0;
                std::vector<int32_t> masks(300*200*200);
                if (scenario_ == "layout_contract") {
                    masks[66] = 1;
                    masks[200*200] = 1;
                    masks[2*200*200+100*200+100] = 1;
                }
                Tensor a{"fetch_name_0", DataType::Float32, TensorLayout::Matrix, {300,7},
                         std::vector<uint8_t>(rows.size()*sizeof(float))};
                Tensor b{"fetch_name_1", DataType::Int32, TensorLayout::Matrix, {1},
                         std::vector<uint8_t>(sizeof(count))};
                Tensor c{"fetch_name_2", DataType::Int32, TensorLayout::Matrix, {300,200,200},
                         std::vector<uint8_t>(masks.size()*sizeof(int32_t))};
                std::memcpy(a.data.data(), rows.data(), a.data.size());
                std::memcpy(b.data.data(), &count, sizeof(count));
                std::memcpy(c.data.data(), masks.data(), c.data.size());
                return {TensorOutput{{std::move(a), std::move(b), std::move(c)}}};
            }
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
        if (scenario_.rfind("printed_page", 0) == 0) {
            if (scenario_ == "printed_page_slow")
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (scenario_ == "printed_page_invalid_utf8" && generation.source_box.y0 == 0)
                throw std::runtime_error(std::string("\xff", 1));
            GenerationOutput result;
            result.elapsed_ms = 2;
            if (scenario_.rfind("printed_page_formula", 0) == 0) {
                if (generation.task == "formula") {
                    result.text = result.raw_output =
                        scenario_ == "printed_page_formula_unclosed" ? "$$\\frac{a}{b}=c" :
                        scenario_ == "printed_page_formula_fragment" ? "$a+(b$" :
                        scenario_ == "printed_page_formula_mixed" ? "$a+b$，则" :
                        scenario_ == "printed_page_formula_prose" ? "Please solve x+y" :
                        scenario_ == "printed_page_formula_unknown_command" ? "$$\\foo{a}$$" :
                        scenario_ == "printed_page_formula_missing_arg" ? "$$\\frac{a}$$" :
                        scenario_ == "printed_page_formula_empty_arg" ? "$$\\frac{}{b}=c$$" :
                        scenario_ == "printed_page_formula_sqrt_empty" ? "$$\\sqrt{}$$" :
                        scenario_ == "printed_page_formula_spaced_empty" ?
                            "$$\\frac {}{b}=c$$" :
                        scenario_ == "printed_page_formula_misnested" ? "$$([)]$$" :
                        scenario_ == "printed_page_formula_misnested_brace" ? "$$({)}$$" :
                        scenario_ == "printed_page_formula_scalable_dot" ?
                            "$$\\left. x\\right)$$" :
                        scenario_ == "printed_page_formula_scalable_angle" ?
                            "$$\\left\\langle x\\right\\rangle$$" :
                        scenario_ == "printed_page_formula_env_mismatch" ?
                            "$$\\begin{aligned}x\\end{matrix}$$" :
                        scenario_ == "printed_page_formula_left_missing" ?
                            "$$\\left\\right$$" :
                        scenario_ == "printed_page_formula_percent" ? "$$x%hidden$$" :
                        scenario_ == "printed_page_formula_ampersand" ? "$$a&b$$" :
                        scenario_ == "printed_page_formula_comparison" ? "$$x>0$$" :
                        scenario_ == "printed_page_formula_chinese_text" ?
                            "$$\\frac{\\text{甲}}{b}=c$$" :
                        scenario_ == "printed_page_formula_inline_wrapper" ? "$x^2+1$" :
                        scenario_ == "printed_page_formula_tagged" ? "$$x=1\\tag{1}$$" :
                        "$$\n\\frac{a}{b}=c\n$$";
                } else result.text = result.raw_output =
                    generation.source_box.y0 == 0 ?
                        scenario_ == "printed_page_formula_parent_unclosed" ? "设$x^2+1。" :
                        scenario_ == "printed_page_formula_parent_missing_arg" ?
                            "设$\\frac{a}$，请计算。" :
                        scenario_ == "printed_page_formula_parent_single_symbol" ?
                            "设$r$，可得$x^2+1$。" :
                        scenario_ == "printed_page_formula_parent_currency" ?
                            "价格 $5，设$x^2+1$。" :
                        scenario_ == "printed_page_formula_parent_numeric" ?
                            "已知 $2 = a$，求$x^2+1$。" :
                        scenario_ == "printed_page_formula_parent_numeric_product" ?
                            "已知 $2 x$，求$x^2+1$。" :
                        scenario_ == "printed_page_formula_parent_numeric_macro" ?
                            "已知 $2 \\alpha$，求$x^2+1$。" :
                        "设$x^2+1$。" : "1. 请计算";
                result.finish_reason = scenario_ == "printed_page_formula_truncated" &&
                    generation.task == "formula" ? "truncated" : "complete";
                result.stop_reason = result.finish_reason == "truncated" ? "token_limit" : "normal";
                return {result};
            }
            if (scenario_.rfind("printed_page_table", 0) == 0) {
                result.text = result.raw_output = generation.task == "table" ?
                    scenario_ == "printed_page_table_unclosed" ?
                        "<table><tr><td>甲</td></tr>" :
                    scenario_ == "printed_page_table_prefix" ?
                        "<table><tr><td>甲" :
                    scenario_ == "printed_page_table_unsafe" ?
                        "<table><tr><td><img src=images/fake.png></td></tr></table>" :
                    scenario_ == "printed_page_table_bad_span" ?
                        "<table><tr><td rowspan=3>甲</td><td>1</td></tr>"
                        "<tr><td>2</td></tr></table>" :
                    scenario_ == "printed_page_table_duplicate_span" ?
                        "<table><tr><td rowspan=1 rowspan=2>甲</td></tr>"
                        "<tr><td>2</td></tr></table>" :
                    scenario_ == "printed_page_table_bad_section" ?
                        "<table><tbody><tr><td>甲</td></tr></tbody/></table>" :
                    scenario_ == "printed_page_table_rowspan" ?
                        "<table><tr><td rowspan=2>甲</td><td>1</td></tr>"
                        "<tr><td>2</td></tr></table>" :
                    "<table border=1><tr><th colspan=2>项目</th></tr>"
                    "<tr><td>甲</td><td>$x+1$</td></tr></table>" :
                    "外部表题";
                result.finish_reason = scenario_ == "printed_page_table_truncated" &&
                    generation.task == "table" ? "truncated" : "complete";
                result.stop_reason = result.finish_reason == "truncated" ? "token_limit" : "normal";
                return {result};
            }
            if (generation.task == "formula") {
                result.text = result.raw_output = "## 公式=原始片段";
                result.finish_reason = "complete"; result.stop_reason = "normal";
            } else if (generation.task == "table") {
                result.text = result.raw_output = "<table><tr><td>甲</td></tr></table>";
                result.finish_reason = "complete"; result.stop_reason = "normal";
            } else if (scenario_ == "printed_page_failure" && generation.source_box.y0 == 0) {
                result.raw_output = "partial raw"; result.finish_reason = "failed";
                result.stop_reason = "error"; result.error = "controlled_failure";
            } else if (scenario_ == "printed_page_empty" && generation.source_box.y0 == 0) {
                result.finish_reason = "failed"; result.stop_reason = "empty_output";
                result.error = "ovis_empty_output";
            } else if (scenario_ == "printed_page_truncated" && generation.source_box.y0 == 0) {
                result.text = result.raw_output = "截断"; result.finish_reason = "truncated";
                result.stop_reason = "token_limit"; result.error = "ovis_token_limit";
            } else if (scenario_ == "printed_page_model_resource" && generation.source_box.y0 == 0) {
                result.text = result.raw_output =
                    "正文<img src=\"https://example.invalid/x.png\" />\n![](images/fake.png)"
                    "\n\\![单](https://example.invalid/one.png)"
                    "\n\\\\![双](https://example.invalid/two.png)"
                    "\n\\(x+1\\) \\[a+b\\]";
                result.finish_reason = "complete"; result.stop_reason = "normal";
            } else {
                result.text = result.raw_output = "中文，English!\n第二行。";
                result.finish_reason = "complete"; result.stop_reason = "normal";
            }
            return {result};
        }
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
    bool reset() override {
        ++reset_count_;
        return scenario_ != "reset_failure" &&
            !(scenario_ == "printed_page_reset_failure" && reset_count_ == 1);
    }
    void unload() override {}
private:
    EngineCapabilities capabilities_{true, true, true, 1};
    std::string scenario_;
    std::vector<ArtifactInfo> loaded_;
    int reset_count_ = 0;
};
bool config_supported(const std::string& config) {
    return config == "fixture:normalized" || config == "fixture:runtime" ||
           config == "fixture:graph" || config == "fixture:sample" || config == "fixture:blank" ||
           config == "fixture:failure" || config == "fixture:formula_table" ||
           config == "fixture:slow" || config == "fixture:invalid_utf8" ||
           config == "fixture:unknown" || config == "fixture:reset_failure" ||
           config == "fixture:generation_exception" || config == "fixture:wrong_response" ||
           config == "fixture:layout_contract" || config == "fixture:layout_table" ||
           config == "fixture:layout_inline_formula" ||
           config == "fixture:layout_empty" ||
           config == "fixture:layout_infer_failure" || config == "fixture:printed_page" ||
           config == "fixture:printed_page_failure" || config == "fixture:printed_page_empty" ||
           config == "fixture:printed_page_truncated" ||
           config == "fixture:printed_page_invalid_utf8" ||
           config == "fixture:printed_page_model_resource" ||
           config == "fixture:printed_page_reset_failure" ||
           config == "fixture:printed_page_filtered" || config == "fixture:printed_page_slow" ||
           config == "fixture:printed_page_formula" ||
           config == "fixture:printed_page_formula_unclosed" ||
           config == "fixture:printed_page_formula_fragment" ||
           config == "fixture:printed_page_formula_mixed" ||
           config == "fixture:printed_page_formula_prose" ||
           config == "fixture:printed_page_formula_unknown_command" ||
           config == "fixture:printed_page_formula_missing_arg" ||
           config == "fixture:printed_page_formula_empty_arg" ||
           config == "fixture:printed_page_formula_sqrt_empty" ||
           config == "fixture:printed_page_formula_spaced_empty" ||
           config == "fixture:printed_page_formula_misnested" ||
           config == "fixture:printed_page_formula_misnested_brace" ||
           config == "fixture:printed_page_formula_scalable_dot" ||
           config == "fixture:printed_page_formula_scalable_angle" ||
           config == "fixture:printed_page_formula_env_mismatch" ||
           config == "fixture:printed_page_formula_left_missing" ||
           config == "fixture:printed_page_formula_percent" ||
           config == "fixture:printed_page_formula_ampersand" ||
           config == "fixture:printed_page_formula_comparison" ||
           config == "fixture:printed_page_formula_chinese_text" ||
           config == "fixture:printed_page_formula_parent_unclosed" ||
           config == "fixture:printed_page_formula_parent_missing_arg" ||
           config == "fixture:printed_page_formula_parent_single_symbol" ||
           config == "fixture:printed_page_formula_parent_currency" ||
           config == "fixture:printed_page_formula_parent_numeric" ||
           config == "fixture:printed_page_formula_parent_numeric_product" ||
           config == "fixture:printed_page_formula_parent_numeric_macro" ||
           config == "fixture:printed_page_formula_inline_wrapper" ||
           config == "fixture:printed_page_formula_tagged" ||
           config == "fixture:printed_page_formula_truncated" ||
           config == "fixture:printed_page_table" ||
           config == "fixture:printed_page_table_unclosed" ||
           config == "fixture:printed_page_table_prefix" ||
           config == "fixture:printed_page_table_truncated" ||
           config == "fixture:printed_page_table_unsafe" ||
           config == "fixture:printed_page_table_rowspan" ||
           config == "fixture:printed_page_table_bad_span" ||
           config == "fixture:printed_page_table_duplicate_span" ||
           config == "fixture:printed_page_table_bad_section";
}
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config) {
    return std::make_unique<FixtureBackend>(config.substr(8));
}
std::string backend_capabilities(const std::string&) {
    return "{\"schema_version\":\"1.0\",\"backend\":\"test_fixture\",\"model_available\":false,\"tasks\":[\"layout\",\"text\",\"formula\",\"table\"]}";
}
}
