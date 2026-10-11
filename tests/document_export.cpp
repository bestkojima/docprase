#include "document_export.hpp"
#include "json.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace dococr;
using Json = nlohmann::json;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
RegionRecognition recognized(const std::string& text) {
    RegionRecognition r;
    r.attempts.push_back({});
    auto& a = r.attempts.back();
    a.max_new_tokens = 128; a.generation_timeout_ms = 800;
    a.output = {text, text, "complete", "", "normal"};
    a.output.visual_evidence = "explicit_success";
    a.output.visual_transform = {256, 256, 256, 256, 0, 0, 1, 0, 0};
    return r;
}
struct Fixture {
    PagePlanInput input;
    PreRecognitionPagePlan plan;
    std::vector<LayoutCandidateRecord> candidates;
    std::vector<DocumentBlock> blocks;
    LayoutPageTransform transform;
    LabelExportPolicy policy;
    explicit Fixture(PagePlanInput value) : input(std::move(value)), plan(plan_page(input)) {
        for (const auto& c : input.candidates) {
            LayoutCandidateRecord record;
            static_cast<PageLayoutCandidate&>(record) = c;
            record.score = .95f; record.selected = true;
            record.label = layout_region_policy(c.class_id).model_label;
            record.box[0] = c.crop.x0; record.box[1] = c.crop.y0;
            record.box[2] = c.crop.x1; record.box[3] = c.crop.y1;
            candidates.push_back(record);
        }
        for (const auto& p : plan.regions) {
            DocumentBlock b;
            b.id = p.id; b.layout_id = p.layout_id; b.region_id = p.region_id;
            b.type = p.type; b.model_label = p.model_label;
            b.box = p.box; b.layout_box = p.layout_box; b.crop_expanded = p.crop_expanded;
            b.original_class_id = p.original_class_id; b.candidate_id = b.mask_row = p.candidate_id;
            b.candidate_rank = p.candidate_rank; b.detection_score = .95f;
            b.owned_layout_ids = p.owned_layout_ids;
            b.resource = "assets/" + (input.source_page ? b.id : "p0001-" + b.id) + ".png";
            const auto text = b.type == "table" ? "<table><tr><td>表格内容</td></tr></table>" :
                              b.type == "formula" ? "$x=1$" : "正文" + std::to_string(p.candidate_id);
            b.recognition = recognized(text);
            b.result = finalize_region_recognition(b.type, b.recognition);
            blocks.push_back(std::move(b));
        }
    }
    PageLayoutExport evidence() const {
        return {candidates, plan, transform, policy, "assets/overlay.png"};
    }
    PageDocumentOutput output() const {
        const auto layout = evidence();
        return export_document_page({{input.width, input.height, input.source_page, "fixed-document"},
                                     blocks, "ok", "fixed-profile", &layout});
    }
};
void frozen_order_and_metadata() {
    Fixture f({1000, 800, 0, true, {
        {10, 22, 3, {20, 20, 480, 200}}, {20, 22, 2, {20, 250, 480, 400}},
        {30, 22, 1, {520, 20, 980, 200}}, {40, 22, 0, {520, 250, 980, 400}}}});
    const auto before = f.output();
    std::reverse(f.blocks.begin(), f.blocks.end());
    const auto after = f.output();
    check(before.json == after.json && before.markdown == after.markdown, "存储顺序影响冻结阅读顺序");
    const auto doc = Json::parse(after.json); const auto& page = doc["pages"][0];
    check(page["reading_order"] == f.plan.structure.block_order &&
          doc["document_id"] == "fixed-document" && page["raster_size"] == Json({1000, 800}),
          "导出重建身份或页面计划");
    check(after.markdown == "正文10\n\n正文20\n\n正文30\n\n正文40\n", "阅读分隔符或末尾换行改变");
    Fixture pdf({1000, 800, 7, true, {{0, 22, 0, {20, 20, 980, 200}}}});
    const auto pdf_doc = Json::parse(pdf.output().json);
    check(pdf_doc["source"]["type"] == "pdf" && pdf_doc["pages"][0]["page_index"] == 6 &&
          pdf_doc["pages"][0]["page_id"] == "p0007" &&
          pdf_doc["layout_diagnostics"]["raw_tensor_assets"]["image"] == "assets/p0007-image.f32",
          "显式 PDF 页身份或资源路径错误");
    check(f.output().json == after.json, "PDF 导出污染后续图片身份");
}
void ownership_and_visibility() {
    Fixture f({1000, 800, 0, true, {
        {4, 21, 0, {10, 10, 490, 240}}, {12, 22, 1, {20, 20, 300, 80}},
        {13, 5, 2, {30, 40, 90, 60}}, {22, 8, 3, {10, 760, 990, 795}}}});
    const auto before = f.output(); const auto doc = Json::parse(before.json);
    const auto& page = doc["pages"][0];
    check(page["layout_blocks"].size() == 4 && page["blocks"].size() == 2 &&
          page["relations"].size() == 2 && page["regions"][0]["source_layout_block_ids"].size() == 3,
          "稀疏候选编号丢失归属证据");
    check(page["relations"][0]["owner_block_id"] == f.plan.ownership[0].owner_block_id &&
          page["blocks"][1]["block_order"].is_null() && before.markdown.find("正文22") == std::string::npos,
          "归属身份或标签过滤改变");
    f.policy.markdown_ignore_labels.clear();
    check(f.output().markdown.find("正文22") != std::string::npos, "标签展示策略未被采用");
}
void versions_and_history() {
    Fixture f({1000, 800, 0, true, {{0, 5, 0, {20, 20, 980, 200}}}});
    check(Json::parse(f.output().json)["schema_version"] == "1.10", "版面导出版本改变");
    f.blocks[0].recognition = recognized("$a+b$，则");
    f.blocks[0].result = finalize_region_recognition("formula", f.blocks[0].recognition);
    check(Json::parse(f.output().json)["schema_version"] == "1.11", "混合公式版本错误");
    auto layout = f.evidence(); layout.candidate_reviews = "{}";
    const auto reviewed = export_document_page({{1000, 800, 0, "fixed-document"}, f.blocks, "ok", "fixed-profile", &layout});
    check(Json::parse(reviewed.json)["schema_version"] == "1.12", "候选复核版本优先级改变");
    auto history = recognized("正文"); history.attempts[0].output.raw_output.assign(1, char(0xff));
    const auto saved = Json::parse(recognition_json(history));
    check(saved["attempts"][0]["output"]["raw_output"].is_null() &&
          saved["attempts"][0]["output"]["raw_output_base64"] == "/w==" &&
          saved["configured_generation_budget_ms"] == 800, "尝试来源编码或预算记录错误");
}
void generic_and_invalid_order() {
    DocumentBlock b;
    b.id = "b0001"; b.layout_id = "l0001"; b.region_id = "r0001";
    b.type = "text"; b.box = {0, 0, 20, 20};
    b.result.status = "ok"; b.result.text = b.result.raw = "中文\"正文\\路径";
    std::vector<DocumentBlock> blocks{b};
    const auto legacy = export_document_page({{20, 20, 0, "legacy-id"}, blocks, "ok", "legacy-profile"});
    const auto doc = Json::parse(legacy.json);
    check(doc["schema_version"] == "1.0" && !doc.contains("export_policy") &&
          doc["pages"][0]["blocks"][0]["content"]["text"] == b.result.text &&
          legacy.markdown == b.result.text + "\n", "通用导出格式或转义改变");
    Fixture f({1000, 800, 0, true, {{0, 22, 0, {20, 20, 980, 200}}}});
    bool rejected = false; f.plan.structure.block_order[0] = "missing-block";
    try { (void)f.output(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "计划缺失块被静默忽略");
}
}
int main() {
    try {
        frozen_order_and_metadata(); ownership_and_visibility(); versions_and_history(); generic_and_invalid_order();
        std::cout << "文档导出：冻结顺序、页面身份、归属、策略、版本及来源检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
