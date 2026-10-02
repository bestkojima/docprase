#include "label_export_policy.hpp"
#include "config.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace dococr {
namespace {
constexpr std::array<const char*, 25> semantic_labels = {{
    "abstract", "algorithm", "aside_text", "chart", "content", "display_formula",
    "doc_title", "figure_title", "footer", "footer_image", "footnote", "formula_number",
    "header", "header_image", "image", "inline_formula", "number", "paragraph_title",
    "reference", "reference_content", "seal", "table", "text", "vertical_text", "vision_footnote"}};
// PaddleX result.py SKIP_ORDER_LABELS, pinned in docs/label-pipeline.md.
constexpr std::array<std::string_view, 11> skip_order_labels = {{
    "figure_title", "vision_footnote", "image", "chart", "table", "header", "header_image",
    "footer", "footer_image", "footnote", "aside_text"}};
}

const char* layout_semantic_label(int class_id) {
    return class_id >= 0 && class_id < int(semantic_labels.size()) ? semantic_labels[size_t(class_id)] : "unknown";
}
bool known_layout_semantic_label(const std::string& label) {
    return std::find(semantic_labels.begin(), semantic_labels.end(), label) != semantic_labels.end();
}
bool LabelExportPolicy::ignores(int class_id) const {
    return std::find(markdown_ignore_labels.begin(), markdown_ignore_labels.end(),
                     layout_semantic_label(class_id)) != markdown_ignore_labels.end();
}
bool LabelExportPolicy::markdown_visible(int class_id) const {
    return !ignores(class_id) && (show_formula_number || class_id != 11);
}
bool LabelExportPolicy::has_block_order(int class_id) const {
    const std::string_view label = layout_semantic_label(class_id);
    return !ignores(class_id) && std::find(skip_order_labels.begin(), skip_order_labels.end(), label) == skip_order_labels.end();
}
std::string label_export_policy_json(const LabelExportPolicy& policy) {
    std::string json = "{\"policy\":" + json_quote(label_export_policy_version) + ",\"markdown_ignore_labels\":[";
    for (size_t i = 0; i < policy.markdown_ignore_labels.size(); ++i) {
        if (i) json += ',';
        json += json_quote(policy.markdown_ignore_labels[i]);
    }
    return json + "],\"show_formula_number\":" + (policy.show_formula_number ? "true" : "false") + "}";
}
}
