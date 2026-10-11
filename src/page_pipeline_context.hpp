#pragma once
#include "config.hpp"
#include "page_identity.hpp"

namespace dococr {
inline constexpr uint64_t max_standard_page_pixels = 16000000;
// 页面阶段借用 pipeline 持有的像素、身份、配置与回调；不得跨调用保存这些引用。
struct PagePipelineContext {
    IInferenceEngine& backend;
    const Image& image;
    const PageIdentity& identity;
    const ExecutionPlan* plan;
    std::atomic_bool& cancelled;
    const ProgressCallback& progress;
};
}
