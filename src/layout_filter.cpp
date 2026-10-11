#include "layout_filter.hpp"
#include "layout_geometry.hpp"
#include <algorithm>

namespace dococr {
namespace {
using page_plan_detail::box_area;
using page_plan_detail::covered_fraction;
using page_plan_detail::overlapping_text_line;

// PaddleX release/3.7 object_detection.processors.nms, applied to the raw model boxes.
void deduplicate_layout(std::vector<LayoutCandidateRecord>& records, int page_width, int page_height) {
    std::vector<size_t> order;
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].selected) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (records[a].score != records[b].score) return records[a].score > records[b].score;
        return a > b; // numpy argsort(scores)[::-1] on equal scores
    });
    std::vector<size_t> kept;
    for (size_t index : order) {
        auto& candidate = records[index];
        for (size_t previous : kept) {
            const auto& winner = records[previous];
            const double left = std::max(candidate.box[0], winner.box[0]);
            const double top = std::max(candidate.box[1], winner.box[1]);
            const double right = std::min(candidate.box[2], winner.box[2]);
            const double bottom = std::min(candidate.box[3], winner.box[3]);
            // The frozen PaddleX iou() uses inclusive coordinates, even for float boxes.
            const double intersection = std::max(0.0, right-left+1) * std::max(0.0, bottom-top+1);
            const double candidate_area = double(candidate.box[2]-candidate.box[0]+1) *
                                          (candidate.box[3]-candidate.box[1]+1);
            const double winner_area = double(winner.box[2]-winner.box[0]+1) *
                                       (winner.box[3]-winner.box[1]+1);
            const double iou = intersection / (candidate_area + winner_area - intersection);
            const bool same_class = candidate.class_id == winner.class_id;
            if (iou >= (same_class ? 0.6 : 0.98)) {
                candidate.selected = false;
                candidate.reason = same_class ? "nms_same_class" : "nms_cross_class";
                break;
            }
        }
        if (candidate.selected) kept.push_back(index);
    }
    if (kept.size() <= 1) return;
    const double limit = page_width > page_height ? 0.82 : 0.93;
    std::vector<size_t> large_images;
    for (size_t index : kept) {
        auto& candidate = records[index];
        if (candidate.class_id != 14) continue; // official label "image"
        const double x0 = std::max(0.0, double(candidate.box[0]));
        const double y0 = std::max(0.0, double(candidate.box[1]));
        const double x1 = std::min(double(page_width), double(candidate.box[2]));
        const double y1 = std::min(double(page_height), double(candidate.box[3]));
        if ((x1-x0)*(y1-y0) > limit * page_width * page_height) {
            large_images.push_back(index);
        }
    }
    // PaddleX restores the input when the area filter would remove every box.
    if (large_images.size() == kept.size()) return;
    for (size_t index : large_images) {
        records[index].selected = false;
        records[index].reason = "large_page_image";
    }
}

double overlap_of_smaller(const Box& a, const Box& b) {
    const double area = std::min(box_area(a), box_area(b));
    if (area <= 0) return 0;
    const int width = std::max(0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
    const int height = std::max(0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    return double(width) * height / area;
}

// PaddleX 的 large 模式只筛除被该类别包含的框；union 不改变几何形状。
void filter_layout_containment(std::vector<LayoutCandidateRecord>& records) {
    constexpr int large_classes[] = {3, 5, 6, 15, 17};
    std::vector<bool> active;
    active.reserve(records.size());
    for (const auto& candidate : records) active.push_back(candidate.selected);
    for (size_t i = 0; i < records.size(); ++i) {
        auto& inner = records[i];
        if (!active[i]) continue;
        for (int large_class : large_classes) {
            bool contained = false;
            for (size_t j = 0; j < records.size(); ++j) {
                const auto& outer = records[j];
                if (!active[j] || inner.id == outer.id || outer.class_id != large_class)
                    continue;
                // 官方 check_containment 对公式类别 5 的这一方向做保护。
                if (inner.class_id == 5 && outer.class_id != 5) continue;
                const double inner_area = double(inner.box[2]-inner.box[0]) *
                                          (inner.box[3]-inner.box[1]);
                const double width = std::max(0.0, double(std::min(inner.box[2], outer.box[2]) -
                                                    std::max(inner.box[0], outer.box[0])));
                const double height = std::max(0.0, double(std::min(inner.box[3], outer.box[3]) -
                                                     std::max(inner.box[1], outer.box[1])));
                if (inner_area > 0 && width*height / inner_area >= 0.9) {
                    contained = true;
                    break;
                }
            }
            if (contained) {
                inner.selected = false;
                inner.reason = "contained_by_large_class";
                break;
            }
        }
    }
}

// 外层 filter_boxes 使用裁到页面后的矩形；合法内容父子关系由区域规划负责。
void filter_layout_overlap(std::vector<LayoutCandidateRecord>& records, int page_width, int page_height) {
    if (page_width < 6 || page_height < 6) return; // 旧版 2x2 契约夹具
    for (auto& candidate : records) {
        if (!candidate.selected) continue;
        if (candidate.class_id == 18) {
            candidate.selected = false;
            candidate.reason = "outer_reference";
        }
    }
    for (size_t i = 0; i < records.size(); ++i) {
        auto& first = records[i];
        if (!first.selected) continue;
        for (size_t j = i+1; j < records.size(); ++j) {
            auto& second = records[j];
            if (!second.selected || overlap_of_smaller(first.crop, second.crop) <= 0.7) continue;
            if (overlapping_text_line(first, second) || overlapping_text_line(second, first)) continue;
            const std::string first_label = first.label, second_label = second.label;
            const bool table_child =
                (first.class_id == 21 && (second_label == "text" || second_label == "formula") &&
                 covered_fraction(first.crop, second.crop) >= 0.9) ||
                (second.class_id == 21 && (first_label == "text" || first_label == "formula") &&
                 covered_fraction(second.crop, first.crop) >= 0.9);
            const bool inline_formula =
                (first_label == "formula" && second_label == "text" &&
                 covered_fraction(second.crop, first.crop) >= 0.85) ||
                (second_label == "formula" && first_label == "text" &&
                 covered_fraction(first.crop, second.crop) >= 0.85);
            const bool annotation = first_label == "figure_title" ||
                second_label == "figure_title" || first_label == "footnote" ||
                second_label == "footnote" || first_label == "vision_footnote" ||
                second_label == "vision_footnote";
            const double size_ratio = double(std::min(box_area(first.crop), box_area(second.crop))) /
                                      std::max(box_area(first.crop), box_area(second.crop));
            if (table_child || inline_formula || (annotation && size_ratio < 0.8)) continue;
            const bool special_first = first_label == "image" || first_label == "table" ||
                                       first_label == "chart" || first_label == "seal";
            const bool special_second = second_label == "image" || second_label == "table" ||
                                        second_label == "chart" || second_label == "seal";
            if ((special_first || special_second) && first_label != second_label &&
                (first_label != "table" && second_label != "table" ||
                 (special_first && special_second))) continue;
            auto& loser = box_area(first.crop) >= box_area(second.crop) ? second : first;
            loser.selected = false;
            loser.reason = "outer_overlap";
            if (&loser == &first) break;
        }
    }
}

} // namespace

std::vector<LayoutCandidateRecord> filter_layout_candidates(
    int page_width, int page_height, std::vector<LayoutCandidateRecord> candidates) {
    deduplicate_layout(candidates, page_width, page_height);
    if (page_width >= 6 && page_height >= 6) {
        filter_layout_containment(candidates);
        filter_layout_overlap(candidates, page_width, page_height);
    }
    return candidates;
}
} // namespace dococr
