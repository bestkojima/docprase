#include "page_execution.hpp"
#include "image_resources.hpp"
#include "output_encoding.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

namespace dococr {
using output_encoding::valid_utf8;
using output_encoding::base64;

PageExecutionResult execute_planned_regions(const PagePipelineContext& context,
    const PreRecognitionPagePlan& page_plan, std::vector<LayoutCandidateRecord>& records,
    const image_resources::LayoutRasterSource& raster, bool transcribe, RunResult initial_audit) {
    using Clock = std::chrono::steady_clock;
    PageExecutionResult execution;
    execution.audit = std::move(initial_audit);
    auto& audit = execution.audit;
    auto& blocks = execution.blocks;
    auto& assets = execution.assets;
    auto& region_total = execution.region_total;
    auto& region_done = execution.region_done;
    auto* backend = &context.backend;
    const auto& image = context.image;
    const auto& identity = context.identity;
    const auto* plan = context.plan;
    auto& cancelled = context.cancelled;
    const auto& progress = context.progress;
    ExecutionContext inference_context{cancelled};
    std::vector<const LayoutCandidateRecord*> selected;
    for (int candidate_id : page_plan.candidate_order) selected.push_back(&records.at(size_t(candidate_id)));
    for (const auto& relation : page_plan.ownership) {
        auto& child = records.at(size_t(relation.candidate_id));
        child.handling_reason = relation.handling_reason;
    }
    for (const auto* candidate : selected) {
        auto& mutable_candidate = records[size_t(candidate->id)];
        mutable_candidate.mask_asset = identity.page_asset("mask-c") +
            std::to_string(candidate->id) + ".png";
        assets.push_back({mutable_candidate.mask_asset,
            image_resources::layout_mask_png(*candidate, raster)});
    }
    bool recognition_unavailable = false;
    region_total = uint32_t(page_plan.structure.recognition_order.size());
    std::string first_region_error_code, first_region_error_message;
    auto stop_after_cancel = [&] {
        audit.code = first_region_error_code.empty() ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = first_region_error_code;
        audit.error_message = first_region_error_message;
    };
    for (const auto& planned : page_plan.regions) {
        if (cancelled) { stop_after_cancel(); return execution; }
        const auto* candidate = &records.at(size_t(planned.candidate_id));
        DocumentBlock block;
        block.id = planned.id; block.layout_id = planned.layout_id; block.region_id = planned.region_id;
        block.type = planned.type; block.model_label = planned.model_label;
        block.box = planned.box; block.layout_box = planned.layout_box;
        block.crop_expanded = planned.crop_expanded;
        block.owned_layout_ids = planned.owned_layout_ids;
        block.candidate_id = block.mask_row = candidate->id;
        block.mask_nonzero = candidate->mask_nonzero;
        block.detection_score = candidate->score;
        block.candidate_rank = planned.candidate_rank;
        block.original_class_id = planned.original_class_id;
        block.clamped = candidate->clamped;
        std::copy(std::begin(candidate->box), std::end(candidate->box), block.raw_box);
        block.result.status = "skipped";
        if (planned.crop_expanded) {
            auto& record = records.at(size_t(planned.candidate_id));
            record.crop_expanded = true;
            record.recognition_crop = planned.box;
        }
        block.result.error = block.type == "unknown" ? "unknown_layout_class" : "recognition_not_executed";
        if (block.type == "table") block.result.format_override = "markdown";
        block.resource = identity.block_asset(block.id);
        assets.push_back({block.resource, image_resources::crop_png(image, block.box)});
        if (block.type == "image") {
            block.result.status = "ok";
            block.result.error.clear();
            block.result.assessment.state = "ok";
            block.result.assessment.reason = "resource_saved";
        }
        audit.did_crop = true;
        blocks.push_back(std::move(block));
    }
    // 识别阶段只消费冻结计划；模型返回不改变归属、身份、裁剪及顺序。
    std::map<std::string, DocumentBlock*> regions;
    for (auto& block : blocks) regions.emplace(block.region_id, &block);
    for (const auto& region_id : page_plan.structure.recognition_order) {
        auto& block = *regions.at(region_id);
        if (cancelled) { stop_after_cancel(); return execution; }
        if (transcribe && requires_ovis(block.type)) {
            const std::string request_id = "req" + block.region_id;
            RunResult::RegionRun region{request_id, "skipped", block.result.error, 0};
            if (requires_ovis(block.type)) {
                if (progress) progress("region_started", identity.progress_page(),
                                       request_id, region_done, region_total);
                auto region_start = Clock::now();
                if (recognition_unavailable) {
                    block.result.status = "skipped";
                    block.result.error = "recognition_unavailable_after_backend_failure";
                    region.stop_reason = "backend_unavailable";
                } else try {
                    block.recognition = recognize_region(*backend, image_resources::crop(image, block.box), block.box,
                        block.type, request_id, *plan, inference_context);
                    region.recognition_json = recognition_json(block.recognition);
                    recognition_unavailable = block.recognition.backend_unavailable;
                    if (cancelled && block.recognition.attempts.empty()) {
                        region.status = "cancelled"; region.stop_reason = "cancelled_before_attempt";
                        audit.regions.push_back(std::move(region));
                        stop_after_cancel(); return execution;
                    }
                    audit.reset_failed |= block.recognition.reset_failed;
                    audit.did_reset |= !block.recognition.reset_failed && !block.recognition.attempts.empty();
                    audit.did_recognition |= !block.recognition.reset_failed && !block.recognition.attempts.empty();
                    {
                        auto* generation = block.recognition.attempts.empty() ? nullptr :
                            &block.recognition.attempts.back().output;
                        if (cancelled && generation && generation->finish_reason == "failed" &&
                            generation->stop_reason == "cancelled") {
                            region.status = "cancelled";
                            region.stop_reason = generation->stop_reason;
                            region.elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - region_start).count());
                            audit.recognition_ms += region.elapsed_ms;
                            audit.regions.push_back(std::move(region));
                            stop_after_cancel();
                            return execution;
                        }
                        block.result = finalize_region_recognition(block.type, block.recognition);
                        if (!generation) {
                            recognition_unavailable = true;
                            region.stop_reason = "error";
                        } else {
                            region.stop_reason = generation->stop_reason;
                            region.elapsed_ms = generation->elapsed_ms;
                        }
                    }
                } catch (const std::bad_alloc&) { throw; }
                  catch (const std::exception& e) {
                    recognition_unavailable = true;
                    block.result.status = "failed";
                    std::string detail = e.what();
                    if (valid_utf8(detail)) block.result.error = "region_inference_exception:" + detail;
                    else {
                        block.result.error = "region_inference_exception:invalid_utf8";
                        block.result.error_base64 = base64(detail);
                    }
                    region.stop_reason = "error";
                }
                region.elapsed_ms = std::max(region.elapsed_ms, uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-region_start).count()));
                if (block.result.assessment.state == "skipped") {
                    block.result.assessment.state = block.result.status;
                    block.result.assessment.reason = block.result.error;
                    block.result.assessment.stop_reason = region.stop_reason;
                }
                region.status = block.result.status;
                audit.recognition_ms += region.elapsed_ms;
                if (block.result.status == "failed" && first_region_error_code.empty()) {
                    first_region_error_code = block.result.error == "region_reset_failed" ? "region_reset_failed" :
                        block.result.error == "generation_response_type_mismatch" ? "generation_response_type_mismatch" :
                        region.stop_reason == "timeout" ? "region_backend_timeout" :
                        block.result.error.rfind("region_inference_exception:", 0) == 0 ?
                            "region_inference_exception" : "region_inference_failed";
                    first_region_error_message = block.result.error;
                }
                if (cancelled) {
                    if (block.result.status != "failed") {
                        region.status = "cancelled";
                        region.stop_reason = "cancelled_after_backend_call";
                    }
                    audit.regions.push_back(std::move(region));
                    stop_after_cancel();
                    return execution;
                }
            }
            audit.regions.push_back(std::move(region));
            if (requires_ovis(block.type)) {
                ++region_done;
                if (progress) progress("region_completed", identity.progress_page(),
                                       request_id, region_done, region_total);
            }
        }
    }
    for (auto& block : blocks)
        if (block.result.assessment.state == "skipped") block.result.assessment.reason = block.result.error;
    if (cancelled) { stop_after_cancel(); return execution; }
    execution.completed = true;
    return execution;
}

PageExecutionResult execute_generic_regions(const PagePipelineContext& context,
    const std::vector<GenericLayoutCandidate>& candidates, RunResult initial_audit) {
    using Clock = std::chrono::steady_clock;
    PageExecutionResult execution;
    execution.audit = std::move(initial_audit);
    auto& audit = execution.audit;
    auto& blocks = execution.blocks;
    auto& assets = execution.assets;
    auto& region_total = execution.region_total;
    auto& region_done = execution.region_done;
    auto* backend = &context.backend;
    const auto& image = context.image;
    const auto& identity = context.identity;
    const auto* plan = context.plan;
    auto& cancelled = context.cancelled;
    const auto& progress = context.progress;
    ExecutionContext inference_context{cancelled};
    const auto start = Clock::now();
    audit.did_recognition = !candidates.empty();
    bool recognition_unavailable = false;
    for (const auto& candidate : candidates)
        if (candidate.label == "text" || candidate.label == "formula" || candidate.label == "table") ++region_total;
    std::string first_region_error_code, first_region_error_message;
    auto stop_after_cancel = [&] {
        audit.code = first_region_error_code.empty() ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = first_region_error_code;
        audit.error_message = first_region_error_message;
    };
    for (const auto& candidate : candidates) {
        if (cancelled) { stop_after_cancel(); return execution; }
        size_t n = blocks.size() + 1;
        DocumentBlock block;
        block.id = identity.id('b', n); block.layout_id = identity.id('l', n); block.region_id = identity.id('r', n);
        block.type = candidate.label; block.box = candidate.box;
        block.detection_score = candidate.detection_score;
        block.candidate_rank = candidate.rank;
        block.original_class_id = candidate.class_id;
        block.result.status = "ok";
        if (block.type == "image" || block.type == "chart") {
            block.type = "image";
            block.resource = identity.block_asset(block.id);
            audit.did_crop = true;
            assets.push_back({block.resource, image_resources::crop_png(image, block.box)});
        } else if (block.type == "text" || block.type == "formula" || block.type == "table") {
            const std::string request_id = "req" + block.region_id;
            if (progress) progress("region_started", identity.progress_page(),
                                   request_id, region_done, region_total);
            Image crop = image_resources::crop(image, block.box);
            audit.did_crop = true;
            auto recognition = recognize_generic_region(*backend, std::move(crop), block.box,
                block.type, request_id, plan ? plan->max_new_tokens : 4096,
                recognition_unavailable, inference_context);
            recognition_unavailable = recognition.backend_unavailable;
            audit.reset_failed |= recognition.reset_failed;
            audit.did_reset |= recognition.reset_completed;
            const auto& generation = recognition.output;
            const auto& local_error = recognition.error;
            if (cancelled) {
                bool malformed = !valid_utf8(generation.text) || !valid_utf8(generation.raw_output) ||
                                 !valid_utf8(generation.error);
                bool failed_response = generation.finish_reason != "complete" &&
                                       generation.finish_reason != "truncated" &&
                                       !(generation.finish_reason == "failed" &&
                                         generation.stop_reason == "cancelled");
                if (!local_error.empty() || malformed || failed_response) {
                    if (first_region_error_code.empty()) {
                        first_region_error_code = audit.reset_failed ? "region_reset_failed" :
                            local_error == "generation response type mismatch" ?
                                "generation_response_type_mismatch" :
                            !local_error.empty() ? "region_inference_exception" :
                            malformed ? "region_invalid_backend_utf8" :
                            generation.stop_reason == "timeout" ? "region_backend_timeout" :
                                "region_inference_failed";
                        first_region_error_message = !local_error.empty() ? local_error :
                            malformed ? "invalid backend UTF-8" :
                            generation.error.empty() ? "recognition incomplete" : generation.error;
                    }
                    audit.regions.push_back({request_id, "failed",
                        generation.stop_reason.empty() ? "backend_error" : generation.stop_reason, 0});
                } else audit.regions.push_back({request_id, "cancelled",
                    generation.stop_reason == "cancelled" ? "cancelled" :
                    "cancelled_after_backend_call", 0});
                stop_after_cancel();
                return execution;
            }
            block.result = finalize_generic_recognition(recognition);
            if (block.result.status != "ok") {
                block.resource = identity.block_asset(block.id);
                audit.did_crop = true;
                assets.push_back({block.resource, image_resources::crop_png(image, block.box)});
            }
            if (block.result.status == "failed" && first_region_error_code.empty()) {
                first_region_error_code = audit.reset_failed ? "region_reset_failed" :
                    local_error == "generation response type mismatch" ?
                        "generation_response_type_mismatch" :
                    !local_error.empty() ? "region_inference_exception" :
                    generation.stop_reason == "timeout" ? "region_backend_timeout" :
                        "region_inference_failed";
                first_region_error_message = block.result.error;
            }
            ++region_done;
            if (progress) progress("region_completed", identity.progress_page(),
                                   request_id, region_done, region_total);
        } else {
            block.result.status = "skipped";
            block.result.error = "unsupported layout label";
            block.resource = identity.block_asset(block.id);
            audit.did_crop = true;
            assets.push_back({block.resource, image_resources::crop_png(image, block.box)});
        }
        blocks.push_back(std::move(block));
        if (cancelled) { stop_after_cancel(); return execution; }
    }
    if (cancelled) { stop_after_cancel(); return execution; }
    audit.recognition_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    execution.completed = true;
    return execution;
}
} // namespace dococr
