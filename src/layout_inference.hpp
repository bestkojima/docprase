#pragma once
#include "page_pipeline_context.hpp"
#include "layout_output.hpp"

namespace dococr {
struct DocLayoutInference {
    LayoutPageTransform transform;
    Image preview;
    DecodedLayoutOutput output;
};
// 成功结果拥有预处理变换、preview 和原始模型输出，供后续资源处理借用。
// 失败通过 audit 保持各路径原错误、取消和输入诊断资源规则，bad_alloc 重抛。
std::optional<DocLayoutInference> infer_doclayout(const PagePipelineContext& context,
    bool transcribe, RunResult& audit);
// 通用布局异常继续向调用方传播；非法输出仍重置为原 fresh Failed 审计。
std::optional<std::vector<GenericLayoutCandidate>> infer_generic_layout(
    const PagePipelineContext& context, RunResult& audit);
}
