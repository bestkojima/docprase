#include "content_validation.hpp"
#include "table_parser.hpp"
#include "formula_symbols.hpp"
#include "formula_scripts.hpp"
#include "json.hpp"
#include <algorithm>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

namespace dococr {
namespace {
std::string trim_formula(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

size_t formula_argument_end(const std::string& value, size_t pos) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' ||
                                  value[pos] == '\n')) ++pos;
    if (pos == value.size() || value[pos] != '{') return std::string::npos;
    int depth = 0;
    for (size_t i = pos; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() &&
            (value[i+1] == '{' || value[i+1] == '}')) { ++i; continue; }
        if (value[i] == '{') ++depth;
        else if (value[i] == '}' && --depth == 0)
            return trim_formula(value.substr(pos + 1, i - pos - 1)).empty() ?
                std::string::npos : i + 1;
    }
    return std::string::npos;
}

size_t formula_arrow_argument_end(const std::string& value, size_t pos) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == '\n')) ++pos;
    if (pos < value.size() && value[pos] == '[') {
        int brackets = 1, braces = 0;
        for (++pos; pos < value.size(); ++pos) {
            const char ch = value[pos];
            if (ch == '\\' && pos + 1 < value.size()) { ++pos; continue; }
            if (ch == '{') ++braces;
            else if (ch == '}' && --braces < 0) return std::string::npos;
            else if (braces == 0 && ch == '[') ++brackets;
            else if (braces == 0 && ch == ']' && --brackets == 0) { ++pos; break; }
        }
        if (brackets != 0 || braces != 0) return std::string::npos;
    }
    return formula_argument_end(value, pos);
}

struct ScalableDelimiter {
    char symbol = 0;
    size_t width = 0;
};

ScalableDelimiter scalable_delimiter(const std::string& value, size_t pos) {
    if (pos >= value.size()) return {};
    char ch = value[pos];
    if (ch == '\\' && pos + 1 < value.size() &&
        (value[pos+1] == '{' || value[pos+1] == '}' ||
         value[pos+1] == '|')) return {value[pos+1], 2};
    for (const auto& named : {
             std::pair<const char*, char>{"\\langle", '<'}, {"\\rangle", '>'},
             {"\\lvert", '|'}, {"\\rvert", '|'},
             {"\\lbrace", '{'}, {"\\rbrace", '}'}}) {
        size_t width = std::strlen(named.first);
        if (value.compare(pos, width, named.first) == 0)
            return {named.second, width};
    }
    return std::string("()[]|.<>{}").find(ch) == std::string::npos ?
        ScalableDelimiter{} : ScalableDelimiter{ch, 1};
}

ParsedFormula parse_formula(const std::string& raw, bool check_scripts = true) {
    ParsedFormula result;
    std::string value = trim_formula(raw);
    if (value.empty()) return result;
    const auto wrapped = [&](const std::string& open, const std::string& close,
                             bool display) {
        if (value.compare(0, open.size(), open) != 0) return false;
        if (value.size() < open.size() + close.size() ||
            value.compare(value.size() - close.size(), close.size(), close) != 0)
            return false;
        value = trim_formula(value.substr(open.size(), value.size() - open.size() - close.size()));
        result.display = display;
        return true;
    };
    bool has_wrapper = false;
    if (value.rfind("$$", 0) == 0) has_wrapper = wrapped("$$", "$$", true);
    else if (value.rfind("\\[", 0) == 0) has_wrapper = wrapped("\\[", "\\]", true);
    else if (value.rfind("\\(", 0) == 0) has_wrapper = wrapped("\\(", "\\)", false);
    else if (value[0] == '$') has_wrapper = wrapped("$", "$", false);
    else if (value.find("$$") != std::string::npos || value.find("\\]") != std::string::npos ||
             value.find("\\)") != std::string::npos) return result;
    if (!has_wrapper && (raw.find("$$") != std::string::npos ||
                         raw.find("\\[") != std::string::npos ||
                         raw.find("\\(") != std::string::npos || raw[0] == '$'))
        return result;
    if (value.empty() || value.find("$$") != std::string::npos ||
        value.find("\\[") != std::string::npos || value.find("\\]") != std::string::npos ||
        value.find("\\(") != std::string::npos || value.find("\\)") != std::string::npos ||
        value.find("![") != std::string::npos || value.find("\x60\x60\x60") != std::string::npos ||
        value.find("\\tag") != std::string::npos || value.find("\\label") != std::string::npos)
        return result;
    int depth = 0, text_depth = -1;
    size_t annotation_end = 0, script_end = 0;
    std::vector<char> delimiters;
    std::vector<int> scalable;
    struct FormulaEnvironment {
        std::string name;
        std::vector<char> delimiters;
        std::vector<int> scalable;
        size_t content_begin;
    };
    std::vector<FormulaEnvironment> environments;
    bool next_text_brace = false, math_evidence = has_wrapper;
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '_' || ch == '^') {
            const size_t end = formula_argument_end(value, i + 1);
            if (end != std::string::npos) script_end = std::max(script_end, end);
        }
        if (ch == '\\') {
            size_t start = ++i;
            size_t skip_delimiter = 0;
            while (i < value.size() && ((value[i] >= 'A' && value[i] <= 'Z') ||
                   (value[i] >= 'a' && value[i] <= 'z'))) ++i;
            if (i == start) {
                if (i >= value.size() || value[i] == '\n' || value[i] == '\r') return result;
            } else {
                std::string command = value.substr(start, i - start);
                // Commands with arguments or parser effects stay explicit here.
                // The versioned catalog contains only standalone math symbols
                // and operators, verified through public exports and KaTeX.
                static const std::set<std::string> symbols = [] {
                    std::set<std::string> names;
                    const auto catalog = nlohmann::json::parse(formula_symbols_json);
                    for (const auto& category : catalog.at("categories"))
                        for (const auto& name : category) names.insert(name.get<std::string>());
                    return names;
                }();
                static const std::vector<std::string> structural = {
                    "frac", "dfrac", "tfrac", "sqrt", "left", "right", "text",
                    "mathrm", "mathbf", "mathbb", "operatorname",
                    "xrightarrow", "xleftarrow",
                    "overline", "overrightarrow", "hat", "vec", "bar",
                    "begin", "end", "quad", "qquad", "big", "Big", "bigl",
                    "bigr", "Bigl", "Bigr", "langle", "rangle", "lvert",
                    "rvert", "lbrace", "rbrace"
                };
                if (!symbols.count(command) &&
                    std::find(structural.begin(), structural.end(), command) == structural.end())
                    return result;
                if (command == "xrightarrow" || command == "xleftarrow") {
                    const size_t end = formula_arrow_argument_end(value, i);
                    if (end == std::string::npos) return result;
                    // Arrow labels are annotations and may contain Chinese or
                    // words. Keep scanning their contents to validate structure.
                    annotation_end = std::max(annotation_end, end);
                } else if (command == "sqrt") {
                    size_t argument = i;
                    if (argument < value.size() && value[argument] == '[') {
                        argument = value.find(']', argument);
                        if (argument == std::string::npos) return result;
                        ++argument;
                    }
                    if (formula_argument_end(value, argument) == std::string::npos)
                        return result;
                } else {
                    size_t argument = i;
                    for (int group = 0; group < formula_group_arguments(command); ++group) {
                        argument = formula_argument_end(value, argument);
                        if (argument == std::string::npos) return result;
                    }
                }
                if (command == "left" || command == "right") {
                    ScalableDelimiter delimiter = scalable_delimiter(value, i);
                    if (!delimiter.symbol) return result;
                    skip_delimiter = delimiter.width;
                    if (command == "left") {
                        scalable.push_back(depth);
                    } else {
                        if (scalable.empty() || scalable.back() != depth) return result;
                        if (!environments.empty() && scalable.size() <= environments.back().scalable.size())
                            return result;
                        scalable.pop_back();
                    }
                }
                if (command == "begin" || command == "end") {
                    size_t end = formula_argument_end(value, i);
                    size_t environment_name_start = value.find('{', i);
                    std::string environment = value.substr(environment_name_start + 1, end - environment_name_start - 2);
                    if (environment != "aligned" && environment != "array" &&
                        environment != "matrix" && environment != "pmatrix" &&
                        environment != "bmatrix" && environment != "cases")
                        return result;
                    if (command == "begin") {
                        if (environment == "array") {
                            size_t columns_end = formula_argument_end(value, end);
                            if (columns_end == std::string::npos) return result;
                            size_t columns_start = value.find('{', end);
                            const std::string columns = value.substr(columns_start + 1, columns_end - columns_start - 2);
                            if (columns.find_first_not_of("clr| \t\n") != std::string::npos ||
                                columns.find_first_of("clr") == std::string::npos) return result;
                            end = columns_end;
                        }
                        environments.push_back({environment, delimiters, scalable, end});
                    }
                    else {
                        if (environments.empty() || environments.back().name != environment ||
                            environments.back().delimiters != delimiters ||
                            environments.back().scalable != scalable ||
                            trim_formula(value.substr(environments.back().content_begin,
                                start - 1 - environments.back().content_begin)).empty())
                            return result;
                        environments.pop_back();
                    }
                    // The checked environment name is metadata, not a math/prose
                    // token. Its braces were already validated by argument_end.
                    skip_delimiter = end - i;
                }
                next_text_brace = command == "text" || command == "mathrm" ||
                                  command == "operatorname";
                math_evidence = true;
                if (skip_delimiter) i += skip_delimiter - 1;
                else --i;
            }
            continue;
        }
        if (ch == '{') {
            delimiters.push_back('{');
            ++depth;
            if (next_text_brace) text_depth = depth;
            next_text_brace = false;
        } else if (ch == '}') {
            if (delimiters.empty() || delimiters.back() != '{') return result;
            if (!scalable.empty() && scalable.back() == depth) return result;
            if (!environments.empty() && delimiters.size() <= environments.back().delimiters.size())
                return result;
            delimiters.pop_back();
            if (--depth < 0) return result;
            if (text_depth > depth) text_depth = -1;
        }
        // Round and square brackets are printed symbols in TeX, not grouping
        // syntax. Mixed interval endpoints such as [a,b) are valid content.
        else if (ch >= 0x80 && text_depth < 0 && i >= annotation_end && i >= script_end) return result;
        else if (ch == '$' || ch == '#' || ch == '%' || ch == '\x60' ||
                 (ch == '&' && environments.empty())) return result;
        else if (ch >= 'A' && ch <= 'Z' && text_depth < 0 && i >= annotation_end) {
            size_t end = i + 1;
            while (end < value.size() && ((value[end] >= 'A' && value[end] <= 'Z') ||
                   (value[end] >= 'a' && value[end] <= 'z'))) ++end;
            const bool geometry_points = end - i <= 4 && std::all_of(
                value.begin() + i, value.begin() + end, [](char letter) {
                    return letter >= 'A' && letter <= 'Z';
                });
            // Chemical symbols are uppercase letters with an optional lowercase
            // suffix. This checks notation (FeO, NaOH), not chemical correctness.
            bool chemical_symbols = true;
            for (size_t symbol = i; symbol < end;) {
                if (value[symbol] < 'A' || value[symbol] > 'Z') {
                    chemical_symbols = false; break;
                }
                ++symbol;
                if (symbol < end && value[symbol] >= 'a' && value[symbol] <= 'z') ++symbol;
            }
            if (end - i > 2 && !geometry_points && !chemical_symbols && i >= script_end) return result;
            i = end - 1;
        } else if (ch >= 'a' && ch <= 'z' && text_depth < 0 && i >= annotation_end) {
            size_t end = i + 1;
            while (end < value.size() && ((value[end] >= 'A' && value[end] <= 'Z') ||
                   (value[end] >= 'a' && value[end] <= 'z'))) ++end;
            if (end - i > 2 && i >= script_end) return result;
            i = end - 1;
        }
        else if (ch == '^' || ch == '_' || ch == '=' || ch == '<' || ch == '>' ||
                 ch == '+' || ch == '-' ||
                 ch == '*' || ch == '/' || ch == '{' || ch == '}') math_evidence = true;
        else if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') return result;
    }
    if (depth != 0 || !delimiters.empty() || !scalable.empty() ||
        !environments.empty() ||
        value.back() == '\\' || value.back() == '^' || value.back() == '_' ||
        // A complete transcription may faithfully end with an exam answer blank,
        // such as "$z=$". Generation completeness is checked separately.
        value.back() == '+' || value.back() == '-' ||
        !math_evidence || (check_scripts && !valid_formula_scripts(value))) return result;
    result.valid = true;
    result.latex = std::move(value);
    return result;
}

bool valid_text_math(const std::string& text, size_t* math_spans = nullptr, bool check_scripts = true) {
    if (math_spans) *math_spans = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i+1] == '$' || text[i+1] == '\\')) { i += 2; continue; }
        const size_t amount_end = currency_amount_end(text, i);
        if (amount_end > i) { i = amount_end; continue; }
        std::string close;
        size_t open_length = 0;
        if (text[i] == '$') {
            open_length = i + 1 < text.size() && text[i+1] == '$' ? 2 : 1;
            close = std::string(open_length, '$');
        } else if (text[i] == '\\' && i + 1 < text.size() &&
                   (text[i+1] == '(' || text[i+1] == '[')) {
            open_length = 2;
            close = text[i+1] == '(' ? "\\)" : "\\]";
        } else if (text[i] == '\\' && i + 1 < text.size() &&
                   (text[i+1] == ')' || text[i+1] == ']')) {
            return false;
        } else { ++i; continue; }
        size_t end = i + open_length;
        bool found = false;
        for (; end + close.size() <= text.size(); ++end) {
            if (text.compare(end, close.size(), close) != 0) continue;
            size_t slashes = 0;
            for (size_t k = end; k > 0 && text[k-1] == '\\'; --k) ++slashes;
            if (slashes % 2 == 0 || close == "\\)" || close == "\\]") {
                found = true; break;
            }
        }
        if (!found || !parse_formula(text.substr(i, end + close.size() - i), check_scripts).valid)
            return false;
        if (math_spans) ++*math_spans;
        i = end + close.size();
    }
    return true;
}

ParsedFormulaRegion parse_formula_region_policy(const std::string& generated, bool check_scripts) {
    const auto single = parse_formula(generated, check_scripts);
    if (single.valid) return {true, "latex", single.latex, single.display};
    size_t spans = 0;
    if (valid_text_math(generated, &spans, check_scripts) && spans > 0)
        return {true, "markdown", generated, false};
    return {};
}

} // namespace

size_t currency_amount_end(const std::string& text, size_t i) {
    if (i >= text.size() || text[i] != '$' || i + 1 == text.size() ||
        text[i+1] < '0' || text[i+1] > '9') return 0;
    size_t slashes = 0;
    for (size_t k = i; k > 0 && text[k-1] == '\\'; --k) ++slashes;
    if (slashes % 2) return 0;
    size_t closing_math = text.find('$', i + 1);
    while (closing_math != std::string::npos) {
        size_t slashes = 0;
        for (size_t k = closing_math; k > 0 && text[k-1] == '\\'; --k)
            ++slashes;
        if (slashes % 2 == 0) break;
        closing_math = text.find('$', closing_math + 1);
    }
    bool complete_math = closing_math != std::string::npos &&
        parse_formula(text.substr(i, closing_math - i + 1)).valid;
    size_t amount_end = i + 1;
    while (amount_end < text.size() &&
           ((text[amount_end] >= '0' && text[amount_end] <= '9') ||
            text[amount_end] == ',' || text[amount_end] == '.'))
        ++amount_end;
    size_t after_space = amount_end;
    while (after_space < text.size() &&
           (text[after_space] == ' ' || text[after_space] == '\t' ||
            text[after_space] == '\r' || text[after_space] == '\n'))
        ++after_space;
    // A following legacy math opener (or an escaped dollar) starts separate
    // content. Other TeX commands still continue the numeric math candidate,
    // including non-letter commands such as \{ and \,.
    const bool separate_content = text.compare(after_space, 2, "\\(") == 0 ||
        text.compare(after_space, 2, "\\[") == 0 || text.compare(after_space, 2, "\\$") == 0;
    bool math_continues = !separate_content && after_space < text.size() &&
        std::string("$=+-*/_^<>\\").find(text[after_space]) != std::string::npos;
    if (!complete_math && !math_continues &&
        (amount_end == text.size() ||
         static_cast<unsigned char>(text[amount_end]) >= 0x80 ||
         text[amount_end] == ' ' || text[amount_end] == '\t' ||
         text[amount_end] == '\r' || text[amount_end] == '\n' ||
         text[amount_end] == ';' || text[amount_end] == ':')) {
        return amount_end;
    }
    return 0;
}
ParsedFormula parse_formula_content(const std::string& generated) {
    return parse_formula(generated);
}
ParsedFormulaRegion parse_formula_region(const std::string& generated) {
    return parse_formula_region_policy(generated, true);
}
bool matches_saved_formula_content(const std::string& generated, const std::string& content,
                                   const std::string& format, bool display) {
    // Only provenance matching uses the pre-script-check policy. Current
    // recognition, rendering and explicit revalidation always use strict rules.
    const auto parsed = parse_formula_region_policy(generated, false);
    return parsed.valid && parsed.text == content && parsed.format == format && parsed.display == display;
}
bool valid_text_math_content(const std::string& content) {
    return valid_text_math(content);
}
bool valid_table_math_content(const ParsedTable& table) {
    return table.valid && std::all_of(table.cells.begin(), table.cells.end(),
        [](const TableCell& cell) { return valid_text_math(cell.text); });
}
bool matches_formula_content(const std::string& generated, const std::string& content, bool display) {
    const auto formula = parse_formula(generated);
    return formula.valid && formula.latex == content && formula.display == display;
}

} // namespace dococr
