#pragma once
#include "region_structure.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dococr {
// 已完成候选筛选的页面几何；不包含识别内容、图片像素或资源存储。
struct PageLayoutCandidate {
    int id = 0, class_id = 0, rank = 0;
    Box crop;
};
struct PagePlanInput {
    int width = 0, height = 0;
    uint32_t source_page = 0; // 0 为单张图片；PDF 页号从 1 开始。
    bool resolve_content_ownership = true; // 仅版面输出关闭归属合并；是否执行识别由作业控制。
    std::vector<PageLayoutCandidate> candidates;
};
struct PlannedRegion {
    int candidate_id = -1, original_class_id = 0, candidate_rank = 0;
    std::string id, layout_id, region_id, type, model_label;
    Box box, layout_box;
    bool crop_expanded = false;
    std::vector<std::string> owned_layout_ids;
};
struct ContentOwnership {
    int candidate_id = -1, owner_candidate_id = -1;
    std::string layout_id, owner_block_id, handling_reason;
};
struct ReadingOrderEvidence {
    std::string source = "geometry", reason = "page_geometry";
};
struct PreRecognitionPagePlan {
    // 候选与区域按稳定身份顺序存放；结构中的三份顺序单独记录。
    std::vector<int> candidate_order;
    std::vector<PlannedRegion> regions;
    std::vector<ContentOwnership> ownership;
    ReadingOrderEvidence reading_order_evidence;
    RegionStructure structure;
};
// 一次完成归属、身份、裁剪及顺序。调用方持有 const 结果，识别结果不参与规划。
PreRecognitionPagePlan plan_page(const PagePlanInput& input);
}
