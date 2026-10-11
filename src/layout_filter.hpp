#pragma once
#include "layout_candidate.hpp"

namespace dococr {
// 输入已通过张量契约、分数与几何安全检查；选中记录的 crop 已裁到有效页面。
// 按 NMS/大图片、包含、外层重叠的顺序筛选，只更新 selected 和 reason。
// 保留所有记录、身份、存储顺序及几何；两边至少 6 像素才执行后两步。
// 按值接收，可移动转交；返回完整记录以供复核、页面规划和导出追溯。
std::vector<LayoutCandidateRecord> filter_layout_candidates(
    int page_width, int page_height, std::vector<LayoutCandidateRecord> candidates);
}
