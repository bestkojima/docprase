#include "document_export.hpp"
#include "markdown.hpp"
#include "layout_geometry.hpp"
#include "pdf_page_id.hpp"
#include "output_encoding.hpp"
#include "json.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dococr {
using output_encoding::valid_utf8;
using output_encoding::base64;
std::string recognition_json(const RegionRecognition& recognition) {
    using Json = nlohmann::json;
    auto transform_json = [](const VisualTransform& t) {
        return Json{{"canvas_size", {t.canvas_width, t.canvas_height}},
            {"content_size", {t.content_width, t.content_height}}, {"pad_offset", {t.pad_x, t.pad_y}},
            {"scale", t.scale}, {"rounding_error", {t.rounding_error_x, t.rounding_error_y}}};
    };
    Json attempts = Json::array();
    uint64_t budget = 0, generation_ms = 0;
    for (const auto& attempt : recognition.attempts) {
        const auto& output = attempt.output;
        auto visual = transform_json(output.visual_transform);
        visual["evidence"] = output.visual_evidence;
        visual["token_count"] = output.visual_tokens;
        Json saved = {{"finish_reason", output.finish_reason}, {"stop_reason", output.stop_reason},
            {"generation_elapsed_ms", output.generation_elapsed_ms}};
        for (const auto& value : {std::make_pair("text", output.text),
             std::make_pair("raw_output", output.raw_output), std::make_pair("error", output.error)}) {
            saved[value.first] = valid_utf8(value.second) ? Json(value.second) : Json(nullptr);
            saved[std::string(value.first) + "_base64"] = valid_utf8(value.second) ? Json(nullptr) : Json(base64(value.second));
        }
        attempts.push_back({{"index", attempts.size() + 1}, {"correction", attempt.correction},
            {"config", {{"max_new_tokens", attempt.max_new_tokens},
                        {"generation_timeout_ms", attempt.generation_timeout_ms},
                        {"visual_min_pixels", attempt.visual_min_pixels},
                        {"visual_max_pixels", attempt.visual_max_pixels}}},
            {"elapsed_ms", attempt.elapsed_ms}, {"output", saved},
            {"visual", visual},
            {"input_visual", attempt.input_visual ? transform_json(*attempt.input_visual) : Json(nullptr)}});
        budget += attempt.generation_timeout_ms;
        generation_ms += output.generation_elapsed_ms;
    }
    return Json{{"policy", "targeted-retry-v1"}, {"max_attempts", 2},
        {"max_total_generation_budget_ms", 240000}, {"configured_generation_budget_ms", budget},
        {"generation_elapsed_ms", generation_ms},
        {"selected_attempt", attempts.empty() ? Json(nullptr) : Json(attempts.size())},
        {"attempts", attempts}}.dump();
}

namespace {
using BlockView = std::vector<const DocumentBlock*>;
std::string box_json(Box b) {
    return "[" + std::to_string(b.x0) + "," + std::to_string(b.y0) + "," +
           std::to_string(b.x1) + "," + std::to_string(b.y1) + "]";
}
std::string canonical_label(int id) {
    return region_type_name(layout_region_policy(id).type);
}
struct SemanticRelation {
    std::string type, source_block_id, target_block_id, evidence;
};

const Box& layout_geometry(const DocumentBlock& block) {
    return block.crop_expanded ? block.layout_box : block.box;
}

bool spans_columns(const DocumentBlock& block, int page_width) {
    return page_plan_detail::spans_columns(layout_geometry(block), block.model_label, page_width);
}

std::string structure_json(const RegionStructure& plan) {
    using Json = nlohmann::json;
    Json value = {{"stage", "before_recognition"}, {"policy", "image_structure_v2"},
        {"block_order", plan.block_order}, {"region_order", plan.region_order},
        {"recognition_order", plan.recognition_order},
        {"groups", Json::array()}, {"captions", Json::array()},
        {"ambiguous_caption_ids", plan.ambiguous_caption_ids}};
    for (const auto& group : plan.groups) {
        Json items = Json::array();
        for (const auto& item : group.items)
            items.push_back({{"image_block_id", item.image_block_id},
                {"caption_block_id", item.caption_block_id.empty() ? Json(nullptr) : Json(item.caption_block_id)}});
        value["groups"].push_back({{"id", group.id}, {"type", "horizontal_images"},
            {"bbox", {group.bbox.x0, group.bbox.y0, group.bbox.x1, group.bbox.y1}},
            {"evidence", "fixed_anchor_and_shared_body_or_labels"}, {"items", items}});
    }
    for (const auto& caption : plan.captions)
        value["captions"].push_back({{"image_block_id", caption.image_block_id},
                                     {"caption_block_id", caption.caption_block_id}});
    return value.dump();
}

std::string footnote_marker(const std::string& text) {
    for (const std::string& marker : {"¹", "²", "³", "⁴", "⁵", "①", "②", "③", "④", "⑤",
                                      "[1]", "[2]", "[3]", "[4]", "[5]"}) {
        if (text.rfind(marker, 0) != 0) continue;
        if (marker.front() != '[') {
            for (const std::string& digit : {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"})
                if (text.compare(marker.size(), digit.size(), digit) == 0) return {};
        }
        return marker;
    }
    return {};
}

int count_footnote_references(const std::string& text, const std::string& marker) {
    const bool superscript = marker.front() != '[';
    const std::vector<std::string> superscripts = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    int count = 0;
    for (size_t pos = text.find(marker); pos != std::string::npos;
         pos = text.find(marker, pos + marker.size())) {
        bool adjacent = false;
        if (superscript) for (const auto& digit : superscripts) {
            adjacent |= pos >= digit.size() && text.compare(pos - digit.size(), digit.size(), digit) == 0;
            adjacent |= text.compare(pos + marker.size(), digit.size(), digit) == 0;
        }
        if (!adjacent) ++count;
    }
    return count;
}

bool numbered_table_caption(const std::string& text) {
    size_t offset = text.rfind("表", 0) == 0 ? std::string("表").size() :
        text.rfind("Table ", 0) == 0 ? std::string("Table ").size() : 0;
    if (!offset) return false;
    while (offset < text.size() && text[offset] == ' ') ++offset;
    if (offset == text.size()) return false;
    if (text[offset] >= '0' && text[offset] <= '9') return true;
    for (const std::string& numeral : {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"})
        if (text.compare(offset, numeral.size(), numeral) == 0) return true;
    return false;
}

std::vector<SemanticRelation> associate_annotations(const BlockView& blocks,
                                                     int page_width, int page_height) {
    std::vector<SemanticRelation> relations;
    for (const auto* caption_ptr : blocks) {
        const auto& caption = *caption_ptr;
        bool figure = caption.model_label == "figure_title";
        bool table = caption.type == "text" && caption.result.status == "ok" &&
            numbered_table_caption(caption.result.text);
        bool footnote = caption.model_label == "footnote" || caption.model_label == "vision_footnote";
        if (!figure && !table && !footnote) continue;
        const std::string marker = footnote ? footnote_marker(caption.result.text) : "";
        if (footnote && marker.empty()) continue;
        const DocumentBlock* target = nullptr;
        int best_distance = std::numeric_limits<int>::max();
        bool tie = false;
        int footnote_matches = 0;
        for (const auto* other_ptr : blocks) {
            const auto& other = *other_ptr;
            if (other.id == caption.id) continue;
            if (figure && other.type != "image") continue;
            if (table && other.type != "table") continue;
            if (footnote) {
                if (other.type != "text" || other.model_label == "footnote" ||
                    other.model_label == "vision_footnote" ||
                    count_footnote_references(other.result.text, marker) == 0)
                    continue;
            }
            int overlap = std::min(layout_geometry(caption).x1, layout_geometry(other).x1) -
                          std::max(layout_geometry(caption).x0, layout_geometry(other).x0);
            if (overlap <= 0) continue;
            int gap = layout_geometry(caption).y0 >= layout_geometry(other).y1 ? layout_geometry(caption).y0 - layout_geometry(other).y1 :
                layout_geometry(other).y0 >= layout_geometry(caption).y1 ? layout_geometry(other).y0 - layout_geometry(caption).y1 : -1;
            int max_gap = std::max(8, std::min(page_height * 3 / 100,
                std::max(layout_geometry(caption).y1 - layout_geometry(caption).y0, layout_geometry(other).y1 - layout_geometry(other).y0)));
            if (gap < 0 || (!footnote && gap > max_gap) ||
                (footnote && layout_geometry(other).y1 > layout_geometry(caption).y0)) continue;
            if (footnote) footnote_matches += count_footnote_references(other.result.text, marker);
            if (gap < best_distance) { target = &other; best_distance = gap; tie = false; }
            else if (gap == best_distance) tie = true;
        }
        if (target && !tie && (!footnote || footnote_matches == 1)) relations.push_back({footnote ? "footnote_of" : "caption_of",
            caption.id, target->id, footnote ? "model_footnote_marker_and_geometry" :
            figure ? "model_figure_title_and_geometry" : "table_prefix_and_geometry"});
    }
    for (const auto* heading_ptr : blocks) {
        const auto& heading = *heading_ptr;
        if (heading.type != "text" || heading.result.status != "ok" || heading.result.text.empty() ||
            (heading.model_label != "doc_title" && heading.model_label != "paragraph_title") ||
            numbered_table_caption(heading.result.text)) continue;
        const bool span = spans_columns(heading, page_width);
        const DocumentBlock* first[3] = {nullptr, nullptr, nullptr};
        int best_gap[3] = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                           std::numeric_limits<int>::max()};
        bool ties[3] = {false, false, false};
        for (const auto* other_ptr : blocks) {
            const auto& other = *other_ptr;
            if (other.id == heading.id || other.type == "unknown" ||
                other.model_label == "figure_title" || other.model_label == "footnote" ||
                other.model_label == "vision_footnote" || other.model_label == "footer" ||
                other.model_label == "doc_title" || other.model_label == "paragraph_title" ||
                layout_geometry(other).y0 < layout_geometry(heading).y1) continue;
            int overlap = std::min(layout_geometry(heading).x1, layout_geometry(other).x1) -
                          std::max(layout_geometry(heading).x0, layout_geometry(other).x0);
            if (overlap <= 0) continue;
            int gap = layout_geometry(other).y0 - layout_geometry(heading).y1;
            int max_gap = std::max(12, std::min(page_height * 5 / 100,
                2 * (layout_geometry(heading).y1 - layout_geometry(heading).y0)));
            if (gap > max_gap) continue;
            int column = span ? layout_geometry(other).x1 <= page_width * 0.50 ? 0 :
                layout_geometry(other).x0 >= page_width * 0.50 ? 1 : 2 : 0;
            if (gap < best_gap[column]) {
                first[column] = &other; best_gap[column] = gap; ties[column] = false;
            } else if (gap == best_gap[column]) ties[column] = true;
        }
        for (int column = 0; column < (span ? 3 : 1); ++column)
            if (first[column] && !ties[column]) relations.push_back({
                "heading_precedes", heading.id, first[column]->id, "model_heading_and_geometry"});
    }
    return relations;
}

std::string render(const DocumentBlock& b, bool uncertain_caption = false, bool semantic_text = false) {
    bool safe_table = b.result.table.valid;
    if (b.type == "table" && b.result.status == "ok" && !safe_table) {
        auto table = parse_table(b.result.text);
        safe_table = table.valid && table.html == b.result.text;
    }
    return render_markdown_block({b.id, b.type, b.result.status, b.result.text, b.resource,
                                  b.result.display_formula, safe_table, b.result.assessment.state,
                                  b.result.assessment.state == "skipped" ? "" : b.result.assessment.reason,
                                  semantic_text ? layout_semantic_label(b.original_class_id) : "",
                                  b.type == "formula" && b.result.format_override == "markdown"}, uncertain_caption);
}

std::string serialize(const PageDocumentInput& input, const BlockView& blocks,
                      const std::vector<SemanticRelation>& semantic) {
    const auto& image = input.page;
    const auto& state = input.state;
    const auto& profile = input.model_profile;
    const auto* layout = input.layout;
    const auto* raw = layout ? &layout->candidates : nullptr;
    const std::vector<ContentOwnership> empty_ownership;
    const auto& ownership = layout ? layout->plan.ownership : empty_ownership;
    const bool structured_tables = layout != nullptr;
    const auto* order_evidence = layout ? &layout->plan.reading_order_evidence : nullptr;
    const auto* structure = layout ? &layout->plan.structure : nullptr;
    const auto* layout_transform = layout ? &layout->transform : nullptr;
    const auto* label_export = layout ? &layout->label_policy : nullptr;
    const std::string overlay = layout ? layout->overlay_asset : std::string{};
    const double score_threshold = layout ? layout->score_threshold : .5;
    const std::string candidate_reviews = layout ? layout->candidate_reviews : std::string{};
    const auto page_name = pdf_page_id(image.source_page ? image.source_page : 1);
    auto page_asset = [&](const std::string& suffix) { return "assets/" + page_name + "-" + suffix; };
    std::map<int, const LayoutCandidateRecord*> candidates;
    if (raw) for (const auto& candidate : *raw) candidates.emplace(candidate.id, &candidate);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    bool has_table = structured_tables &&
        std::any_of(blocks.begin(), blocks.end(), [](const DocumentBlock* b) { return b->type == "table"; });
    const bool has_visual = std::any_of(blocks.begin(), blocks.end(), [](const DocumentBlock* b) {
        return !b->result.visual.visual_evidence.empty();
    });
    const bool mixed_formula = std::any_of(blocks.begin(), blocks.end(), [](const DocumentBlock* b) {
        return b->type == "formula" && b->result.status == "ok" && b->result.format_override == "markdown";
    });
    out << "{\"schema_version\":" << json_quote(!candidate_reviews.empty() ? "1.12" : label_export ? (mixed_formula ? "1.11" : "1.10") : structure ? "1.9" : structured_tables ? "1.7" : has_visual ? "1.5" : order_evidence ? "1.3" :
        has_table ? "1.2" : ownership.empty() ? "1.0" : "1.1")
        << ",\"document_id\":" << json_quote(image.document_id)
        << ",\"status\":" << json_quote(state);
    if (label_export) out << ",\"export_policy\":" << label_export_policy_json(*label_export);
    out << ",\"source\":{\"type\":" << json_quote(image.source_page ? "pdf" : "image")
        << "},\"pages\":[{\"page_id\":" << json_quote(page_name)
        << ",\"page_index\":" << (image.source_page ? image.source_page - 1 : 0) << ','
        << "\"raster_size\":[" << image.width << ',' << image.height << "],\"coordinate_space\":\"raster_page\","
        << "\"status\":" << json_quote(state) << ",\"reading_order\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        out << json_quote(blocks[i]->id);
    }
    out << ']';
    if (order_evidence)
        out << ",\"reading_order_evidence\":{\"source\":" << json_quote(order_evidence->source)
            << ",\"reason\":" << json_quote(order_evidence->reason) << '}';
    if (structure) out << ",\"structure_plan\":" << structure_json(*structure);
    out << ",\"layout_blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const DocumentBlock& b = *blocks[i];
        if (label_export) out << "{\"semantic_label\":" << json_quote(layout_semantic_label(b.original_class_id)) << ',';
        else out << '{';
        out << "\"id\":" << json_quote(b.layout_id) << ",\"page_id\":" << json_quote(page_name) << ",\"label\":" << json_quote(b.type)
            << ",\"bbox\":" << box_json(b.crop_expanded ? b.layout_box : b.box) << ",\"coordinate_space\":\"raster_page\","
            << "\"detection_score\":" << b.detection_score << ",\"candidate_rank\":" << b.candidate_rank
            << ",\"original_class_id\":" << b.original_class_id
            << (b.candidate_id < 0 ? "" : ",\"candidate_id\":" + std::to_string(b.candidate_id) +
                ",\"mask_row\":" + std::to_string(b.mask_row) +
                ",\"mask_nonzero\":" + std::to_string(b.mask_nonzero) +
                ",\"model_label\":" + json_quote(b.model_label) +
                ",\"original_bbox\":[" + std::to_string(b.raw_box[0]) + "," +
                std::to_string(b.raw_box[1]) + "," + std::to_string(b.raw_box[2]) + "," +
                std::to_string(b.raw_box[3]) + "]" +
                ",\"clamped\":" + (b.clamped ? "true" : "false"))
            << ",\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("layout-" + page_name) << "}}";
    }
    for (const auto& evidence : ownership) {
        const auto& c = *candidates.at(evidence.candidate_id);
        if (!blocks.empty() || &evidence != &ownership.front()) out << ',';
        if (label_export) out << "{\"semantic_label\":" << json_quote(layout_semantic_label(c.class_id)) << ',';
        else out << '{';
        out << "\"id\":" << json_quote(evidence.layout_id)
            << ",\"page_id\":" << json_quote(page_name) << ",\"label\":" << json_quote(canonical_label(c.class_id))
            << ",\"bbox\":" << box_json(c.crop)
            << ",\"coordinate_space\":\"raster_page\",\"detection_score\":" << c.score
            << ",\"candidate_rank\":" << c.rank << ",\"original_class_id\":" << c.class_id
            << ",\"candidate_id\":" << c.id << ",\"mask_row\":" << c.id
            << ",\"mask_nonzero\":" << c.mask_nonzero
            << ",\"model_label\":" << json_quote(c.label)
            << ",\"original_bbox\":[" << c.box[0] << ',' << c.box[1] << ','
            << c.box[2] << ',' << c.box[3] << "],\"clamped\":"
            << (c.clamped ? "true" : "false")
            << ",\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("layout-" + page_name) << "}}";
    }
    out << "],\"regions\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const DocumentBlock& b = *blocks[i];
        out << "{\"id\":" << json_quote(b.region_id) << ",\"page_id\":" << json_quote(page_name) << ",\"source_layout_block_ids\":["
            << json_quote(b.layout_id);
        for (const auto& owned : b.owned_layout_ids) out << ',' << json_quote(owned);
        out << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\"";
        if (structure) out << ",\"recognition_type\":" << json_quote(b.type);
        out << '}';
    }
    out << "],\"blocks\":[";
    size_t body_order = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const DocumentBlock& b = *blocks[i];
        out << '{';
        if (label_export) out << "\"block_order\":" << (label_export->has_block_order(b.original_class_id) ? std::to_string(++body_order) : "null") << ',';
        out << "\"id\":" << json_quote(b.id) << ",\"page_id\":" << json_quote(page_name) << ",\"type\":" << json_quote(b.type)
            << ",\"source_region_ids\":[" << json_quote(b.region_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\",\"geometry_granularity\":\"region\","
            << "\"reading_order_source\":" << json_quote(order_evidence ? order_evidence->source : "geometry") << ','
            << "\"status\":" << json_quote(b.result.status) << ",\"confidence\":null,\"content\":{\"format\":"
            << json_quote(!b.result.format_override.empty() ? b.result.format_override :
                     b.type == "image" || b.type == "unknown" ? "resource" :
                     b.type == "formula" ? "latex" : b.type == "table" ? "html" : "markdown")
            << ",\"text\":" << json_quote(b.result.text) << ",\"resource\":"
            << (b.resource.empty() ? "null" : json_quote(b.resource));
        if (b.type == "formula") out << ",\"display\":" << (b.result.display_formula ? "true" : "false");
        if ((has_table || order_evidence) && b.type == "table") {
            out << ",\"table\":";
            if (!b.result.table.valid) out << "null";
            else {
                out << "{\"rows\":" << b.result.table.rows << ",\"columns\":" << b.result.table.columns
                    << ",\"cells\":[";
                for (size_t j = 0; j < b.result.table.cells.size(); ++j) {
                    if (j) out << ',';
                    const auto& cell = b.result.table.cells[j];
                    out << "{\"row\":" << cell.row << ",\"column\":" << cell.column
                        << ",\"rowspan\":" << cell.rowspan << ",\"colspan\":" << cell.colspan
                        << ",\"header\":" << (cell.header ? "true" : "false")
                        << ",\"text\":" << json_quote(cell.text) << ",\"bbox\":null}";
                }
                out << "]}";
            }
        }
        out
            << "},\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("req" + b.region_id)
            << ",\"raw_output\":" << (b.result.raw_base64.empty() ? json_quote(b.result.raw) : "null")
            << ",\"raw_output_base64\":" << (b.result.raw_base64.empty() ? "null" : json_quote(b.result.raw_base64))
            << ",\"text_base64\":" << (b.result.text_base64.empty() ? "null" : json_quote(b.result.text_base64));
        if (has_visual && !b.result.visual.visual_evidence.empty()) {
            const auto& transform = b.result.visual.visual_transform;
            const double inverse_x = transform.content_width > 0 ?
                double(b.box.x1 - b.box.x0) / transform.content_width : 0;
            const double inverse_y = transform.content_height > 0 ?
                double(b.box.y1 - b.box.y0) / transform.content_height : 0;
            out << std::setprecision(17);
            out << ",\"visual\":{\"evidence\":" << json_quote(b.result.visual.visual_evidence)
                << ",\"token_count\":" << b.result.visual.visual_tokens
                << ",\"source_crop\":" << json_quote(b.resource)
                << ",\"source_bbox\":" << box_json(b.box)
                << ",\"canvas_size\":[" << transform.canvas_width << ',' << transform.canvas_height << ']'
                << ",\"content_size\":[" << transform.content_width << ',' << transform.content_height << ']'
                << ",\"pad_offset\":[" << transform.pad_x << ',' << transform.pad_y << ']'
                << ",\"scale\":" << transform.scale
                << ",\"rounding_error\":[" << transform.rounding_error_x << ',' << transform.rounding_error_y << ']'
                << ",\"canvas_to_page_affine\":[" << inverse_x << ",0,"
                << b.box.x0 - transform.pad_x * inverse_x << ",0," << inverse_y << ','
                << b.box.y0 - transform.pad_y * inverse_y << ']'
                << ",\"stop_reason\":" << json_quote(b.result.visual.stop_reason) << '}';
            out << std::setprecision(9);
        }
        if (structured_tables) {
            out << ",\"recognition\":" << recognition_json(b.recognition);
            const auto& a = b.result.assessment;
            out << ",\"assessment\":{\"policy\":" << json_quote(assessment_policy)
                << ",\"state\":" << json_quote(a.state) << ",\"reason\":" << json_quote(a.reason)
                << ",\"finish_reason\":" << json_quote(a.finish_reason)
                << ",\"stop_reason\":" << json_quote(a.stop_reason)
                << ",\"evidence_source\":" << json_quote(a.evidence_source)
                << ",\"backend_error\":" << (b.result.visual.error.empty() || !valid_utf8(b.result.visual.error) ?
                    "null" : json_quote(b.result.visual.error))
                << ",\"generation_text\":" << (valid_utf8(b.result.visual.text) ? json_quote(b.result.visual.text) : "null")
                << ",\"nonempty_lines\":" << a.nonempty_lines
                << ",\"empty_number_lines\":" << a.empty_number_lines
                << ",\"repeat_offset\":" << a.repeat_offset
                << ",\"repeat_unit_bytes\":" << a.repeat_unit_bytes
                << ",\"repeat_count\":" << a.repeat_count
                << ",\"repeat_coverage\":" << std::setprecision(17) << a.repeat_coverage << '}';
            out << std::setprecision(9);
        }
        out
            << "},\"error\":" << (b.result.error.empty() ? "null" : json_quote(b.result.error))
            << ",\"error_base64\":" << (b.result.error_base64.empty() ? "null" : json_quote(b.result.error_base64)) << "}";
    }
    out << "],\"relations\":[";
    for (size_t i = 0; i < ownership.size(); ++i) {
        if (i) out << ',';
        const auto& evidence = ownership[i];
        out << "{\"type\":\"content_owned_by\",\"source_layout_block_id\":"
            << json_quote(evidence.layout_id) << ",\"owner_block_id\":"
            << json_quote(evidence.owner_block_id) << "}";
    }
    for (const auto& relation : semantic) {
        if (!ownership.empty() || &relation != &semantic.front()) out << ',';
        out << "{\"type\":" << json_quote(relation.type)
            << ",\"source_block_id\":" << json_quote(relation.source_block_id)
            << ",\"target_block_id\":" << json_quote(relation.target_block_id)
            << ",\"evidence\":" << json_quote(relation.evidence) << '}';
    }
    out << "]}],\"resources\":[";
    bool first = true;
    for (const auto* block : blocks) if (!block->resource.empty()) {
        const auto& b = *block;
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << json_quote("asset-" + b.id) << ",\"path\":" << json_quote(b.resource)
            << ",\"media_type\":\"image/png\",\"source_block_id\":" << json_quote(b.id)
            << ",\"width\":" << b.box.x1-b.box.x0 << ",\"height\":" << b.box.y1-b.box.y0
            << ",\"bbox\":" << box_json(b.box) << ",\"coordinate_space\":\"raster_page\"}";
    }
    out << ']';
    if (raw) {
        out << ",\"layout_diagnostics\":{\"score_threshold\":" << score_threshold
            << ",\"candidate_count\":" << raw->size()
            << ",\"overlay_asset\":" << json_quote(overlay)
            << ",\"raw_tensor_assets\":{\"image\":" << json_quote(page_asset("image.f32"))
            << ",\"im_shape\":" << json_quote(page_asset("im_shape.f32"))
            << ",\"scale_factor\":" << json_quote(page_asset("scale_factor.f32"))
            << ",\"fetch_name_0\":" << json_quote(page_asset("fetch_name_0.f32"))
            << ",\"fetch_name_1\":" << json_quote(page_asset("fetch_name_1.i32"))
            << ",\"fetch_name_2\":" << json_quote(page_asset("fetch_name_2.rle"))
            << '}';
        if (layout_transform && layout_transform->applied) {
            out << std::setprecision(17)
                << ",\"input_transform\":{\"mode\":\"smartresize_800\",\"resample\":"
                << json_quote(layout_transform->resample == LayoutResample::Area ? "area" :
                              layout_transform->resample == LayoutResample::Lanczos ? "lanczos" : "bilinear")
                << ",\"source_size\":["
                << image.width << ',' << image.height << "],\"canvas_size\":[800,800],\"content_size\":["
                << layout_transform->content_width << ',' << layout_transform->content_height
                << "],\"pad_offset\":[" << layout_transform->pad_x << ',' << layout_transform->pad_y
                << "],\"canvas_to_page_affine\":[" << 1/layout_transform->scale_x << ",0,"
                << -layout_transform->pad_x/layout_transform->scale_x << ",0,"
                << 1/layout_transform->scale_y << ','
                << -layout_transform->pad_y/layout_transform->scale_y << "]}";
            out << std::setprecision(9);
        }
        out << ",\"candidates\":[";
        for (size_t i = 0; i < raw->size(); ++i) {
            const auto& c = (*raw)[i];
            if (i) out << ',';
            out << "{\"candidate_id\":" << c.id << ",\"mask_row\":" << c.id
                << ",\"class_id\":" << c.class_id << ",\"model_label\":" << json_quote(c.label)
                << ",\"score\":" << c.score << ",\"original_bbox\":["
                << c.box[0] << ',' << c.box[1] << ',' << c.box[2] << ',' << c.box[3]
                << "],\"rank\":" << c.rank << ",\"mask_nonzero\":" << c.mask_nonzero
                << ",\"selected\":" << (c.selected ? "true" : "false")
                << ",\"filter_reason\":" << (c.reason.empty() ? "null" : json_quote(c.reason))
                << ",\"handling_reason\":" << (c.handling_reason.empty() ?
                    (c.selected && c.label == "unknown" ? "\"unknown_class\"" : "null") :
                    json_quote(c.handling_reason))
                << ",\"mask_asset\":" << (c.mask_asset.empty() ? "null" : json_quote(c.mask_asset))
                << ",\"clamped\":" << (c.clamped ? "true" : "false")
                << ",\"crop_bbox\":" << (c.selected ? box_json(c.crop) : "null")
                << ",\"recognition_crop_bbox\":" << (c.crop_expanded ? box_json(c.recognition_crop) : "null")
                << ",\"crop_expansion_reason\":" << (c.crop_expanded ? "\"owned_content_union\"" : "null") << '}';
        }
        out << ']';
        if (!candidate_reviews.empty()) out << ",\"candidate_reviews\":" << candidate_reviews;
        out << '}';
    }
    out << '}';
    return out.str();
}
} // namespace

PageDocumentOutput export_document_page(const PageDocumentInput& input) {
    BlockView blocks;
    if (input.layout) {
        std::map<std::string, const DocumentBlock*> by_id;
        for (const auto& block : input.blocks)
            if (!by_id.emplace(block.id, &block).second)
                throw std::invalid_argument("document_export_duplicate_block_id");
        const auto& order = input.layout->plan.structure.block_order;
        if (order.size() != input.blocks.size())
            throw std::invalid_argument("document_export_incomplete_block_order");
        std::set<std::string> seen;
        for (const auto& id : order) {
            if (!seen.insert(id).second || !by_id.count(id))
                throw std::invalid_argument("document_export_invalid_block_order");
            blocks.push_back(by_id.at(id));
        }
    } else {
        for (const auto& block : input.blocks) blocks.push_back(&block);
    }
    std::vector<SemanticRelation> semantic;
    if (input.layout) {
        semantic = associate_annotations(blocks, input.page.width, input.page.height);
        semantic.erase(std::remove_if(semantic.begin(), semantic.end(), [&](const SemanticRelation& relation) {
            if (relation.type != "caption_of") return false;
            auto target = std::find_if(blocks.begin(), blocks.end(), [&](const DocumentBlock* block) {
                return block->id == relation.target_block_id;
            });
            return target != blocks.end() && (*target)->type == "image";
        }), semantic.end());
        for (const auto& caption : input.layout->plan.structure.captions)
            semantic.push_back({"caption_of", caption.caption_block_id, caption.image_block_id,
                                "pre_recognition_geometry"});
    }
    const std::vector<std::string> no_ambiguity;
    const auto& captions = input.layout ? input.layout->plan.structure.ambiguous_caption_ids : no_ambiguity;
    const std::set<std::string> uncertain_captions(captions.begin(), captions.end());
    PageDocumentOutput result;
    for (const auto* block : blocks) {
        if (input.layout && !input.layout->label_policy.markdown_visible(block->original_class_id)) continue;
        if (!result.markdown.empty()) result.markdown += "\n\n";
        result.markdown += render(*block, uncertain_captions.count(block->id), input.layout != nullptr);
    }
    if (!result.markdown.empty()) result.markdown += '\n';
    result.json = serialize(input, blocks, semantic);
    return result;
}
} // namespace dococr
