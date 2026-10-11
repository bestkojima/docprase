#include "page_plan.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace dococr;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool equal(Box a, Box b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}
bool covers(Box a, Box b) {
    return a.x0 <= b.x0 && a.y0 <= b.y0 && a.x1 >= b.x1 && a.y1 >= b.y1;
}
void invariants(const PagePlanInput& input, const PreRecognitionPagePlan& plan) {
    std::map<int, Box> candidates;
    for (const auto& c : input.candidates) candidates.emplace(c.id, c.crop);
    std::map<std::string, const PlannedRegion*> blocks;
    std::set<std::string> layouts, regions;
    std::set<int> represented;
    for (const auto& r : plan.regions) {
        check(blocks.emplace(r.id, &r).second, "内容块身份重复");
        check(layouts.insert(r.layout_id).second && regions.insert(r.region_id).second, "版面或区域身份重复");
        check(represented.insert(r.candidate_id).second, "候选输出重复");
        check(equal(r.layout_box, candidates.at(r.candidate_id)), "识别裁剪改变了版面几何");
        check(covers(r.box, r.layout_box), "识别裁剪未覆盖原版面");
    }
    for (const auto& o : plan.ownership) {
        check(represented.insert(o.candidate_id).second, "归属内容重复输出");
        check(layouts.insert(o.layout_id).second, "归属版面身份重复");
        const auto* owner = blocks.at(o.owner_block_id);
        check(owner->candidate_id == o.owner_candidate_id, "归属引用错误");
        check(covers(owner->box, candidates.at(o.candidate_id)), "归属裁剪未覆盖子内容");
        check(std::count(owner->owned_layout_ids.begin(), owner->owned_layout_ids.end(), o.layout_id) == 1,
              "归属来源未正确绑定区域");
    }
    check(represented.size() == candidates.size(), "计划漏记候选");
    check(plan.candidate_order.size() == candidates.size(), "候选顺序不完整");
    check(std::set<int>(plan.candidate_order.begin(), plan.candidate_order.end()) == represented,
          "候选顺序包含重复或无效编号");
    const auto& s = plan.structure;
    check(s.block_order.size() == blocks.size() && s.region_order.size() == regions.size(), "阅读顺序不完整");
    std::set<std::string> ordered;
    std::vector<std::string> tasks;
    for (size_t i = 0; i < s.block_order.size(); ++i) {
        const auto* r = blocks.at(s.block_order[i]);
        check(ordered.insert(r->id).second, "阅读顺序重复");
        check(s.region_order[i] == r->region_id, "区域阅读顺序不匹配");
        if (requires_ovis(r->type)) tasks.push_back(r->region_id);
    }
    check(s.recognition_order == tasks, "识别执行顺序不匹配");
    for (const auto& g : s.groups) {
        for (const auto& member : g.items) {
            check(blocks.at(member.image_block_id)->type == "image", "图片分组引用无效");
            if (!member.caption_block_id.empty()) check(blocks.count(member.caption_block_id), "图注引用无效");
        }
    }
}
const PlannedRegion& candidate(const PreRecognitionPagePlan& plan, int id) {
    const auto r = std::find_if(plan.regions.begin(), plan.regions.end(), [&](const auto& value) {
        return value.candidate_id == id;
    });
    if (r == plan.regions.end()) throw std::runtime_error("缺少计划区域");
    return *r;
}
void ownership_and_crop() {
    // 文本行越出父段落；行内公式归属被吸收的行，最终仍归根段落。
    PagePlanInput in{600, 800, 0, true, {
        {10, 22, 0, {10, 10, 310, 210}}, {20, 22, 1, {10, 190, 310, 240}},
        {30, 5, 2, {280, 218, 315, 232}}}};
    const auto plan = plan_page(in);
    invariants(in, plan);
    check(plan.regions.size() == 1 && plan.ownership.size() == 2, "文本/公式归属未归根");
    check(equal(candidate(plan, 10).box, {10, 10, 315, 240}), "裁剪未包含跨框内容");
    check(equal(in.candidates[0].crop, {10, 10, 310, 210}), "规划修改输入");
    check(plan.ownership[0].handling_reason == "overlapping_text_owned_by_text" &&
          plan.ownership[1].handling_reason == "inline_formula_owned_by_text", "归属依据错误");
    in.resolve_content_ownership = false;
    const auto layout_only = plan_page(in);
    invariants(in, layout_only);
    check(layout_only.regions.size() == 3 && layout_only.ownership.empty(), "无转写计划改变内容归属");
}
void table_precedence_and_ambiguity() {
    PagePlanInput in{600, 800, 0, true, {
        {0, 21, 0, {10, 10, 500, 400}}, {1, 22, 1, {50, 50, 350, 200}},
        {2, 5, 2, {70, 70, 150, 100}}}};
    const auto plan = plan_page(in);
    invariants(in, plan);
    check(plan.regions.size() == 1 && plan.ownership.size() == 2, "表格归属优先级改变");
    for (const auto& o : plan.ownership) check(o.owner_candidate_id == 0, "表格子内容归属错误");
    // 等面积竞争不能按输入顺序选择某个表格。
    in.candidates.push_back({3, 21, 3, {10, 10, 500, 400}});
    const auto ambiguous = plan_page(in);
    invariants(in, ambiguous);
    for (const auto& o : ambiguous.ownership)
        check(o.handling_reason != "table_content_owned_by_table", "歧义表格归属被强行选择");
    check(ambiguous.regions.size() == 3 && ambiguous.ownership.size() == 1 &&
          ambiguous.ownership.front().owner_candidate_id == 1,
          "表格歧义影响了独立的行内公式归属");
}
void ordering_and_identity() {
    PagePlanInput in{1000, 1000, 0, true, {
        {0, 22, 3, {20, 20, 480, 200}}, {1, 22, 2, {20, 250, 480, 400}},
        {2, 22, 1, {520, 20, 980, 200}}, {3, 22, 0, {520, 250, 980, 400}}}};
    const auto plan = plan_page(in);
    invariants(in, plan);
    check(plan.reading_order_evidence.reason == "model_column_conflict", "分栏冲突未记录");
    check(plan.structure.block_order == std::vector<std::string>({"b0001", "b0003", "b0002", "b0004"}),
          "阅读顺序改变");
    std::reverse(in.candidates.begin(), in.candidates.end());
    const auto reversed = plan_page(in);
    invariants(in, reversed);
    check(reversed.structure.block_order == plan.structure.block_order, "输入存储顺序影响阅读计划");
    for (const auto& r : plan.regions) check(candidate(reversed, r.candidate_id).id == r.id, "区域身份不稳定");
    in.source_page = 7;
    const auto pdf = plan_page(in);
    invariants(in, pdf);
    check(pdf.structure.block_order.front() == "p0007-b0001", "PDF 页身份错误");
    in.candidates[0].rank = -1;
    check(plan_page(in).reading_order_evidence.reason == "missing_rank", "缺失 rank 未退回几何");
    for (auto& c : in.candidates) c.rank = 0;
    check(plan_page(in).reading_order_evidence.reason == "duplicate_rank", "重复 rank 未退回几何");
}
void image_groups() {
    PagePlanInput in{1000, 800, 0, true, {
        {0, 22, 0, {20, 10, 980, 50}}, {1, 14, 1, {100, 100, 400, 300}},
        {2, 14, 2, {600, 100, 900, 300}}, {3, 7, 3, {180, 320, 310, 345}},
        {4, 7, 4, {680, 320, 810, 345}}}};
    const auto plan = plan_page(in);
    invariants(in, plan);
    check(plan.structure.groups.size() == 1 && plan.regions.size() == 5, "图片分组错误合并识别区域");
    check(plan.structure.block_order == std::vector<std::string>({"b0001", "b0002", "b0004", "b0003", "b0005"}),
          "图片与图注阅读顺序错误");
    check(plan.structure.recognition_order == std::vector<std::string>({"r0001", "r0004", "r0005"}),
          "图片资源被当成识别任务");
    check(candidate(plan, 1).model_label == "image", "规划改写模型标签");
    // 两个同样合理的图注维持歧义，不能强行归属。
    in.candidates.push_back({5, 7, 5, {180, 320, 310, 345}});
    const auto ambiguous = plan_page(in);
    invariants(in, ambiguous);
    check(ambiguous.structure.ambiguous_caption_ids.size() == 2, "图注歧义丢失");
}
void empty_unknown_and_invalid() {
    PagePlanInput in{100, 100, 0, true, {}};
    const auto empty = plan_page(in);
    invariants(in, empty);
    check(empty.reading_order_evidence.reason == "empty_page", "空页计划错误");
    in.candidates.push_back({99, 99, 0, {1, 1, 10, 10}});
    const auto unknown = plan_page(in);
    invariants(in, unknown);
    check(unknown.regions[0].type == "unknown" && unknown.structure.recognition_order.empty(),
          "未知类型启动识别");
    auto rejects = [](const PagePlanInput& input) {
        try { (void)plan_page(input); } catch (const std::invalid_argument&) { return; }
        throw std::runtime_error("非法规划输入未被拒绝");
    };
    in.candidates.push_back(in.candidates[0]); rejects(in);
    in.candidates.pop_back(); in.candidates[0].crop.x1 = 101; rejects(in);
    in.candidates.clear(); in.width = 0; rejects(in);
}
}
int main() {
    try {
        ownership_and_crop(); table_precedence_and_ambiguity(); ordering_and_identity();
        image_groups(); empty_unknown_and_invalid();
        std::cout << "页面计划：归属、裁剪、身份、三份顺序、图片歧义及非法输入检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
