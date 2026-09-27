#include "markdown.hpp"

namespace dococr {
std::string render_markdown_block(const MarkdownBlock& b) {
    auto safe_text = [](const std::string& source) {
        std::string result;
        result.reserve(source.size());
        size_t consecutive_backslashes = 0;
        for (size_t i = 0; i < source.size(); ++i) {
            char ch = source[i];
            if (ch == '\\') { result += ch; ++consecutive_backslashes; continue; }
            if (ch == '&') result += "&amp;";
            else if (ch == '<') result += "&lt;";
            else if (ch == '>') result += "&gt;";
            else if (ch == '!' && i + 1 < source.size() && source[i+1] == '[') {
                if (consecutive_backslashes % 2 == 0) result += '\\';
                result += ch;
            } else result += ch;
            consecutive_backslashes = 0;
        }
        return result;
    };
    if (b.type == "image" && !b.resource.empty()) return "![插图](" + b.resource + ")";
    if (b.status != "ok") {
        std::string marker = "[" + std::string(b.status == "skipped" ? "未处理：" :
            b.status == "partial" ? "待核验：" : "识别失败：") + b.id + "](" + b.resource + ")";
        return b.text.empty() ? marker :
            (b.untrusted_output ? safe_text(b.text) : b.text) + "\n\n" + marker;
    }
    if (b.type == "formula") {
        const std::string formula = b.untrusted_output ? safe_text(b.text) : b.text;
        return b.display_formula ? "$$\n" + formula + "\n$$" : "$" + formula + "$";
    }
    if (b.type == "table" && b.structured_table) return b.text;
    return b.untrusted_output ? safe_text(b.text) : b.text;
}
}
