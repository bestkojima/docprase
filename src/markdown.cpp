#include "markdown.hpp"
#include "content_validation.hpp"
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace dococr {
namespace {
bool escaped_at(const std::string& text, size_t position) {
    size_t slashes = 0;
    while (position > 0 && text[position - 1] == '\\') { --position; ++slashes; }
    return slashes % 2 != 0;
}

struct MathSpan {
    size_t end = 0;
    std::string markdown;
};

std::string math_content(const std::string& content) {
    std::string result;
    result.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        const char ch = content[i];
        // A physical line break is TeX whitespace, but may end a Markdown
        // paragraph or prevent an inline math parser from finding its end.
        // Preserve explicit LaTeX row breaks (\\) and all other content.
        if (ch == '\r' || ch == '\n') {
            result += ' ';
            if (ch == '\r' && i + 1 < content.size() && content[i + 1] == '\n') ++i;
        } else result += ch;
    }
    return result;
}

MathSpan math_span(const std::string& source, size_t start) {
    const bool brackets = source[start] == '\\' && start + 1 < source.size() &&
        (source[start + 1] == '(' || source[start + 1] == '[');
    if (source[start] != '$' && !brackets) return {};
    if (escaped_at(source, start)) return {};
    size_t width = 0;
    std::string close;
    bool display = false;
    if (source[start] == '$') {
        width = start + 1 < source.size() && source[start + 1] == '$' ? 2 : 1;
        close.assign(width, '$');
        display = width == 2;
    } else {
        width = 2;
        display = source[start + 1] == '[';
        close = display ? "\\]" : "\\)";
    }
    size_t end = source.find(close, start + width);
    while (end != std::string::npos && escaped_at(source, end))
        end = source.find(close, end + width);
    if (end == std::string::npos) return {};
    const std::string raw = source.substr(start, end + width - start);
    const size_t first = source.find_first_not_of(" \t\r\n", start + width);
    const size_t last = source.find_last_not_of(" \t\r\n", end - 1);
    if (first >= end || last < first ||
        !matches_formula_content(raw, source.substr(first, last - first + 1), display)) return {};
    const std::string delimiter = display ? "$$" : "$";
    std::string markdown = delimiter + math_content(source.substr(first, last - first + 1)) + delimiter;
    const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    // Dollar-aware Markdown parsers distinguish math from currency using the
    // adjacent characters. Keep valid math recognizable after normalization.
    if (start && (digit(source[start - 1]) || source[start - 1] == '\\'))
        markdown.insert(0, 1, ' ');
    if (end + width < source.size() && digit(source[end + width])) markdown += ' ';
    return {end + width, std::move(markdown)};
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

bool horizontal_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

size_t heading_content(const std::string& line) {
    size_t start = 0;
    while (start < line.size() && line[start] == ' ' && start < 3) ++start;
    size_t end = start;
    while (end < line.size() && line[end] == '#') ++end;
    if (end == start || end - start > 6 || (end < line.size() && !horizontal_space(line[end]))) return 0;
    while (end < line.size() && horizontal_space(line[end])) ++end;
    return end;
}

std::string trim_horizontal(std::string text) {
    const auto start = text.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
}

// Protect math, code and quoted examples before looking for exam labels or
// caption headings. These constructs may contain the exact same characters.
std::vector<bool> protected_text(const std::string& source) {
    std::vector<bool> protected_bytes(source.size(), false);
    const std::array<std::pair<std::string, std::string>, 5> quotes = {{{"\"", "\""},
        {"“", "”"}, {"‘", "’"}, {"「", "」"}, {"『", "』"}}};
    char fence = 0;
    size_t fence_width = 0;
    for (size_t i = 0; i < source.size();) {
        const bool line_start = i == 0 || source[i - 1] == '\n';
        if (line_start) {
            const size_t line_end = source.find('\n', i);
            const size_t end = line_end == std::string::npos ? source.size() : line_end + 1;
            size_t p = i;
            while (p < end && source[p] == ' ' && p - i < 3) ++p;
            const char ch = p < end ? source[p] : 0;
            size_t run = p;
            if (ch == '`' || ch == '~') while (run < end && source[run] == ch) ++run;
            if (fence || run - p >= 3) {
                std::fill(protected_bytes.begin() + i, protected_bytes.begin() + end, true);
                if (fence && ch == fence && run - p >= fence_width &&
                    trim_horizontal(source.substr(run, (line_end == std::string::npos ? end : line_end) - run)).empty()) fence = 0;
                else if (!fence) { fence = ch; fence_width = run - p; }
                i = end;
                continue;
            }
        }
        if (source[i] == '`' && !escaped_at(source, i)) {
            size_t run = i;
            while (run < source.size() && source[run] == '`') ++run;
            const std::string ticks(run - i, '`');
            size_t end = source.find(ticks, run);
            while (end != std::string::npos &&
                   ((end > 0 && source[end - 1] == '`') || (end + ticks.size() < source.size() && source[end + ticks.size()] == '`')))
                end = source.find(ticks, end + ticks.size());
            if (end != std::string::npos) {
                end += ticks.size();
                std::fill(protected_bytes.begin() + i, protected_bytes.begin() + end, true);
                i = end;
                continue;
            }
            i = run;
            continue;
        }
        const auto math = math_span(source, i);
        if (math.end > i) {
            std::fill(protected_bytes.begin() + i, protected_bytes.begin() + math.end, true);
            i = math.end;
            continue;
        }
        bool quoted = false;
        for (const auto& quote : quotes) {
            if (source.compare(i, quote.first.size(), quote.first) != 0 || escaped_at(source, i)) continue;
            size_t end = source.find(quote.second, i + quote.first.size());
            while (end != std::string::npos && escaped_at(source, end))
                end = source.find(quote.second, end + quote.second.size());
            if (end == std::string::npos) continue;
            end += quote.second.size();
            std::fill(protected_bytes.begin() + i, protected_bytes.begin() + end, true);
            i = end;
            quoted = true;
            break;
        }
        if (!quoted) ++i;
    }
    return protected_bytes;
}

struct ExamMarker { size_t start, end; bool answer; };
std::vector<ExamMarker> exam_markers(const std::string& source, size_t start, size_t end,
                                     const std::vector<bool>& protected_bytes) {
    const std::array<std::pair<std::string, bool>, 4> labels = {{{"[答案]", true}, {"[解析]", false},
                                                             {"【答案】", true}, {"【解析】", false}}};
    std::vector<ExamMarker> markers;
    for (size_t i = start; i < end; ++i) {
        if (protected_bytes[i] || escaped_at(source, i)) continue;
        for (const auto& label : labels) {
            if (source.compare(i, label.first.size(), label.first) != 0) continue;
            size_t p = i + label.first.size();
            while (p < end && horizontal_space(source[p])) ++p;
            if (p < end && source[p] == ':') ++p;
            else if (p + 3 <= end && source.compare(p, 3, "：") == 0) p += 3;
            else continue;
            if (p > end || std::any_of(protected_bytes.begin() + i, protected_bytes.begin() + p,
                                      [](bool value) { return value; })) continue;
            markers.push_back({i, p, label.second});
            i = p - 1;
            break;
        }
    }
    return markers;
}

bool choice_answer(std::string answer) {
    answer = trim_horizontal(std::move(answer));
    bool choice = false;
    for (size_t i = 0; i < answer.size();) {
        if (answer[i] >= 'A' && answer[i] <= 'H') { choice = true; ++i; }
        else if (horizontal_space(answer[i]) || answer[i] == ',') ++i;
        else if (answer.compare(i, 3, "、") == 0 || answer.compare(i, 3, "，") == 0) i += 3;
        else return false;
    }
    return choice;
}

std::string semantic_text(const std::string& source, const std::string& label) {
    if (label.empty()) return source;
    const bool caption = label == "figure_title" || label == "vision_footnote";
    const auto protected_bytes = protected_text(source);
    std::string result;
    for (size_t start = 0; start < source.size();) {
        const size_t newline = source.find('\n', start);
        const size_t end = newline == std::string::npos ? source.size() : newline;
        std::string line = source.substr(start, end - start);
        const size_t heading = heading_content(line);
        const auto markers = caption ? std::vector<ExamMarker>{} : exam_markers(source, start, end, protected_bytes);
        const bool standalone = !markers.empty() && markers.front().start >= start + heading &&
            trim_horizontal(line.substr(heading, markers.front().start - start - heading)).empty();
        const bool inline_pair = markers.size() >= 2 && markers[0].answer && !markers[1].answer &&
            choice_answer(source.substr(markers[0].end, markers[1].start - markers[0].end));
        if (standalone || inline_pair) {
            if (standalone && !result.empty() && result.back() == '\n' &&
                (result.size() < 2 || result[result.size() - 2] != '\n')) result += '\n';
            if (!standalone) line = trim_horizontal(source.substr(start, markers[0].start - start));
            else line.clear();
            for (size_t i = 0; i < markers.size(); ++i) {
                if (!line.empty()) line += "\n\n";
                line += markers[i].answer ? "**答案：**" : "**解析：**";
                const size_t stop = i + 1 == markers.size() ? end : markers[i + 1].start;
                const auto value = trim_horizontal(source.substr(markers[i].end, stop - markers[i].end));
                if (!value.empty()) line += ' ' + value;
            }
        } else if (caption && heading && !protected_bytes[start]) {
            line = line.substr(heading);
        }
        result += line;
        if (newline == std::string::npos) break;
        result += '\n';
        start = newline + 1;
    }
    return result;
}
}

std::string render_markdown_block(const MarkdownBlock& b, bool uncertain_caption) {
    auto with_relation_note = [&](std::string rendered) {
        if (uncertain_caption) {
            rendered += "\n\n> 图注与图片的对应关系尚未确认，请核对原图。";
            if (b.status == "ok" && !b.resource.empty()) rendered += "\n\n![原图](" + b.resource + ")";
        }
        return rendered;
    };
    auto safe_text = [](const std::string& source) {
        std::string result;
        result.reserve(source.size());
        const auto escape_link = link_openers(source);
        size_t consecutive_backslashes = 0;
        for (size_t i = 0; i < source.size(); ++i) {
            const auto math = math_span(source, i);
            if (math.end > i) {
                result += math.markdown;
                i = math.end - 1;
                consecutive_backslashes = 0;
                continue;
            }
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
        return with_relation_note(b.resource.empty() ? marker : "![原图](" + b.resource + ")\n\n" + marker);
    }
    if (b.type == "formula") {
        if (b.mixed_formula) return safe_text(b.text);
        const std::string delimiter = b.display_formula ? "$$" : "$";
        const std::string formula = matches_formula_content(delimiter + b.text + delimiter,
            b.text, b.display_formula) ? math_content(b.text) : safe_text(b.text);
        return b.display_formula ? "$$\n" + formula + "\n$$" : "$" + formula + "$";
    }
    if (b.type == "table" && b.structured_table) return b.text;
    return with_relation_note(safe_text(b.type == "text" ? semantic_text(b.text, b.semantic_label) : b.text));
}
}
