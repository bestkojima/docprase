#pragma once
#include "page_plan.hpp"

namespace dococr {
// 候选筛选与导出诊断共享的记录；不持有张量或图片像素。
struct LayoutCandidateRecord : PageLayoutCandidate {
    int mask_nonzero = 0;
    float score = 0, box[4]{}, model_box[4]{};
    std::string label, reason, handling_reason;
    std::string mask_asset;
    bool selected = false, clamped = false;
    Box recognition_crop;
    bool crop_expanded = false;
};
}
