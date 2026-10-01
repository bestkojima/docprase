#pragma once
#include <string>

namespace dococr {
struct MarkdownBlock {
    std::string id, type, status, text, resource;
    bool display_formula = true;
    bool structured_table = false;
    std::string assessment_state;
    std::string assessment_reason;
};
std::string render_markdown_block(const MarkdownBlock& block);
}
