#include "page_plan.hpp"
#include "layout_geometry.hpp"
#include "pdf_page_id.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dococr {
namespace {
using page_plan_detail::box_area;
using page_plan_detail::covered_fraction;
using page_plan_detail::overlapping_text_line;
using page_plan_detail::spans_columns;
std::string canonical_label(int class_id) {
    return region_type_name(layout_region_policy(class_id).type);
}
const Box& layout_geometry(const PlannedRegion& region) {
    return region.layout_box;
}
std::string identifier(uint32_t page, char kind, size_t index) {
    std::ostringstream out;
    if (page) out << pdf_page_id(page) << '-';
    out << kind << std::setw(4) << std::setfill('0') << index;
    return out.str();
}
int table_owner(const PageLayoutCandidate& child,
                const std::vector<const PageLayoutCandidate*>& selected) {
    const std::string label = canonical_label(child.class_id);
    if (label != "text" && label != "formula") return -1;
    int owner = -1, ties = 0;
    int64_t smallest_area = std::numeric_limits<int64_t>::max();
    for (const auto* candidate : selected) {
        if (canonical_label(candidate->class_id) != "table" ||
            covered_fraction(candidate->crop, child.crop) < 0.9) continue;
        const auto area = box_area(candidate->crop);
        if (area <= box_area(child.crop)) continue;
        if (area < smallest_area) {
            smallest_area = area; owner = candidate->id; ties = 1;
        } else if (area == smallest_area) ++ties;
    }
    return ties == 1 ? owner : -1;
}

int inline_formula_owner(const PageLayoutCandidate& formula,
                         const std::vector<const PageLayoutCandidate*>& selected,
                         const std::map<int, int>& table_owners) {
    if (canonical_label(formula.class_id) != "formula") return -1;
    int owner = -1, ties = 0;
    int64_t smallest_area = std::numeric_limits<int64_t>::max();
    for (const auto* candidate : selected) {
        if (canonical_label(candidate->class_id) != "text" ||
            table_owners.at(candidate->id) >= 0 ||
            candidate->class_id == 11 || candidate->class_id == 16 ||
            covered_fraction(candidate->crop, formula.crop) < 0.85) continue;
        const auto area = box_area(candidate->crop);
        if (area <= box_area(formula.crop)) continue;
        if (area < smallest_area) {
            smallest_area = area;
            owner = candidate->id;
            ties = 1;
        } else if (area == smallest_area) ++ties;
    }
    return ties == 1 ? owner : -1;
}

// Assign order to already identified blocks. Detection and recognition IDs never depend on this pass.
ReadingOrderEvidence arrange_reading_order(std::vector<PlannedRegion>& blocks, int page_width,
                                         const std::set<std::string>& horizontal_groups) {
    if (blocks.empty()) return {"geometry", "empty_page"};
    const size_t size = blocks.size();
    int left_edge = page_width;
    std::vector<size_t> right_body;
    for (size_t i = 0; i < size; ++i) {
        const PlannedRegion& block = blocks[i];
        if (layout_geometry(block).x0 < page_width * 0.45)
            left_edge = std::min(left_edge, layout_geometry(block).x0);
        if (layout_geometry(block).x0 >= page_width * 0.50 && block.model_label != "header" &&
            block.model_label != "footer" && block.model_label != "footnote" &&
            block.model_label != "vision_footnote") right_body.push_back(i);
    }
    auto section_title = [&](const PlannedRegion& title) {
        if (right_body.empty() ||
            (title.model_label != "paragraph_title" && title.model_label != "doc_title" &&
             !horizontal_groups.count(title.id)) ||
            layout_geometry(title).x0 > left_edge + std::max(8, int(page_width * 0.03))) return false;
        const int height = layout_geometry(title).y1 - layout_geometry(title).y0;
        // A left-aligned section heading can be shorter than the column gap.
        // Treat it as a page-wide break only when the right column is clear
        // for at least its own height above and below the heading.
        for (size_t i : right_body)
            if (layout_geometry(blocks[i]).y1 >= layout_geometry(title).y0 - height &&
                layout_geometry(blocks[i]).y0 <= layout_geometry(title).y1 + height) return false;
        return true;
    };
    std::vector<size_t> spans;
    for (size_t i = 0; i < size; ++i)
        if (page_width >= 64 &&
            (spans_columns(layout_geometry(blocks[i]), blocks[i].model_label,
                           page_width, horizontal_groups.count(blocks[i].id) != 0) ||
             section_title(blocks[i]))) spans.push_back(i);
    std::stable_sort(spans.begin(), spans.end(), [&](size_t a, size_t b) {
        return layout_geometry(blocks[a]).y0 < layout_geometry(blocks[b]).y0;
    });
    std::vector<int> segment(size), column(size, 0);
    std::vector<bool> is_span(size, false);
    for (size_t i : spans) is_span[i] = true;
    for (size_t i = 0; i < size; ++i) {
        int before = 0;
        for (size_t span : spans) if (layout_geometry(blocks[span]).y0 < layout_geometry(blocks[i]).y0) ++before;
        segment[i] = 2 * before + (is_span[i] ? 1 : 0);
    }
    for (int band = 0; band <= 2 * int(spans.size()); band += 2) {
        if (page_width < 64) continue;
        bool left = false, right = false;
        for (size_t i = 0; i < size; ++i) if (segment[i] == band) {
            const auto purpose = layout_region_policy(blocks[i].original_class_id).purpose;
            // Marginal labels cannot establish a second prose column.
            if (purpose == LayoutPurpose::PageMarker || purpose == LayoutPurpose::Number ||
                purpose == LayoutPurpose::Footnote || purpose == LayoutPurpose::Aside) continue;
            if (layout_geometry(blocks[i]).x1 <= page_width * 0.50) left = true;
            else if (layout_geometry(blocks[i]).x0 >= page_width * 0.50) right = true;
        }
        if (!left || !right) continue;
        for (size_t i = 0; i < size; ++i) if (segment[i] == band)
            column[i] = layout_geometry(blocks[i]).x1 <= page_width * 0.50 ? 0 :
                layout_geometry(blocks[i]).x0 >= page_width * 0.50 ? 1 : 2;
    }
    std::vector<size_t> geometry(size);
    std::iota(geometry.begin(), geometry.end(), 0);
    // 图片及短图注的一像素top抖动不能改变同一行的左右顺序。
    // 固定行锚点先生成整数排序键；不用带容差的比较器，也不链式合并行。
    std::vector<int> row_top(size);
    for (size_t i = 0; i < size; ++i) row_top[i] = layout_geometry(blocks[i]).y0;
    std::stable_sort(geometry.begin(), geometry.end(), [&](size_t a, size_t b) {
        return row_top[a] < row_top[b];
    });
    std::vector<size_t> anchors;
    for (size_t i : geometry) {
        const PlannedRegion& block = blocks[i];
        if (block.type != "image" && block.model_label != "figure_title" &&
            block.model_label != "vision_footnote" && block.model_label != "formula_number" &&
            block.model_label != "number") continue;
        const Box& bounds = layout_geometry(block);
        auto anchor = std::find_if(anchors.rbegin(), anchors.rend(), [&](size_t a) {
            const Box& other = layout_geometry(blocks[a]);
            return segment[a] == segment[i] && column[a] == column[i] && blocks[a].type == block.type &&
                bounds.y0 - other.y0 <= std::max(2, std::min(bounds.y1-bounds.y0, other.y1-other.y0)/10);
        });
        if (anchor == anchors.rend()) anchors.push_back(i);
        else row_top[i] = row_top[*anchor];
    }
    std::stable_sort(geometry.begin(), geometry.end(), [&](size_t a, size_t b) {
        if (segment[a] != segment[b]) return segment[a] < segment[b];
        if (column[a] != column[b]) return column[a] < column[b];
        if (row_top[a] != row_top[b]) return row_top[a] < row_top[b];
        if (layout_geometry(blocks[a]).x0 != layout_geometry(blocks[b]).x0) return layout_geometry(blocks[a]).x0 < layout_geometry(blocks[b]).x0;
        return a < b;
    });
    ReadingOrderEvidence evidence;
    std::set<int> ranks;
    bool missing = false, duplicate = false;
    for (const PlannedRegion& block : blocks) {
        if (block.candidate_rank < 0) missing = true;
        else if (!ranks.insert(block.candidate_rank).second) duplicate = true;
    }
    std::vector<size_t> chosen = geometry;
    if (missing) evidence.reason = "missing_rank";
    else if (duplicate) evidence.reason = "duplicate_rank";
    else {
        std::vector<size_t> ranked(size);
        std::iota(ranked.begin(), ranked.end(), 0);
        std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
            return blocks[a].candidate_rank < blocks[b].candidate_rank;
        });
        bool conflict = false, column_conflict = false;
        for (size_t i = 1; i < size; ++i) {
            size_t previous = ranked[i-1], current = ranked[i];
            if (segment[previous] > segment[current]) conflict = true;
            if (segment[previous] == segment[current] && column[previous] > column[current])
                column_conflict = true;
            if (segment[previous] == segment[current] && column[previous] == column[current] &&
                layout_geometry(blocks[previous]).y0 > layout_geometry(blocks[current]).y0 &&
                layout_geometry(blocks[previous]).y0 >= layout_geometry(blocks[current]).y1)
                conflict = true;
        }
        if (column_conflict) evidence.reason = "model_column_conflict";
        else if (conflict) evidence.reason = "model_geometry_conflict";
        else { evidence = {"model", "unique_rank"}; chosen = std::move(ranked); }
    }
    std::vector<PlannedRegion> reordered;
    reordered.reserve(size);
    for (size_t i : chosen) reordered.push_back(std::move(blocks[i]));
    blocks = std::move(reordered);
    return evidence;
}

RegionStructure arrange_structure(const std::vector<PlannedRegion>& blocks, int width, int height,
                                  ReadingOrderEvidence& evidence) {
    std::vector<StructureItem> items;
    std::map<std::string, size_t> by_id;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        by_id[b.id] = i;
        items.push_back({b.id, b.region_id, b.type, layout_region_policy(b.original_class_id).purpose, layout_geometry(b)});
    }
    RegionStructure plan = plan_image_structure(items, width, height);
    std::map<std::string, std::vector<std::string>> members;
    std::set<std::string> contained;
    std::map<std::string, Box> group_bounds;
    for (auto& group : plan.groups) {
        for (const auto& item : group.items) {
            for (const auto& bid : {item.image_block_id, item.caption_block_id}) {
                if (bid.empty()) continue;
                const Box crop = blocks.at(by_id.at(bid)).box;
                group.bbox = {std::min(group.bbox.x0, crop.x0), std::min(group.bbox.y0, crop.y0),
                              std::max(group.bbox.x1, crop.x1), std::max(group.bbox.y1, crop.y1)};
            }
        }
        const std::string leader = group.items.front().image_block_id;
        group_bounds[leader] = group.bbox;
        for (const auto& item : group.items) {
            members[leader].push_back(item.image_block_id);
            contained.insert(item.image_block_id);
            if (!item.caption_block_id.empty()) {
                members[leader].push_back(item.caption_block_id);
                contained.insert(item.caption_block_id);
            }
        }
    }
    for (const auto& caption : plan.captions) {
        if (contained.count(caption.image_block_id)) continue;
        members[caption.image_block_id] = {caption.image_block_id, caption.caption_block_id};
        contained.insert(caption.image_block_id);
        contained.insert(caption.caption_block_id);
    }
    std::vector<PlannedRegion> units;
    std::set<std::string> horizontal_groups;
    for (const auto& block : blocks) {
        if (contained.count(block.id) && !members.count(block.id)) continue;
        units.push_back(block);
        if (group_bounds.count(block.id)) {
            auto& unit = units.back();
            unit.box = unit.layout_box = group_bounds.at(block.id);
            unit.crop_expanded = false;
            // A supported horizontal row is one structural unit, not several columns.
            horizontal_groups.insert(unit.id);
        }
    }
    evidence = arrange_reading_order(units, width, horizontal_groups);
    if (!plan.groups.empty()) evidence = {"geometry", "planned_horizontal_groups"};
    for (const auto& unit : units) {
        const auto ids = members.count(unit.id) ? members.at(unit.id) : std::vector<std::string>{unit.id};
        for (const auto& bid : ids) {
            const auto& block = blocks.at(by_id.at(bid));
            plan.block_order.push_back(block.id);
            plan.region_order.push_back(block.region_id);
            if (requires_ovis(block.type)) plan.recognition_order.push_back(block.region_id);
        }
    }
    return plan;
}

}

PreRecognitionPagePlan plan_page(const PagePlanInput& input) {
    if (input.width <= 0 || input.height <= 0)
        throw std::invalid_argument("page_plan_invalid_dimensions");
    std::map<int, const PageLayoutCandidate*> by_id;
    std::vector<const PageLayoutCandidate*> selected;
    for (const auto& candidate : input.candidates) {
        const Box b = candidate.crop;
        if (candidate.id < 0 || !by_id.emplace(candidate.id, &candidate).second)
            throw std::invalid_argument("page_plan_duplicate_or_invalid_candidate_id");
        if (b.x0 < 0 || b.y0 < 0 || b.x1 > input.width || b.y1 > input.height ||
            b.x0 >= b.x1 || b.y0 >= b.y1)
            throw std::invalid_argument("page_plan_invalid_candidate_box");
        selected.push_back(&candidate);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
        if (a->crop.y0 != b->crop.y0) return a->crop.y0 < b->crop.y0;
        return a->crop.x0 < b->crop.x0;
    });
    PreRecognitionPagePlan result;
    std::map<int, int> table_owners, text_owners;
    for (const auto* candidate : selected) {
        result.candidate_order.push_back(candidate->id);
        table_owners[candidate->id] = text_owners[candidate->id] = -1;
    }
    auto own = [&](const PageLayoutCandidate& child, int owner, const char* reason) {
        result.ownership.push_back({child.id, owner,
            identifier(input.source_page, 'l', selected.size() + size_t(child.id) + 1), {}, reason});
    };
    if (input.resolve_content_ownership) {
        for (const auto* candidate : selected) {
            const int owner = table_owner(*candidate, selected);
            if (owner < 0) continue;
            table_owners[candidate->id] = owner;
            own(*candidate, owner, "table_content_owned_by_table");
        }
        for (const auto* child : selected) {
            if (table_owners.at(child->id) >= 0) continue;
            int owner = -1, count = 0;
            for (const auto* parent : selected) {
                if (table_owners.at(parent->id) >= 0 || !overlapping_text_line(*parent, *child)) continue;
                owner = parent->id;
                ++count;
            }
            if (count == 1) text_owners[child->id] = owner;
        }
        // 父区域高度严格增加；先将文本归属链归到根，再处理行内公式。
        auto text_root = [&](int owner) {
            while (owner >= 0 && text_owners.at(owner) >= 0) owner = text_owners.at(owner);
            return owner;
        };
        for (const auto* child : selected) {
            if (text_owners.at(child->id) < 0) continue;
            own(*child, text_root(child->id), "overlapping_text_owned_by_text");
        }
        for (const auto* candidate : selected) {
            if (table_owners.at(candidate->id) >= 0) continue;
            const int owner = text_root(inline_formula_owner(*candidate, selected, table_owners));
            if (owner >= 0) own(*candidate, owner, "inline_formula_owned_by_text");
        }
    }
    std::set<int> owned;
    for (const auto& relation : result.ownership) owned.insert(relation.candidate_id);
    for (const auto* candidate : selected) {
        if (owned.count(candidate->id)) continue;
        PlannedRegion region;
        const size_t n = result.regions.size() + 1;
        region.id = identifier(input.source_page, 'b', n);
        region.layout_id = identifier(input.source_page, 'l', n);
        region.region_id = identifier(input.source_page, 'r', n);
        region.candidate_id = candidate->id;
        region.original_class_id = candidate->class_id;
        region.candidate_rank = candidate->rank;
        region.type = canonical_label(candidate->class_id);
        const auto* label = layout_region_policy(candidate->class_id).model_label;
        region.model_label = label ? label : "unknown";
        region.box = region.layout_box = candidate->crop;
        for (const auto& relation : result.ownership) {
            if (relation.owner_candidate_id != candidate->id) continue;
            region.owned_layout_ids.push_back(relation.layout_id);
            const Box child = by_id.at(relation.candidate_id)->crop;
            region.box = {std::min(region.box.x0, child.x0), std::min(region.box.y0, child.y0),
                          std::max(region.box.x1, child.x1), std::max(region.box.y1, child.y1)};
        }
        region.crop_expanded = region.box.x0 != region.layout_box.x0 || region.box.y0 != region.layout_box.y0 ||
                               region.box.x1 != region.layout_box.x1 || region.box.y1 != region.layout_box.y1;
        result.regions.push_back(std::move(region));
    }
    for (auto& relation : result.ownership) {
        const auto owner = std::find_if(result.regions.begin(), result.regions.end(), [&](const PlannedRegion& region) {
            return region.candidate_id == relation.owner_candidate_id;
        });
        if (owner == result.regions.end()) throw std::runtime_error("content_owner_missing");
        relation.owner_block_id = owner->id;
    }
    result.structure = arrange_structure(result.regions, input.width, input.height, result.reading_order_evidence);
    return result;
}
}
