#pragma once
#include "dococr/inference.hpp"
#include "layout_region_policy.hpp"
#include <string>
#include <vector>

namespace dococr {
struct StructureItem {
    std::string block_id, region_id, type;
    LayoutPurpose purpose;
    Box bbox;
};
struct ImageCaption {
    std::string image_block_id, caption_block_id;
};
struct ImageGroup {
    std::string id;
    Box bbox;
    std::vector<ImageCaption> items;
};
struct RegionStructure {
    std::vector<ImageGroup> groups;
    std::vector<ImageCaption> captions;
    std::vector<std::string> ambiguous_caption_ids;
    std::vector<std::string> block_order, region_order, recognition_order;
};
// Geometry is planned before transcription. Grouping never merges recognition crops.
RegionStructure plan_image_structure(const std::vector<StructureItem>& items,
                                     int page_width, int page_height);
}
