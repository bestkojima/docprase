#include "layout_filter.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace dococr;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
LayoutCandidateRecord candidate(int id, int cls, Box crop, float score = .9f) {
    LayoutCandidateRecord c;
    c.id = id; c.class_id = cls; c.rank = 100 - id; c.crop = crop;
    c.score = score; c.selected = true;
    const auto* label = layout_region_policy(cls).model_label;
    c.label = label ? label : "unknown";
    c.box[0] = float(crop.x0); c.box[1] = float(crop.y0);
    c.box[2] = float(crop.x1); c.box[3] = float(crop.y1);
    std::copy(c.box, c.box + 4, c.model_box);
    c.mask_nonzero = id + 1; c.mask_asset = "mask-" + std::to_string(id);
    c.handling_reason = "preserved-evidence";
    return c;
}
bool same_box(Box a, Box b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}
void unchanged_evidence(const LayoutCandidateRecord& a, const LayoutCandidateRecord& b) {
    check(a.id == b.id && a.class_id == b.class_id && a.rank == b.rank &&
          a.score == b.score && a.label == b.label && same_box(a.crop, b.crop) &&
          same_box(a.recognition_crop, b.recognition_crop) && a.crop_expanded == b.crop_expanded &&
          a.mask_nonzero == b.mask_nonzero && a.mask_asset == b.mask_asset &&
          a.handling_reason == b.handling_reason && a.clamped == b.clamped &&
          std::equal(a.box, a.box + 4, b.box) &&
          std::equal(a.model_box, a.model_box + 4, b.model_box), "筛选修改了身份、几何或资源证据");
}
void nms_and_record_preservation() {
    auto low = candidate(10, 22, {10, 10, 100, 60});
    low.rank = 0;
    auto winner = candidate(90, 22, low.crop); winner.rank = 99;
    auto rejected = candidate(120, 22, low.crop, 1);
    rejected.selected = false; rejected.reason = "below_score_threshold";
    const std::vector<LayoutCandidateRecord> input{low, winner, rejected};
    const auto output = filter_layout_candidates(1000, 800, input);
    check(output.size() == input.size() && !output[0].selected && output[1].selected &&
          output[0].reason == "nms_same_class" && !output[2].selected &&
          output[2].reason == rejected.reason, "同分 NMS 优先级或先验筛除原因改变");
    for (size_t i = 0; i < input.size(); ++i) unchanged_evidence(input[i], output[i]);
    check(input[0].selected && input[0].reason.empty(), "按值输入被改写");

    auto a = candidate(1, 22, {0, 0, 1, 1}, .9f);
    auto b = candidate(2, 22, {0, 0, 2, 1}, .8f);
    auto c = candidate(3, 22, {0, 0, 2, 1}, .7f);
    b.box[0] = .5f; b.box[2] = 1.5f;
    c.box[0] = .51f; c.box[2] = 1.51f;
    auto edge = filter_layout_candidates(2, 2, {a, b, c});
    check(edge[0].selected && edge[1].reason == "nms_same_class" && edge[2].selected,
          "同类浮点 inclusive IoU 边界改变");
    a.class_id = 5; a.label = "formula";
    b.box[0] = .02f; b.box[2] = 1.02f;
    c.box[0] = .021f; c.box[2] = 1.021f;
    edge = filter_layout_candidates(2, 2, {a, b, c});
    check(edge[0].selected && edge[1].reason == "nms_cross_class" && edge[2].selected,
          "跨类浮点 inclusive IoU 边界改变");
}
void large_image_rules() {
    auto image = candidate(0, 14, {0, 0, 200, 90});
    const auto text = candidate(1, 22, {10, 10, 50, 30});
    const auto landscape = filter_layout_candidates(200, 100, {image, text});
    check(landscape[0].reason == "large_page_image" && landscape[1].selected,
          "横页大图片面积阈值改变");
    image = candidate(0, 14, {0, 0, 164, 100});
    check(filter_layout_candidates(200, 100, {image, text})[0].selected,
          "横页 82% 面积边界应保留");
    image = candidate(0, 14, {0, 0, 100, 180});
    auto portrait = filter_layout_candidates(100, 200, {image, text});
    check(portrait[0].selected && portrait[1].selected, "竖页大图片阈值改变");
    image.box[3] = 186; image.crop.y1 = 186;
    check(filter_layout_candidates(100, 200, {image, text})[0].selected,
          "竖页 93% 面积边界应保留");
    image.box[3] = 188; image.crop.y1 = 188;
    portrait = filter_layout_candidates(100, 200, {image, text});
    check(portrait[0].reason == "large_page_image", "竖页大图片未筛除");
    check(filter_layout_candidates(100, 200, {image})[0].selected, "唯一大图片被删除");
    auto ignored = text; ignored.selected = false; ignored.reason = "outside_page";
    check(filter_layout_candidates(100, 200, {image, ignored})[0].selected,
          "大图片回退计入了已筛除候选");
    auto wide = candidate(4, 14, {0, 0, 5, 5});
    auto tall = candidate(5, 14, {0, 0, 5, 5});
    wide.box[0] = -10000; tall.box[1] = -10000;
    const auto all_large = filter_layout_candidates(5, 5, {wide, tall});
    check(all_large[0].selected && all_large[1].selected, "删除全部大图片时未回退");
}
void containment_snapshot_and_direction() {
    // A 被 B 覆盖 90%；C 只在 A 的左侧范围内。A 的筛除不能改变该轮包含证据。
    const auto a = candidate(10, 6, {0, 0, 100, 100});
    const auto b = candidate(40, 3, {10, 0, 120, 100});
    const auto c = candidate(99, 22, {0, 0, 8, 50});
    const auto output = filter_layout_candidates(200, 200, {a, b, c});
    check(output[0].reason == "contained_by_large_class" && output[1].selected &&
          output[2].reason == "contained_by_large_class", "包含快照、90% 边界或稀疏身份处理改变");
    const auto formula = candidate(123, 5, {30, 30, 50, 50});
    check(filter_layout_candidates(200, 200, {b, formula})[1].selected,
          "公式被不同 large 类别包含时未受保护");
    auto outer_formula = candidate(5, 5, {10, 10, 100, 100});
    check(filter_layout_candidates(200, 200, {outer_formula, formula})[1].reason ==
          "contained_by_large_class", "同类公式包含规则改变");
    outer_formula.class_id = 15;
    check(filter_layout_candidates(200, 200, {outer_formula, formula})[1].reason ==
          "outer_overlap", "class 5 对 class 15 的包含方向保护改变");
    auto formula15 = formula; formula15.class_id = 15;
    check(filter_layout_candidates(200, 200, {outer_formula, formula15})[1].reason ==
          "contained_by_large_class", "class 15 被错误套用 class 5 的保护");
    auto inactive = a; inactive.selected = false; inactive.reason = "outside_page";
    check(filter_layout_candidates(200, 200, {inactive, c})[1].selected,
          "先验筛除候选仍作为包含父块");
}
void ownership_and_annotation_protection() {
    const auto table = candidate(0, 21, {10, 10, 400, 300});
    const auto child = candidate(1, 22, {0, 30, 100, 60}); // 表格覆盖 90%。
    check(filter_layout_candidates(1000, 800, {table, child})[1].selected,
          "表格子内容被提前删除");
    const auto body = candidate(2, 22, {15, 10, 400, 300});
    const auto formula = candidate(3, 5, {0, 30, 100, 60}); // 正文覆盖 85%。
    check(filter_layout_candidates(1000, 800, {body, formula})[1].selected,
          "行内公式在归属前被删除");
    const auto paragraph = candidate(5, 22, {10, 30, 400, 200});
    const auto line = candidate(6, 22, {20, 20, 390, 50});
    check(filter_layout_candidates(1000, 800, {paragraph, line})[1].selected,
          "伸出段落边缘的文字行被删除");
    for (int cls : {7, 10, 24}) {
        const auto note = candidate(8, cls, {30, 30, 100, 50});
        check(filter_layout_candidates(1000, 800, {body, note})[1].selected,
              "小图注或脚注未受保护");
    }
    auto note = candidate(9, 7, {15, 10, 390, 280});
    check(filter_layout_candidates(1000, 800, {body, note})[1].reason == "outer_overlap",
          "相近大小的注释重复框未筛除");
    for (int cls : {14, 20}) {
        const auto special = candidate(10, cls, {10, 10, 380, 280});
        check(filter_layout_candidates(1000, 800, {table, special})[1].selected,
              "不同特殊类别的重叠被当作正文重复");
    }
}
void overlap_and_small_page_rules() {
    const auto first = candidate(10, 22, {0, 0, 100, 100});
    const auto boundary = candidate(20, 23, {30, 0, 130, 100});
    check(filter_layout_candidates(200, 200, {first, boundary})[1].selected,
          "70% 重叠边界应保留");
    auto above = boundary; above.crop.x0 = 29; above.crop.x1 = 129;
    std::copy(std::begin(boundary.box), std::end(boundary.box), above.box);
    above.box[0] = 29; above.box[2] = 129;
    check(filter_layout_candidates(200, 200, {first, above})[1].reason == "outer_overlap",
          "外层重叠阈值或面积相同的保留顺序改变");
    const auto reference = candidate(30, 18, {0, 0, 140, 140});
    auto output = filter_layout_candidates(200, 200, {reference, first});
    check(output[0].reason == "outer_reference" && output[1].selected,
          "reference 筛除应先于外层重复比较");
    const auto larger = candidate(40, 23, {29, 0, 180, 100});
    const auto third = candidate(50, 99, {0, 0, 80, 100});
    output = filter_layout_candidates(200, 200, {first, larger, third});
    check(output[0].reason == "outer_overlap" && output[1].selected && output[2].selected,
          "首块被删除后仍继续压制后续候选");
    for (const auto& size : {std::pair<int, int>{5, 100}, {100, 5}}) {
        const auto large = candidate(1, 6, {0, 0, size.first, size.second});
        const auto small = candidate(2, 22, {0, 0, 1, 1});
        output = filter_layout_candidates(size.first, size.second, {large, small});
        check(output[0].selected && output[1].selected, "小页错误执行包含过滤");
        check(filter_layout_candidates(size.first, size.second,
              {candidate(3, 18, {0, 0, 1, 1})})[0].selected, "小页错误执行 reference 过滤");
    }
    check(filter_layout_candidates(6, 6, {candidate(3, 18, {0, 0, 1, 1})})[0].reason ==
          "outer_reference", "6 像素页面未启用外层过滤");
    check(filter_layout_candidates(1000, 800, {}).empty(), "空候选集合行为改变");
}
}
int main() {
    try {
        nms_and_record_preservation(); large_image_rules(); containment_snapshot_and_direction();
        ownership_and_annotation_protection(); overlap_and_small_page_rules();
        std::cout << "版面筛选：去重、包含、合法子内容、诊断证据及小页兼容检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
