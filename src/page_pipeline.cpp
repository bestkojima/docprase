#include "dococr/inference.hpp"
#include "page_identity.hpp"
#include "page_pipeline_context.hpp"
#include "layout_inference.hpp"
#include "layout_filter.hpp"
#include "candidate_review.hpp"
#include "page_plan.hpp"
#include "page_execution.hpp"
#include "document_export.hpp"
#include "image_resources.hpp"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace dococr {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start) {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}
void append_assets(std::vector<Asset>& target, std::vector<Asset> source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}
RunResult export_page(const PagePipelineContext& context, const PageExecutionResult& execution,
    const std::string& state, const PageLayoutExport* layout, JobOutput output, RunResult audit) {
    const auto start = Clock::now();
    if (context.progress) context.progress("export_started", context.identity.progress_page(), "",
                                          execution.region_done, execution.region_total);
    auto document = export_document_page({{context.image.width, context.image.height,
        context.identity.source_page(), document_id(context.image)},
        execution.blocks, state, context.backend.profile(), layout});
    output.json = std::move(document.json);
    output.markdown = std::move(document.markdown);
    audit.did_export = true;
    if (context.progress) context.progress("export_completed", context.identity.progress_page(), "",
                                          execution.region_done, execution.region_total);
    audit.export_ms = elapsed(start);
    uint64_t bytes = output.json.size() + output.markdown.size();
    for (const auto& asset : output.assets) bytes += asset.png.size();
    if (context.plan && bytes > context.plan->max_output_bytes) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "output_bytes";
        return audit;
    }
    audit.code = state == "blank" ? RunCode::Blank : state == "partial" ? RunCode::Partial : RunCode::Ok;
    audit.output = std::move(output);
    return audit;
}
RunResult run_doclayout_pipeline(const PagePipelineContext& context, RunResult audit) {
    const auto* plan = context.plan;
    const bool transcribe = plan && (plan->backend == "mnn:pp-doclayout-v3+ovisocr2" ||
        plan->backend.rfind("fixture:printed_page", 0) == 0);
    auto layout = infer_doclayout(context, transcribe, audit);
    if (!layout) return audit;
    const image_resources::LayoutRasterSource raster{
        context.image.width, context.image.height, layout->output, layout->transform};
    auto records = filter_layout_candidates(context.image.width, context.image.height,
                                           std::move(layout->output.candidates));
    CandidateReviewResult review;
    if (plan && plan->collect_candidate_reviews) {
        review = review_layout_candidates(plan->layout_candidate_reviews,
            context.image, raster, context.identity, records);
        append_assets(audit.output.assets, std::move(review.assets));
    }
    audit.did_layout = true;
    if (context.progress) context.progress("layout_completed", context.identity.progress_page(), "", 0, 0);
    for (const auto& tensor : layout->output.tensors()) {
        if (tensor.name == "fetch_name_0")
            audit.output.assets.push_back({context.identity.page_asset("fetch_name_0.f32"), tensor.data});
        else if (tensor.name == "fetch_name_1")
            audit.output.assets.push_back({context.identity.page_asset("fetch_name_1.i32"), tensor.data});
    }
    audit.output.assets.push_back({context.identity.page_asset("fetch_name_2.rle"), layout->output.masks_rle()});
    PagePlanInput planning_input{context.image.width, context.image.height,
                                context.identity.source_page(), transcribe, {}};
    for (const auto& candidate : records)
        if (candidate.selected) planning_input.candidates.push_back(candidate);
    const PreRecognitionPagePlan page_plan = plan_page(planning_input);
    // 从这里开始暂存全部资产；取消和超预算不会把未完成输出发布到 audit。
    JobOutput output;
    output.assets = std::move(audit.output.assets);
    auto execution = execute_planned_regions(context, page_plan, records, raster, transcribe, std::move(audit));
    if (!execution.completed) return std::move(execution.audit);
    audit = std::move(execution.audit);
    append_assets(output.assets, std::move(execution.assets));
    const auto overlay_name = context.identity.page_asset("layout-overlay.png");
    output.assets.push_back({overlay_name,
        image_resources::layout_overlay_png(context.image, records, raster, layout->preview)});
    const bool incomplete = std::any_of(execution.blocks.begin(), execution.blocks.end(),
        [](const DocumentBlock& block) { return block.result.status != "ok"; });
    const std::string state = records.empty() ? "blank" :
        (execution.blocks.empty() || incomplete) ? "partial" : "ok";
    const LabelExportPolicy label_export = plan ? plan->label_export : LabelExportPolicy{};
    const PageLayoutExport layout_export{records, page_plan, layout->transform, label_export,
        overlay_name, plan ? plan->layout_score_threshold : .5, review.evidence};
    return export_page(context, execution, state, &layout_export, std::move(output), std::move(audit));
}
RunResult run_generic_pipeline(const PagePipelineContext& context, RunResult audit) {
    auto candidates = infer_generic_layout(context, audit);
    if (!candidates) return audit;
    auto execution = execute_generic_regions(context, *candidates, std::move(audit));
    if (!execution.completed) return std::move(execution.audit);
    const bool partial = std::any_of(execution.blocks.begin(), execution.blocks.end(),
        [](const DocumentBlock& block) { return block.result.status != "ok"; });
    const std::string state = candidates->empty() ? "blank" : partial ? "partial" : "ok";
    JobOutput output;
    output.assets = std::move(execution.assets);
    audit = std::move(execution.audit);
    return export_page(context, execution, state, nullptr, std::move(output), std::move(audit));
}
} // namespace

RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled,
    const ExecutionPlan* plan, uint32_t source_page, const ProgressCallback& progress) {
    const PageIdentity identity(source_page);
    RunResult audit{RunCode::Failed, {}};
    const auto start = Clock::now();
    const bool layout_job = plan && plan->uses_doclayout();
    auto decoded_image = image_resources::decode(input,
        layout_job ? max_layout_source_pixels : max_standard_page_pixels);
    if (!decoded_image) return {RunCode::InputError, {}};
    audit.did_decode = true;
    if (progress) progress("decode_completed", identity.progress_page(), "", 0, 0);
    audit.page_pixels = uint64_t(decoded_image->width) * decoded_image->height;
    audit.decode_ms = elapsed(start);
    if (plan && audit.page_pixels > plan->max_page_pixels) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "input_pixels";
        return audit;
    }
    if (!backend) return {RunCode::Unsupported, {}};
    const PagePipelineContext context{*backend, *decoded_image, identity, plan, cancelled, progress};
    return layout_job ? run_doclayout_pipeline(context, std::move(audit)) :
                        run_generic_pipeline(context, std::move(audit));
}
} // namespace dococr
