#pragma once
#include "page_pipeline_context.hpp"
#include "document_export.hpp"
#include "layout_output.hpp"
#include "image_resources.hpp"

namespace dococr {
struct PageExecutionResult {
    std::vector<DocumentBlock> blocks;
    std::vector<Asset> assets;
    RunResult audit{RunCode::Failed, {}};
    uint32_t region_done = 0, region_total = 0;
    bool completed = false;
};
// 执行只消费已冻结计划；更新候选的资源/裁框诊断，不重算归属和顺序。
// assets 暂存，取消返回 completed=false；pipeline 决定最终提交，audit 不包含暂存资源。
PageExecutionResult execute_planned_regions(const PagePipelineContext& context,
    const PreRecognitionPagePlan& page_plan, std::vector<LayoutCandidateRecord>& records,
    const image_resources::LayoutRasterSource& raster, bool transcribe, RunResult audit);
// 通用候选已通过整页解码校验并排序；保持逐候选创建和单次识别的旧规则。
PageExecutionResult execute_generic_regions(const PagePipelineContext& context,
    const std::vector<GenericLayoutCandidate>& candidates, RunResult audit);
}
