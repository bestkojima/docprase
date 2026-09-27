#pragma once
#include <string>

namespace dococr {
struct MarkdownBlock {
    std::string id, type, status, text, resource;
    bool display_formula = true;
    bool structured_table = false;
    bool untrusted_output = false;
};
std::string render_markdown_block(const MarkdownBlock& block);
}
