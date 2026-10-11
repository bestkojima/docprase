#pragma once
#include "page_plan.hpp"
#include <algorithm>

namespace dococr::page_plan_detail {
// 候选筛选、页面计划和导出关联共享的私有几何规则。
inline int64_t box_area(Box box) { return int64_t(box.x1 - box.x0) * (box.y1 - box.y0); }

inline double covered_fraction(Box outer, Box inner) {
    const int64_t area = box_area(inner);
    if (area <= 0) return 0;
    const int width = std::max(0, std::min(outer.x1, inner.x1) -
                                  std::max(outer.x0, inner.x0));
    const int height = std::max(0, std::min(outer.y1, inner.y1) -
                                   std::max(outer.y0, inner.y0));
    return double(width) * height / area;
}

// A detected line can straddle a paragraph's boundary. Preserve its complete
// pixels in that paragraph instead of recognizing the clipped line twice.
// This does not merge disjoint adjacent paragraphs or different layout classes.
inline bool overlapping_text_line(const PageLayoutCandidate& paragraph, const PageLayoutCandidate& line) {
    if (paragraph.class_id != 22 || line.class_id != 22 || paragraph.id == line.id) return false;
    const Box p = paragraph.crop, l = line.crop;
    const int pw = p.x1 - p.x0, ph = p.y1 - p.y0, lw = l.x1 - l.x0, lh = l.y1 - l.y0;
    if (lh <= 0 || lw < 4 * lh || ph < 2 * lh || lw < .6 * pw || lw > 1.1 * pw) return false;
    const int horizontal = std::min(p.x1, l.x1) - std::max(p.x0, l.x0);
    return horizontal >= .9 * lw && covered_fraction(p, l) >= .4 &&
        (l.y0 <= p.y0 || l.y1 >= p.y1);
}

inline bool spans_columns(Box bounds, const std::string& label, int page_width,
                          bool horizontal_group = false) {
    const bool heading = horizontal_group || label == "doc_title" ||
        label == "paragraph_title";
    const bool page_header = label == "header";
    return bounds.x0 < page_width * 0.45 && bounds.x1 > page_width * 0.55 &&
           (heading || page_header || bounds.x1 - bounds.x0 >= page_width * 0.65);
}
}
