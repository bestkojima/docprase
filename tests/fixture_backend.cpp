#include "backend_factory.hpp"
#include "config.hpp"
#include "visual_adaptation.hpp"
#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
namespace dococr {
namespace {
bool layout_only_fixture(const std::string& scenario) {
    return scenario == "layout_dedup" || scenario == "layout_dedup_edges" ||
           scenario == "layout_geometry" || scenario == "layout_smartresize" ||
           scenario == "layout_contract" || scenario == "layout_table" ||
           scenario == "layout_inline_formula" || scenario == "layout_empty" ||
           scenario == "layout_infer_failure";
}
}
class FixtureBackend final : public IInferenceEngine {
public:
    explicit FixtureBackend(std::string scenario) : scenario_(std::move(scenario)) {
        if (layout_only_fixture(scenario_))
            capabilities_.generation = false;
    }
    bool load(const BackendLoadSpec& spec) override {
        if (spec.backend_id != "fixture:" + scenario_) return false;
        if (scenario_ == "printed_page_rebuild_failure") {
            static std::atomic_uint loads{0};
            if (loads.fetch_add(1) != 0) return false;
        }
        if (scenario_ == "printed_page_finalization_oom") {
            static std::atomic_uint loads{0};
            rebuilt_for_oom_ = loads.fetch_add(1) != 0;
        }
        loaded_.clear();
        for (const auto& artifact : spec.artifacts) {
            if (artifact.contract_status != "verified_fixture" ||
                sha256_file(artifact.path) != artifact.sha256) return false;
            loaded_.push_back(artifact);
        }
        if (!spec.config_hash.empty() && loaded_.size() !=
            (layout_only_fixture(scenario_) ? 1u : 2u)) return false;
        return true;
    }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return loaded_; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override {
        if (scenario_ == "printed_page_finalization_oom" && rebuilt_for_oom_ &&
            ++profile_calls_ == 1)
            throw std::bad_alloc();
        if (loaded_.empty()) return "test_fixture";
        std::string profile = "test_fixture";
        for (const auto& artifact : loaded_)
            profile += "/" + artifact.model + "=" + artifact.sha256;
        return profile;
    }
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        auto response = execute_impl(request, context);
        if (std::holds_alternative<GenerationRequest>(request.payload)) {
            auto* output = std::get_if<GenerationOutput>(&response.payload);
            const auto& generation = std::get<GenerationRequest>(request.payload);
            if (output && output->finish_reason == "complete" && output->visual_evidence.empty() &&
                !(scenario_ == "printed_page_visual_missing_complete" && generation.source_box.y0 == 0))
            {
                output->visual_evidence = "explicit_success";
                output->visual_transform = adapt_visual(generation.image).transform;
            }
        }
        return response;
    }
    InferenceResponse execute_impl(const InferenceRequest& request, ExecutionContext& context) {
        if (auto* layout = std::get_if<TensorRequest>(&request.payload)) {
            if (scenario_ == "printed_page_layout_gate_error") {
                const char* gate = std::getenv("DOCOCR_TEST_GATE_PATH");
                const char* entered = std::getenv("DOCOCR_TEST_ENTERED_PATH");
                if (entered) std::ofstream(entered).put('1');
                if (gate) while (std::filesystem::exists(gate))
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (context.cancelled) throw std::runtime_error("controlled_interrupt_failure");
            }
            if (scenario_.rfind("printed_page", 0) == 0 || layout_only_fixture(scenario_)) {
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
                if (geometry[0] != (scenario_ == "layout_smartresize" ? 1 :
                                    scenario_.rfind("printed_page_reading", 0) == 0 ||
                                    scenario_ == "layout_geometry" ? 8 : 400) ||
                    geometry[1] != (scenario_ == "layout_smartresize" ? 1 :
                                    scenario_.rfind("printed_page_reading", 0) == 0 ||
                                    scenario_ == "layout_geometry" ? 8 : 400))
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
                        {99,.9f,1.05f,1,2,2,4}, {14,.9f,0,0,1,1,5}};
                    std::memcpy(rows.data(), page_samples, sizeof(page_samples));
                    if (scenario_.rfind("printed_page_reading", 0) == 0) {
                        const float reading_samples[][7] = {
                            {22,.9f,5,10,40,20,20}, {22,.9f,60,10,95,20,10},
                            {17,.9f,5,30,95,40,30}, {22,.9f,5,50,40,60,60},
                            {22,.9f,60,50,95,60,50}, {14,.9f,5,63,40,78,61},
                            {7,.9f,5,79,40,86,62}, {21,.9f,60,63,95,78,51},
                            {17,.9f,60,79,95,86,52}, {10,.9f,5,90,40,98,63},
                            {99,.9f,60,89,95,98,53}, {22,.2f,0,0,100,100,99}};
                        std::memcpy(rows.data(), reading_samples, sizeof(reading_samples));
                        if (scenario_ == "printed_page_reading_rank" ||
                            scenario_ == "printed_page_reading_owned_rank") {
                            const float ranks[] = {10,20,30,40,80,50,60,90,100,70,110};
                            for (size_t i = 0; i < 11; ++i) rows[i*7+6] = ranks[i];
                        }
                        if (scenario_ == "printed_page_reading_missing") rows[4*7+6] = -1;
                        if (scenario_ == "printed_page_reading_duplicate") rows[4*7+6] = 60;
                        if (scenario_ == "printed_page_reading_ambiguous") {
                            const float second_image[] = {14,.9f,5,63,40,78,54};
                            const float second_table[] = {21,.9f,60,63,95,78,55};
                            std::memcpy(rows.data()+10*7, second_image, sizeof(second_image));
                            std::memcpy(rows.data()+11*7, second_table, sizeof(second_table));
                        }
                        if (scenario_ == "printed_page_reading_footer") {
                            const float footer[] = {8,.9f,45,95,55,99,99};
                            std::memcpy(rows.data()+11*7, footer, sizeof(footer));
                        }
                        if (scenario_ == "printed_page_reading_footnote_vision") {
                            const float note[] = {24,.9f,5,50,40,60,60};
                            std::memcpy(rows.data()+3*7, note, sizeof(note));
                        }
                        if (scenario_ == "printed_page_reading_owned" ||
                            scenario_ == "printed_page_reading_owned_rank") {
                            const float table_text[] = {22,.9f,62,65,80,70,54};
                            const float table_formula[] = {5,.9f,82,65,90,70,55};
                            std::memcpy(rows.data()+10*7, table_text, sizeof(table_text));
                            std::memcpy(rows.data()+11*7, table_formula, sizeof(table_formula));
                        }
                        if (scenario_ == "printed_page_reading_columns") {
                            const float left_title[] = {17,.9f,5,30,40,40,30};
                            std::memcpy(rows.data()+2*7, left_title, sizeof(left_title));
                        }
                        if (scenario_ == "printed_page_reading_short_title") {
                            const float short_title[] = {17,.9f,30,30,70,40,30};
                            std::memcpy(rows.data()+2*7, short_title, sizeof(short_title));
                        }
                        if (scenario_ == "printed_page_reading_tiny_title") {
                            const float short_title[] = {17,.9f,44,30,56,40,30};
                            std::memcpy(rows.data()+2*7, short_title, sizeof(short_title));
                        }
                        if (scenario_ == "printed_page_reading_single") {
                            const float single[][7] = {
                                {22,.9f,5,10,40,20,10}, {17,.9f,5,30,40,40,30},
                                {14,.9f,5,50,40,65,50}, {7,.9f,5,66,40,75,70}};
                            std::memcpy(rows.data(), single, sizeof(single));
                        }
                        if (scenario_ == "printed_page_reading_model_order") {
                            const float single[][7] = {
                                {22,.9f,5,10,20,20,20}, {22,.9f,22,10,40,20,10},
                                {22,.9f,5,30,40,40,30}};
                            std::memcpy(rows.data(), single, sizeof(single));
                        }
                        if (scenario_ == "printed_page_reading_sectioned" ||
                            scenario_ == "printed_page_reading_local_title") {
                            const float sectioned[][7] = {
                                {12,.9f,6,4,64,7,10}, {17,.9f,6,11,48,14,10},
                                {22,.9f,6,18,43,24,10}, {22,.9f,53,18,90,24,10},
                                {22,.9f,6,31,42,37,10}, {14,.9f,61,28,87,43,10},
                                {22,.9f,6,42,42,48,10}, {7,.9f,60,44,88,47,10},
                                {22,.9f,53,50,90,56,10}, {17,.9f,6,63,30,67,10},
                                {22,.9f,6,70,70,74,10}, {22,.9f,6,77,30,81,10}};
                            std::memcpy(rows.data(), sectioned, sizeof(sectioned));
                            if (scenario_ == "printed_page_reading_local_title") {
                                const float right_text[] = {22,.9f,53,40,90,48,10};
                                const float local_title[] = {17,.9f,6,42,30,48,10};
                                std::memcpy(rows.data()+5*7, right_text, sizeof(right_text));
                                std::memcpy(rows.data()+6*7, local_title, sizeof(local_title));
                            }
                        }
                    }
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
                } else if (scenario_ == "layout_dedup") {
                    const float cases[][7] = {
                        {22,.9f,0,0,1,1,0}, {22,.8f,0,0,1,1,1},
                        {5,.7f,0,0,1,1,2}, {14,.95f,0,0,2,2,3},
                        {22,.5f,1,1,2,2,4}, {22,.49f,1,0,2,1,5},
                        {999,.6f,1,0,2,1,6}};
                    std::memcpy(rows.data(), cases, sizeof(cases));
                } else if (scenario_ == "layout_dedup_edges") {
                    const float cases[][7] = {
                        {22,.9f,0,0,1,1,0}, {22,.8f,.5f,0,1.5f,1,1},
                        {22,.7f,.51f,0,1.51f,1,2},
                        {5,.9f,0,1,1,2,3}, {22,.8f,.02f,1,1.02f,2,4},
                        {22,.7f,.021f,1,1.021f,2,5}};
                    std::memcpy(rows.data(), cases, sizeof(cases));
                } else if (scenario_ == "layout_geometry") {
                    const float cases[][7] = {
                        {3,.95f,52,5,90,40,4}, {22,.8f,55,10,65,20,5},
                        {22,.9f,2,2,42,30,1}, {5,.85f,10,0,20,14,2},
                        {21,.9f,2,42,45,85,8}, {22,.8f,1,48,11,58,9},
                        {22,.9f,52,48,90,65,10}, {7,.9f,55,70,90,80,11},
                        {22,.9f,-3,88,28,103,12}, {23,.8f,55,50,85,60,13},
                        {22,.8f,56,70,90,80,14}};
                    std::memcpy(rows.data(), cases, sizeof(cases));
                } else if (scenario_ == "layout_smartresize") {
                    const float full_page[][7] = {{22,.9f,9,0,790,800,0}};
                    std::memcpy(rows.data(), full_page, sizeof(full_page));
                } else if (scenario_ == "layout_contract")
                    std::memcpy(rows.data(), samples, sizeof(samples));
                int32_t count = scenario_ == "printed_page_reading_single" ? 4 :
                    scenario_ == "printed_page_reading_model_order" ? 3 :
                    scenario_.rfind("printed_page_reading", 0) == 0 ? 12 :
                    scenario_ == "printed_page_filtered" || scenario_ == "printed_page_slow" ? 1 :
                    scenario_.rfind("printed_page_formula", 0) == 0 ? 4 :
                    scenario_.rfind("printed_page_table", 0) == 0 ? 4 :
                    scenario_.rfind("printed_page", 0) == 0 ? 6 :
                    scenario_ == "layout_contract" || scenario_ == "layout_dedup" ? 7 :
                    scenario_ == "layout_dedup_edges" ? 6 :
                    scenario_ == "layout_geometry" ? 11 :
                    scenario_ == "layout_smartresize" ? 1 :
                    scenario_ == "layout_table" ? 2 :
                    scenario_ == "layout_inline_formula" ? 2 : 0;
                if (scenario_ == "printed_page_reading_pdf_mixed") {
                    float first_red = 0;
                    std::memcpy(&first_red, layout->inputs[0].data.data(), sizeof(first_red));
                    if (first_red < 0.1f) throw std::runtime_error("controlled_pdf_page_failure");
                    if (first_red > 0.8f && first_red < 0.95f) count = 0;
                }
                std::vector<int32_t> masks(300*200*200);
                if (scenario_ == "layout_contract") {
                    masks[66] = 1;
                    masks[200*200] = 1;
                    masks[2*200*200+100*200+100] = 1;
                }
                if (scenario_ == "layout_geometry") masks[3*200*200+33] = 1;
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
        if (scenario_ == "generation_gate_failed" || scenario_ == "printed_page_gate_failed" ||
            scenario_ == "generation_gate_cancelled" || scenario_ == "printed_page_gate_cancelled" ||
            scenario_ == "generation_gate_timeout" || scenario_ == "printed_page_gate_timeout" ||
            scenario_ == "generation_gate_wrong" || scenario_ == "printed_page_gate_wrong") {
            const char* gate = std::getenv("DOCOCR_TEST_GATE_PATH");
            const char* entered = std::getenv("DOCOCR_TEST_ENTERED_PATH");
            if (entered) std::ofstream(entered).put('1');
            if (gate) while (std::filesystem::exists(gate))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (scenario_ == "generation_gate_cancelled" || scenario_ == "printed_page_gate_cancelled")
                return {GenerationOutput{"", "", "failed", "cancelled", "cancelled"}};
            if (scenario_ == "generation_gate_timeout" || scenario_ == "printed_page_gate_timeout")
                return {GenerationOutput{"", "controlled timeout raw", "failed",
                                         "controlled_backend_timeout", "timeout"}};
            if (scenario_ == "generation_gate_wrong" || scenario_ == "printed_page_gate_wrong")
                return {TensorOutput{}};
            return {GenerationOutput{"", "controlled failed raw", "failed",
                                     "controlled_response_failure", "error"}};
        }
        if (scenario_.rfind("printed_page", 0) == 0) {
            if (scenario_ == "printed_page_gate") {
                const char* gate = std::getenv("DOCOCR_TEST_GATE_PATH");
                const char* entered = std::getenv("DOCOCR_TEST_ENTERED_PATH");
                const char* gate_page = std::getenv("DOCOCR_TEST_GATE_PAGE");
                if (!gate_page || generation.request_id.find(std::string("p000") + gate_page) != std::string::npos) {
                    if (entered) std::ofstream(entered).put('1');
                    if (gate) while (std::filesystem::exists(gate))
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (scenario_ == "printed_page_oom_once") {
                static std::atomic_bool failed{false};
                if (!failed.exchange(true)) throw std::bad_alloc();
            }
            if (scenario_ == "printed_page_fail_once") {
                static std::atomic_bool failed{false};
                if (!failed.exchange(true))
                    return {GenerationOutput{"", "controlled raw", "failed",
                                             "controlled_failure", "error"}};
            }
            if (scenario_ == "printed_page_rebuild_failure" && generation.source_box.y0 == 0)
                return {GenerationOutput{"", "rebuild failure raw", "failed",
                                         "controlled_failure", "error"}};
            if (scenario_ == "printed_page_slow")
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (scenario_ == "printed_page_invalid_utf8" && generation.source_box.y0 == 0)
                throw std::runtime_error(std::string("\xff", 1));
            GenerationOutput result;
            result.elapsed_ms = 2;
            if (scenario_ == "printed_page_visual_empty" && generation.source_box.y0 == 0) {
                // Models the runtime bug: measured visual work but no image-pad IDs.
                result.raw_output = "1. 2. spurious text";
                result.finish_reason = "failed";
                result.stop_reason = "vision_missing";
                result.error = "ovis_visual_tokens_missing";
                result.visual_evidence = "no_visual_tokens";
                result.visual_transform = adapt_visual(generation.image).transform;
                return {result};
            }
            if (scenario_ == "printed_page_visual_missing_complete" && generation.source_box.y0 == 0) {
                result.raw_output = result.text = "spurious complete text";
                result.finish_reason = "complete";
                result.stop_reason = "normal";
                return {result};
            }
            if (scenario_.rfind("printed_page_reading", 0) == 0) {
                result.text = result.raw_output =
                    scenario_ == "printed_page_reading_pdf_literals" &&
                    generation.source_box.x0 == 5 && generation.source_box.y0 == 10 ?
                    "字面 b0001 p0001 assets/p0001-b0001.png" : generation.task == "table" ?
                    "<table><tr><td>值</td></tr></table>" :
                    generation.source_box.x0 == 60 && generation.source_box.y0 == 79 ?
                        scenario_ == "printed_page_reading_table_prose" ? "表明上述结果" : "表1 统计" :
                    generation.source_box.x0 == 5 && generation.source_box.y0 == 79 ? "图1 插图" :
                    generation.source_box.y0 == 90 ?
                        scenario_ == "printed_page_reading_footnote_composite" ? "¹² 来源说明" :
                        "¹ 来源说明" :
                    generation.source_box.x0 == 5 && generation.source_box.y0 == 50 ?
                        scenario_ == "printed_page_reading_footnote_double" ? "左段¹，续¹" :
                        scenario_ == "printed_page_reading_footnote_vision" ? "¹ 另一脚注" : "左段¹" :
                    generation.source_box.y0 == 30 ? "第一节" :
                    generation.source_box.x0 == 60 ? "右段" : "左段";
                result.finish_reason = "complete";
                result.stop_reason = "normal";
                return {result};
            }
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
                result.finish_reason = generation.task == "table" &&
                    scenario_ == "printed_page_table_failed" ? "failed" :
                    generation.task == "table" && scenario_ == "printed_page_table_truncated" ?
                    "truncated" : "complete";
                result.stop_reason = result.finish_reason == "truncated" ? "token_limit" :
                    result.finish_reason == "failed" ? "error" : "normal";
                if (result.finish_reason == "failed") result.error = "controlled_failure";
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
        if (scenario_ == "generation_gate_error") {
            const char* gate = std::getenv("DOCOCR_TEST_GATE_PATH");
            const char* entered = std::getenv("DOCOCR_TEST_ENTERED_PATH");
            if (entered) std::ofstream(entered).put('1');
            if (gate) while (std::filesystem::exists(gate))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (context.cancelled) throw std::runtime_error("controlled_region_failure_after_cancel");
        }
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
        if (scenario_ == "generation_gate_reset_failed" ||
            scenario_ == "printed_page_gate_reset_failed") {
            const char* gate = std::getenv("DOCOCR_TEST_GATE_PATH");
            const char* entered = std::getenv("DOCOCR_TEST_ENTERED_PATH");
            if (entered) std::ofstream(entered).put('1');
            if (gate) while (std::filesystem::exists(gate))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return false;
        }
        return scenario_ != "reset_failure" &&
            !(scenario_ == "printed_page_reset_failure" && reset_count_ == 1);
    }
    void unload() override {}
private:
    EngineCapabilities capabilities_{true, true, true, 1};
    std::string scenario_;
    std::vector<ArtifactInfo> loaded_;
    int reset_count_ = 0;
    mutable int profile_calls_ = 0;
    bool rebuilt_for_oom_ = false;
};
bool config_supported(const std::string& config) {
    return config.rfind("fixture:printed_page_reading", 0) == 0 ||
           config == "fixture:normalized" || config == "fixture:runtime" ||
           config == "fixture:graph" || config == "fixture:sample" || config == "fixture:blank" ||
           config == "fixture:failure" || config == "fixture:formula_table" ||
           config == "fixture:slow" || config == "fixture:invalid_utf8" ||
           config == "fixture:unknown" || config == "fixture:reset_failure" ||
           config == "fixture:generation_exception" || config == "fixture:wrong_response" ||
           config == "fixture:generation_gate_error" ||
           config == "fixture:generation_gate_failed" ||
           config == "fixture:generation_gate_cancelled" ||
           config == "fixture:generation_gate_timeout" ||
           config == "fixture:generation_gate_wrong" ||
           config == "fixture:generation_gate_reset_failed" ||
           config == "fixture:layout_dedup" || config == "fixture:layout_dedup_edges" || config == "fixture:layout_contract" || config == "fixture:layout_table" ||
           config == "fixture:layout_geometry" || config == "fixture:layout_smartresize" ||
           config == "fixture:layout_inline_formula" ||
           config == "fixture:layout_empty" ||
           config == "fixture:layout_infer_failure" || config == "fixture:printed_page" ||
           config == "fixture:printed_page_gate" ||
           config == "fixture:printed_page_gate_failed" ||
           config == "fixture:printed_page_gate_cancelled" ||
           config == "fixture:printed_page_gate_timeout" ||
           config == "fixture:printed_page_gate_wrong" ||
           config == "fixture:printed_page_gate_reset_failed" ||
           config == "fixture:printed_page_layout_gate_error" ||
           config == "fixture:printed_page_oom_once" ||
           config == "fixture:printed_page_fail_once" ||
           config == "fixture:printed_page_rebuild_failure" ||
           config == "fixture:printed_page_finalization_oom" ||
           config == "fixture:printed_page_failure" || config == "fixture:printed_page_empty" ||
           config == "fixture:printed_page_visual_empty" ||
           config == "fixture:printed_page_visual_missing_complete" ||
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
           config == "fixture:printed_page_table_bad_section" ||
           config == "fixture:printed_page_table_failed";
}
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config) {
    return std::make_unique<FixtureBackend>(config.substr(8));
}
std::string backend_capabilities(const std::string&) {
    return "{\"schema_version\":\"1.0\",\"backend\":\"test_fixture\",\"model_available\":false,\"tasks\":[\"layout\",\"text\",\"formula\",\"table\"]}";
}
}
