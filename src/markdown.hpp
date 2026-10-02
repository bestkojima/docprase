#pragma once
#include <string>

namespace dococr {
struct MarkdownBlock {
    std::string id, type, status, text, resource;
    bool display_formula = true;
    bool structured_table = false;
    std::string assessment_state;
    std::string assessment_reason;
    // Present for semantic rendering of DocumentIR 1.10; empty keeps legacy output.
    std::string semantic_label;
};
std::string render_markdown_block(const MarkdownBlock& block, bool uncertain_caption = false);
}
