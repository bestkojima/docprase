#pragma once
#include <string>
#include <vector>

namespace dococr {
inline constexpr const char* label_export_policy_version = "paddleocr-vl-1.5-labels-v1";
// Official semantic labels are distinct from the frozen model-label aliases.
const char* layout_semantic_label(int class_id);
bool known_layout_semantic_label(const std::string& label);
struct LabelExportPolicy {
    std::vector<std::string> markdown_ignore_labels = {
        "number", "footnote", "header", "header_image", "footer", "footer_image", "aside_text"};
    bool show_formula_number = false;
    bool ignores(int class_id) const;
    bool markdown_visible(int class_id) const;
    bool has_block_order(int class_id) const;
};
std::string label_export_policy_json(const LabelExportPolicy& policy);
}
