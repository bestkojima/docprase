#include "dococr/inference.hpp"
#include "page_identity.hpp"
#include "config.hpp"
#include "json.hpp"
#include "dococr/dococr.h"
#include <array>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace dococr;
using Json = nlohmann::json;
namespace {
using Row = std::array<float, 7>;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Backend final : public IInferenceEngine {
public:
    std::vector<Row> rows;
    bool layout_exception = false, layout_malformed = false, cancel_layout = false;
    bool fail_first_region = false, cancel_second_region = false, reset_failure = false;
    bool generation_exception = false, wrong_generation = false;
    int region_calls = 0, reset_calls = 0;
    std::vector<std::string> requests;
    explicit Backend(std::vector<Row> candidates) : rows(std::move(candidates)) {}
    bool load(const BackendLoadSpec&) override { return true; }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return {}; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override { return "fixture:pipeline"; }
    bool reset() override { ++reset_calls; return !reset_failure; }
    void unload() override {}
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        requests.push_back(request.request_id);
        if (const auto* layout = std::get_if<TensorRequest>(&request.payload)) {
            if (layout_exception) throw std::runtime_error("layout_boom");
            if (cancel_layout) context.cancelled = true;
            const bool pp = layout->requested_outputs.front() == "fetch_name_0";
            TensorOutput output;
            output.outputs.push_back({pp ? "fetch_name_0" : "layout_candidates", DataType::Float32,
                TensorLayout::Matrix, {pp ? 300 : int64_t(rows.size()), 7},
                std::vector<uint8_t>((pp ? 300 : rows.size()) * 7 * sizeof(float))});
            for (size_t i = 0; i < rows.size(); ++i)
                std::memcpy(output.outputs[0].data.data() + i * 7 * sizeof(float), rows[i].data(), 7 * sizeof(float));
            if (pp) {
                int32_t count = int32_t(rows.size());
                Tensor quantity{"fetch_name_1", DataType::Int32, TensorLayout::Matrix, {1}, std::vector<uint8_t>(4)};
                std::memcpy(quantity.data.data(), &count, sizeof(count));
                output.outputs.push_back(std::move(quantity));
                output.outputs.push_back({"fetch_name_2", DataType::Int32, TensorLayout::Matrix,
                    {300, 200, 200}, std::vector<uint8_t>(300 * 200 * 200 * sizeof(int32_t))});
            }
            if (layout_malformed) output.outputs[0].shape.pop_back();
            return {std::move(output)};
        }
        ++region_calls;
        if (generation_exception) throw std::runtime_error("region_boom");
        if (wrong_generation) return {TensorOutput{}};
        if (fail_first_region && region_calls == 1)
            return {GenerationOutput{"", "raw_failure", "failed", "controlled_failure", "error"}};
        if (cancel_second_region && region_calls == 2) {
            context.cancelled = true;
            return {GenerationOutput{"", "cancelled_raw", "failed", "", "cancelled"}};
        }
        GenerationOutput output{"正文", "正文", "complete", "", "normal"};
        output.visual_evidence = "explicit_success";
        return {std::move(output)};
    }
private:
    EngineCapabilities capabilities_{true, true, true, 1};
};
ExecutionPlan pp_plan() {
    ExecutionPlan plan;
    plan.backend = "fixture:printed_page";
    plan.layout_preprocess = "reference";
    plan.max_output_bytes = 64 * 1024 * 1024;
    return plan;
}
RunResult run(Backend& backend, const ExecutionPlan* plan = nullptr, uint32_t page = 0,
              const ProgressCallback& progress = {}) {
    std::vector<uint8_t> pixels(4 * 4 * 3, 127);
    std::atomic_bool cancelled{false};
    return run_page(&backend, {pixels.data(), pixels.size(), DOCOCR_IMAGE_RGB8, 4, 4, 12},
                             cancelled, plan, page, progress);
}
void expect_no_output(const RunResult& result) {
    check(result.output.json.empty() && result.output.markdown.empty() && result.output.assets.empty(),
          "失败/取消/超预算发布了暂存资源或文档");
}
void identity_and_reentry() {
    const PageIdentity image(0), pdf(7);
    check(image.page_id() == "p0001" && image.id('b', 1) == "b0001" &&
          image.block_asset("b0001") == "assets/p0001-b0001.png" &&
          pdf.id('b', 1) == "p0007-b0001" &&
          pdf.block_asset("p0007-b0001") == "assets/p0007-b0001.png", "页面身份规则改变");
    Backend outer({{3, 1, 0, 0, 4, 4, 0}});
    bool nested = false;
    auto result = run(outer, nullptr, 7,
        [&](const char* stage, uint32_t page, const std::string&, uint32_t, uint32_t) {
            check(page == 7, "外层 pipeline 进度页号受到嵌套调用影响");
            if (!nested && std::string(stage) == "decode_completed") {
                nested = true;
                Backend inner({{3, 1, 0, 0, 4, 4, 0}});
                const auto inner_result = run(inner, nullptr, 2);
                check(inner_result.code == RunCode::Ok &&
                      inner_result.output.assets[0].name == "assets/p0002-b0001.png", "嵌套页身份错误");
            }
        });
    check(nested && result.code == RunCode::Ok && outer.requests[0] == "layout-p0007" &&
          result.output.assets[0].name == "assets/p0007-b0001.png", "外层资源/请求身份未恢复");
    auto worker = [](uint32_t page) {
        Backend backend({{3, 1, 0, 0, 4, 4, 0}});
        return run(backend, nullptr, page);
    };
    auto first = std::async(std::launch::async, worker, 3);
    auto second = std::async(std::launch::async, worker, 11);
    check(first.get().output.assets[0].name == "assets/p0003-b0001.png" &&
          second.get().output.assets[0].name == "assets/p0011-b0001.png", "独立 pipeline 页身份串页");
}
void layout_failures_and_asset_commit() {
    auto plan = pp_plan();
    for (int defect = 0; defect < 3; ++defect) {
        Backend backend({{14, 1, 0, 0, 4, 4, 0}});
        backend.layout_exception = defect == 0;
        backend.layout_malformed = defect == 1;
        backend.cancel_layout = defect == 2;
        const auto result = run(backend, &plan, 9);
        check(result.code == (defect == 2 ? RunCode::Cancelled : RunCode::Failed) &&
              result.did_decode && result.did_normalize && result.layout_attempted && !result.did_layout &&
              result.output.assets.size() == 3 && result.output.assets[0].name == "assets/p0009-image.f32" &&
              result.output.json.empty(), "PP 布局早退的输入诊断或审计改变");
        if (defect < 2)
            check(result.error_code == (defect == 0 ? "layout_inference_failed" : "layout_output_contract_mismatch"),
                  "PP 布局失败映射改变");
    }
    Backend generic({{3, 1, 0, 0, 4, 4, 0}});
    generic.layout_malformed = true;
    const auto malformed = run(generic);
    check(malformed.code == RunCode::Failed && !malformed.did_decode && malformed.page_pixels == 0,
          "通用非法布局未返回 fresh Failed");
    expect_no_output(malformed);
    generic.layout_malformed = false; generic.layout_exception = true;
    bool propagated = false;
    try { (void)run(generic); } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "layout_boom";
    }
    check(propagated, "通用布局异常被错误吞掉");
    std::atomic_bool cancelled{false};
    std::vector<uint8_t> pixels(48, 127);
    Backend pp({{14, 1, 0, 0, 4, 4, 0}});
    const auto interrupted = run_page(&pp, {pixels.data(), pixels.size(), DOCOCR_IMAGE_RGB8, 4, 4, 12},
        cancelled, &plan, 0, [&](const char* stage, uint32_t, const std::string&, uint32_t, uint32_t) {
            if (std::string(stage) == "layout_completed") cancelled = true;
        });
    check(interrupted.code == RunCode::Cancelled && interrupted.did_layout && !interrupted.did_export,
          "PP 规划交接取消状态改变");
    expect_no_output(interrupted);
}
void budgets_and_filtered_page() {
    for (bool pp : {false, true}) {
        auto plan = pp_plan(); if (!pp) plan.backend = "fixture:sample";
        Backend backend({{pp ? 14.f : 3.f, 1, 0, 0, 4, 4, 0}});
        plan.max_page_pixels = 15;
        auto result = run(backend, &plan);
        check(result.code == RunCode::BudgetExceeded && result.budget_stage == "input_pixels" &&
              result.did_decode && backend.requests.empty(), "输入预算未在模型调用前检查");
        expect_no_output(result);
        plan.max_page_pixels = 16; plan.max_output_bytes = 1;
        result = run(backend, &plan);
        check(result.code == RunCode::BudgetExceeded && result.budget_stage == "output_bytes" && result.did_export,
              "输出预算或导出审计改变");
        expect_no_output(result);
    }
    auto plan = pp_plan();
    Backend filtered({{22, .1f, 0, 0, 4, 4, 0}});
    const auto result = run(filtered, &plan);
    check(result.code == RunCode::Partial && filtered.region_calls == 0 &&
          Json::parse(result.output.json)["pages"][0]["blocks"].empty(), "全部筛除被误记为空页或执行了识别");
}
void recognition_failures_and_cancel_priority() {
    for (bool pp : {false, true}) {
        auto plan = pp_plan(); if (!pp) plan.backend = "fixture:sample";
        const float category = pp ? 22.f : 0.f;
        Backend backend({{category, 1, 0, 0, 4, 2, 0}, {category, 1, 0, 2, 4, 4, 1}});
        backend.fail_first_region = backend.cancel_second_region = true;
        auto result = run(backend, &plan);
        check(result.code == RunCode::Failed && result.error_code == "region_inference_failed" &&
              result.error_message == "controlled_failure" && result.regions.size() == (pp ? 2u : 1u) &&
              (!pp || result.regions[0].status == "failed") && result.regions.back().status == "cancelled" &&
              backend.region_calls == 2, "先失败再取消的终态优先级或区域来源改变");
        expect_no_output(result);
        for (int defect = 0; defect < 3; ++defect) {
            Backend failure({{category, 1, 0, 0, 4, 2, 0}, {category, 1, 0, 2, 4, 4, 1}});
            failure.reset_failure = defect == 0;
            failure.generation_exception = defect == 1;
            failure.wrong_generation = defect == 2;
            result = run(failure, &plan);
            const auto blocks = Json::parse(result.output.json)["pages"][0]["blocks"];
            check(result.code == RunCode::Partial && blocks[0]["status"] == "failed" &&
                  blocks[1]["status"] == "skipped" && failure.reset_calls == 1 &&
                  failure.region_calls == (defect == 0 ? 0 : 1), "后端失败后的区域隔离改变");
            check(blocks[0]["error"] == (defect == 0 ? (pp ? "region_reset_failed" : "region reset failed") :
                  defect == 1 ? (pp ? "region_inference_exception:region_boom" : "region inference exception") :
                  (pp ? "generation_response_type_mismatch" : "generation response type mismatch")),
                  "两种后端的错误文本被统一改写");
        }
    }
}
}
int main() {
    try {
        identity_and_reentry(); layout_failures_and_asset_commit(); budgets_and_filtered_page();
        recognition_failures_and_cancel_priority();
        std::cout << "页面 pipeline：身份隔离、阶段交接、资源提交、预算、异常与取消检查通过\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
