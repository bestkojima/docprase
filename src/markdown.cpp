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
        // A definition may sit inside a list or quote container. Escaping any [label]:
        // prevents a shortcut reference in another block from becoming an active link.
        const bool definition = next < source.size() && source[next] == ':';
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
            b.assessment_state == "incomplete" ? "识别不完整：" :
            b.assessment_state == "anomalous" ? "确认异常：" :
            b.status == "partial" ? "待核验：" : "识别失败：") + b.id + "](" + b.resource + ")";
        if (!b.assessment_reason.empty() && b.status != "skipped") {
            const auto& reason = b.assessment_reason;
            const std::string detail = reason == "empty_numbering_structure" ?
                "输出主要由无正文的编号组成，请核对原图。" : reason == "repeated_fragment" ?
                "输出包含大量重复片段，请核对原图。" : b.assessment_state == "incomplete" ?
                "输出达到生成上限，内容不完整。" : reason == "visual_evidence_missing" ||
                reason == "ovis_visual_tokens_missing" ? "未取得有效视觉输入。" :
                reason == "ovis_runtime_timeout" ? "识别运行超时。" :
                b.status == "failed" ? "识别未能完成，请查看原图。" :
                "输出的完整性或结构尚未确认，请核对原图。";
            marker += "\n\n> " + detail;
        }
        return b.resource.empty() ? marker : "![原图](" + b.resource + ")\n\n" + marker;
    }
    if (b.type == "formula") {
        const std::string formula = safe_text(b.text);
        return b.display_formula ? "$$\n" + formula + "\n$$" : "$" + formula + "$";
    }
    if (b.type == "table" && b.structured_table) return b.text;
    return safe_text(b.text);
}
}
