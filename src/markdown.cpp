#include "markdown.hpp"
#include <vector>

namespace dococr {
namespace {
bool escaped_at(const std::string& text, size_t position) {
    size_t slashes = 0;
    while (position > 0 && text[position - 1] == '\\') { --position; ++slashes; }
    return slashes % 2 != 0;
}

struct LinkEscapes {
    std::vector<bool> open, separator;
};

LinkEscapes link_openers(const std::string& source) {
    LinkEscapes escape{std::vector<bool>(source.size(), false),
                       std::vector<bool>(source.size(), false)};
    std::vector<size_t> opens;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '[' && source[i] != ']') continue;
        if (escaped_at(source, i)) continue;
        if (source[i] == '[') opens.push_back(i);
        if (source[i] != ']' || opens.empty()) continue;
        const size_t open = opens.back();
        opens.pop_back();
        size_t next = i + 1;
        while (next < source.size() && (source[next] == ' ' || source[next] == '\t')) ++next;
        const bool linked = next < source.size() &&
            (source[next] == '(' || source[next] == '[');
        const size_t line = source.rfind('\n', open);
        const size_t start = line == std::string::npos ? 0 : line + 1;
        bool definition = next < source.size() && source[next] == ':' && open - start <= 3;
        for (size_t j = start; definition && j < open; ++j)
            if (source[j] != ' ' && source[j] != '\t') definition = false;
        const bool safe_image = open > 0 && source[open - 1] == '!' && !escaped_at(source, open - 1);
        if ((linked || definition) && !safe_image) escape.open[open] = true;
        if (linked && safe_image) escape.separator[next] = true;
    }
    return escape;
}
}

std::string render_markdown_block(const MarkdownBlock& b) {
    auto safe_text = [](const std::string& source) {
        std::string result;
        result.reserve(source.size());
        const auto escape_link = link_openers(source);
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
            } else if ((ch == '[' && (escape_link.open[i] || escape_link.separator[i])) ||
                       (ch == '(' && escape_link.separator[i])) {
                result += '\\';
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
        return b.text.empty() ? marker : safe_text(b.text) + "\n\n" + marker;
    }
    if (b.type == "formula") {
        const std::string formula = safe_text(b.text);
        return b.display_formula ? "$$\n" + formula + "\n$$" : "$" + formula + "$";
    }
    if (b.type == "table" && b.structured_table) return b.text;
    return safe_text(b.text);
}
}
