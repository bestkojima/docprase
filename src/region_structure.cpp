#include "region_structure.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace dococr {
namespace {
int width(Box b) { return b.x1 - b.x0; }
int height(Box b) { return b.y1 - b.y0; }
Box united(Box a, Box b) {
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0),
            std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
bool caption_shape(const StructureItem& caption, const StructureItem& image, int page_width) {
    if (caption.type != "text") return false;
    if (caption.purpose == LayoutPurpose::Annotation)
        return width(caption.bbox) <= width(image.bbox) * 1.4 &&
               height(caption.bbox) <= std::max(8, int(height(image.bbox) * .6));
    if (caption.purpose != LayoutPurpose::Body && caption.purpose != LayoutPurpose::Number) return false;
    return width(caption.bbox) <= std::min(width(image.bbox) * .45, page_width * .09) &&
           height(caption.bbox) <= std::max(8, int(height(image.bbox) * .3));
}
}

RegionStructure plan_image_structure(const std::vector<StructureItem>& items,
                                     int page_width, int page_height) {
    RegionStructure plan;
    if (page_width < 64) return plan;
    std::vector<size_t> images;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].type == "image") images.push_back(i);
    // Each candidate caption needs a unique geometric owner; competing captions
    // for the same image remain unresolved rather than choosing by storage order.
    std::map<size_t, std::vector<size_t>> proposed;
    for (size_t c = 0; c < items.size(); ++c) {
        int best_gap = std::numeric_limits<int>::max();
        double best_offset = std::numeric_limits<double>::max();
        size_t best = items.size();
        bool tie = false;
        for (size_t i : images) {
            const Box cap = items[c].bbox, image = items[i].bbox;
            if (!caption_shape(items[c], items[i], page_width)) continue;
            const int edge_tolerance = std::max(2, height(cap) / 10);
            const bool below = cap.y0 >= image.y1 - edge_tolerance;
            const bool above = cap.y1 <= image.y0 + edge_tolerance;
            int gap;
            double offset;
            if (below || (above && items[c].purpose == LayoutPurpose::Annotation)) {
                const int overlap = std::min(cap.x1, image.x1) - std::max(cap.x0, image.x0);
                if (overlap < width(cap) * .5) continue;
                gap = std::max(0, below ? cap.y0 - image.y1 : image.y0 - cap.y1);
                if (gap > std::max(8, std::min(int(page_height * .04), height(image) / 2))) continue;
                offset = std::abs((cap.x0 + cap.x1 - image.x0 - image.x1) / 2.);
            } else {
                // Compact side labels such as a subquestion number belong to
                // the adjacent drawing, not after the entire option row.
                const int overlap = std::min(cap.y1, image.y1) - std::max(cap.y0, image.y0);
                if (overlap < height(cap) * .8 || (cap.x1 > image.x0 && cap.x0 < image.x1) ||
                    width(cap) > std::min(width(image) * .45, page_width * .09) ||
                    height(cap) > std::max(8, int(height(image) * .3))) continue;
                gap = cap.x1 <= image.x0 ? image.x0 - cap.x1 : cap.x0 - image.x1;
                offset = std::abs((cap.y0 + cap.y1 - image.y0 - image.y1) / 2.);
                if (gap > std::max(8, std::min(int(page_width * .035), width(image) / 2)) ||
                    offset > height(image) * .3) continue;
            }
            if (gap < best_gap || (gap == best_gap && offset < best_offset)) {
                best = i; best_gap = gap; best_offset = offset; tie = false;
            } else if (gap == best_gap && offset == best_offset) tie = true;
        }
        if (best == items.size()) continue;
        if (tie) plan.ambiguous_caption_ids.push_back(items[c].block_id);
        else proposed[best].push_back(c);
    }
    std::map<size_t, size_t> caption_for;
    for (const auto& proposal : proposed) {
        if (proposal.second.size() == 1) {
            caption_for[proposal.first] = proposal.second.front();
        } else {
            for (size_t c : proposal.second) plan.ambiguous_caption_ids.push_back(items[c].block_id);
        }
    }
    std::stable_sort(images.begin(), images.end(), [&](size_t a, size_t b) {
        if (items[a].bbox.y0 != items[b].bbox.y0) return items[a].bbox.y0 < items[b].bbox.y0;
        return items[a].bbox.x0 < items[b].bbox.x0;
    });
    std::set<size_t> grouped;
    for (size_t anchor : images) {
        if (grouped.count(anchor)) continue;
        const Box start = items[anchor].bbox;
        std::vector<size_t> row;
        for (size_t i : images) {
            if (grouped.count(i)) continue;
            const Box other = items[i].bbox;
            const int small_height = std::min(height(start), height(other));
            // Fixed anchor prevents tolerance from chaining across rows/questions.
            if (std::abs(other.y0 - start.y0) > std::max(2, small_height / 5) ||
                std::max(height(start), height(other)) > small_height * 2) continue;
            row.push_back(i);
        }
        if (row.size() < 2) continue;
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return items[a].bbox.x0 < items[b].bbox.x0; });
        bool disjoint = true;
        Box bounds = items[row.front()].bbox;
        size_t captions = 0;
        for (size_t k = 0; k < row.size(); ++k) {
            if (k && items[row[k-1]].bbox.x1 > items[row[k]].bbox.x0) disjoint = false;
            bounds = united(bounds, items[row[k]].bbox);
            captions += caption_for.count(row[k]);
        }
        if (!disjoint) continue;
        // A shared preceding body block distinguishes a question's image row
        // from illustrations belonging to separate prose columns.
        bool common_body = false;
        for (const auto& item : items) {
            if (item.type != "text" || (item.purpose != LayoutPurpose::Body && item.purpose != LayoutPurpose::Heading)) continue;
            const int gap = bounds.y0 - item.bbox.y1;
            if (gap >= 0 && gap <= std::max(12, std::min(int(page_height * .1), height(start))) &&
                item.bbox.x0 <= bounds.x0 + page_width * .05 &&
                item.bbox.x1 >= bounds.x1 - page_width * .05) common_body = true;
        }
        if (!(common_body || (row.size() >= 3 && captions >= 2 && width(bounds) >= page_width * .5))) continue;
        ImageGroup group;
        group.id = "images-" + items[row.front()].region_id;
        group.bbox = bounds;
        for (size_t i : row) {
            grouped.insert(i);
            ImageCaption member{items[i].block_id, ""};
            if (caption_for.count(i)) {
                member.caption_block_id = items[caption_for[i]].block_id;
                group.bbox = united(group.bbox, items[caption_for[i]].bbox);
            }
            group.items.push_back(std::move(member));
        }
        plan.groups.push_back(std::move(group));
    }
    // Compact text can also label a single image when it is centered below it.
    // Page markers, headings and actual footnotes are excluded by the shared policy.
    for (const auto& pair : caption_for) {
        const auto& image = items[pair.first];
        const auto& caption = items[pair.second];
        const bool centered_below = caption.bbox.y0 >= image.bbox.y1 - std::max(2, height(caption.bbox) / 10) &&
            std::abs(caption.bbox.x0 + caption.bbox.x1 - image.bbox.x0 - image.bbox.x1) <= width(image.bbox) * .3;
        if (grouped.count(pair.first) || caption.purpose == LayoutPurpose::Annotation || centered_below)
            plan.captions.push_back({items[pair.first].block_id, items[pair.second].block_id});
    }
    return plan;
}
}
