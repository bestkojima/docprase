#include "table_parser.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

namespace dococr {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
void skip_space(const std::string& s, size_t& p) { while (p < s.size() && space(s[p])) ++p; }
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
bool append_codepoint(std::string& out, unsigned long cp) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    } else {
        out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    }
    return true;
}
bool decode_text(const std::string& source, std::string& out) {
    for (size_t i = 0; i < source.size();) {
        if (source[i] != '&') { out += source[i++]; continue; }
        size_t end = source.find(';', i + 1);
        if (end == std::string::npos || end - i > 12) return false;
        std::string entity = source.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos" || entity == "#39") out += '\'';
        else if (entity == "nbsp") out += "\xc2\xa0";
        else if (entity.size() > 1 && entity[0] == '#') {
            bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            size_t start = hex ? 2 : 1;
            if (start == entity.size()) return false;
            unsigned long value = 0;
            for (size_t j = start; j < entity.size(); ++j) {
                char c = entity[j];
                int digit = c >= '0' && c <= '9' ? c - '0' :
                    hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (digit < 0) return false;
                value = value * (hex ? 16 : 10) + unsigned(digit);
                if (value > 0x10ffff) return false;
            }
            if (!append_codepoint(out, value)) return false;
        } else return false;
        i = end + 1;
    }
    return true;
}
struct Tag {
    std::string name;
    bool closing = false, self_closing = false;
    bool seen_rowspan = false, seen_colspan = false, seen_border = false;
    int rowspan = 1, colspan = 1;
};
bool positive_span(const std::string& s, int& value) {
    if (s.empty() || s.size() > 2) return false;
    int parsed = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        parsed = parsed * 10 + (c - '0');
    }
    if (parsed < 1 || parsed > 64) return false;
    value = parsed;
    return true;
}
bool read_tag(const std::string& s, size_t& p, Tag& tag) {
    if (p >= s.size() || s[p++] != '<') return false;
    if (p < s.size() && s[p] == '/') { tag.closing = true; ++p; }
    size_t start = p;
    while (p < s.size() && std::isalpha(static_cast<unsigned char>(s[p]))) ++p;
    if (p == start) return false;
    tag.name = lower(s.substr(start, p - start));
    while (p < s.size()) {
        skip_space(s, p);
        if (p < s.size() && s[p] == '>') { ++p; return true; }
        if (p < s.size() && s[p] == '/' && p + 1 < s.size() && s[p+1] == '>') {
            p += 2; tag.self_closing = true; return true;
        }
        if (tag.closing) return false;
        start = p;
        while (p < s.size() && (std::isalpha(static_cast<unsigned char>(s[p])) || s[p] == '-')) ++p;
        if (p == start) return false;
        std::string attr = lower(s.substr(start, p - start));
        skip_space(s, p);
        if (p == s.size() || s[p++] != '=') return false;
        skip_space(s, p);
        if (p == s.size()) return false;
        char quote = s[p] == '\'' || s[p] == '"' ? s[p++] : 0;
        start = p;
        if (quote) { while (p < s.size() && s[p] != quote) ++p; }
        else { while (p < s.size() && !space(s[p]) && s[p] != '>' && s[p] != '/') ++p; }
        if (p == start || p == s.size()) return false;
        std::string value = s.substr(start, p - start);
        if (quote) ++p;
        if (attr == "rowspan" && (tag.name == "td" || tag.name == "th")) {
            if (tag.seen_rowspan || !positive_span(value, tag.rowspan)) return false;
            tag.seen_rowspan = true;
        } else if (attr == "colspan" && (tag.name == "td" || tag.name == "th")) {
            if (tag.seen_colspan || !positive_span(value, tag.colspan)) return false;
            tag.seen_colspan = true;
        } else if (attr == "border" && tag.name == "table" && (value == "0" || value == "1")) {
            if (tag.seen_border) return false;
            tag.seen_border = true;
            // Legacy Ovis output may include border=1; normalized HTML omits display attributes.
        } else return false;
    }
    return false;
}
} // namespace

ParsedTable parse_table(const std::string& raw) {
    ParsedTable failed, result;
    if (raw.size() > 1024 * 1024) return failed;
    size_t p = 0;
    skip_space(raw, p);
    Tag table;
    if (!read_tag(raw, p, table) || table.name != "table" || table.closing || table.self_closing)
        return failed;
    std::vector<std::vector<bool>> occupied;
    std::string html = "<table>";
    std::string section;
    while (true) {
        skip_space(raw, p);
        if (p >= raw.size()) return failed;
        Tag tag;
        if (!read_tag(raw, p, tag)) return failed;
        if (tag.name == "table" && tag.closing && !tag.self_closing) break;
        if ((tag.name == "thead" || tag.name == "tbody" || tag.name == "tfoot") &&
            !tag.closing && !tag.self_closing && section.empty()) {
            section = tag.name; html += '<' + section + '>'; continue;
        }
        if (!section.empty() && tag.name == section && tag.closing && !tag.self_closing) {
            html += "</" + section + '>'; section.clear(); continue;
        }
        if (tag.name != "tr" || tag.closing || tag.self_closing || result.rows >= 100)
            return failed;
        int row = result.rows++;
        if (int(occupied.size()) <= row) occupied.resize(size_t(row + 1));
        html += "<tr>";
        bool has_cell = false;
        while (true) {
            skip_space(raw, p);
            Tag cell_tag;
            if (!read_tag(raw, p, cell_tag)) return failed;
            if (cell_tag.name == "tr" && cell_tag.closing && !cell_tag.self_closing) break;
            if ((cell_tag.name != "td" && cell_tag.name != "th") || cell_tag.closing ||
                cell_tag.self_closing || result.cells.size() >= 1000) return failed;
            has_cell = true;
            int column = 0;
            while (column < 100 && column < int(occupied[size_t(row)].size()) &&
                   occupied[size_t(row)][size_t(column)]) ++column;
            if (column + cell_tag.colspan > 100 || row + cell_tag.rowspan > 100) return failed;
            for (int r = row; r < row + cell_tag.rowspan; ++r) {
                if (int(occupied.size()) <= r) occupied.resize(size_t(r + 1));
                occupied[size_t(r)].resize(std::max(occupied[size_t(r)].size(),
                                             size_t(column + cell_tag.colspan)), false);
                for (int c = column; c < column + cell_tag.colspan; ++c) {
                    if (occupied[size_t(r)][size_t(c)]) return failed;
                    occupied[size_t(r)][size_t(c)] = true;
                }
            }
            TableCell cell{row, column, cell_tag.rowspan, cell_tag.colspan,
                           cell_tag.name == "th", {}};
            std::string cell_html;
            while (true) {
                size_t next = raw.find('<', p);
                if (next == std::string::npos) return failed;
                std::string part;
                if (!decode_text(raw.substr(p, next - p), part)) return failed;
                cell.text += part;
                cell_html += html_escape(part);
                p = next;
                Tag inner;
                if (!read_tag(raw, p, inner)) return failed;
                if (inner.name == cell_tag.name && inner.closing && !inner.self_closing) break;
                if (inner.name != "br" || inner.closing) return failed;
                cell.text += '\n'; cell_html += "<br/>";
            }
            html += '<' + cell_tag.name;
            if (cell.rowspan > 1) html += " rowspan=\"" + std::to_string(cell.rowspan) + '"';
            if (cell.colspan > 1) html += " colspan=\"" + std::to_string(cell.colspan) + '"';
            html += '>' + cell_html + "</" + cell_tag.name + '>';
            result.cells.push_back(std::move(cell));
        }
        if (!has_cell) return failed;
        html += "</tr>";
    }
    if (!section.empty() || result.rows == 0) return failed;
    skip_space(raw, p);
    if (p != raw.size() || int(occupied.size()) != result.rows) return failed;
    result.columns = int(occupied[0].size());
    if (!result.columns) return failed;
    for (const auto& row : occupied)
        if (int(row.size()) != result.columns ||
            std::find(row.begin(), row.end(), false) != row.end()) return failed;
    result.html = html + "</table>";
    result.valid = true;
    return result;
}
} // namespace dococr
