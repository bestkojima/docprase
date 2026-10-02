#include "formula_scripts.hpp"

namespace dococr {
int formula_group_arguments(std::string_view command) {
    if (command == "frac" || command == "dfrac" || command == "tfrac") return 2;
    for (auto name : {"sqrt", "text", "mathrm", "mathbf", "mathbb", "operatorname",
                      "overline", "overrightarrow", "hat", "vec", "bar", "begin", "end",
                      "xrightarrow", "xleftarrow"})
        if (command == name) return 1;
    return 0;
}

namespace {
class ScriptParser {
public:
    explicit ScriptParser(std::string_view formula) : text(formula) {}
    bool parse() { return expression(0, 0); }

private:
    std::string_view text;
    size_t pos = 0;
    void whitespace() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
               text[pos] == '\r' || text[pos] == '\n')) ++pos;
    }
    bool letter(char c) const { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    bool atom(unsigned depth) {
        whitespace();
        if (pos == text.size() || text[pos] == '}' || text[pos] == '^' || text[pos] == '_') return false;
        const char first = text[pos++];
        if (first == '{') return expression('}', depth + 1);
        if (first != '\\') {
            while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80) ++pos;
            return true;
        }
        if (pos == text.size()) return false;
        const size_t start = pos;
        if (!letter(text[pos])) { ++pos; return true; }
        while (pos < text.size() && letter(text[pos])) ++pos;
        const auto command = text.substr(start, pos - start);
        whitespace();
        if ((command == "sqrt" || command == "xrightarrow" || command == "xleftarrow") &&
            pos < text.size() && text[pos] == '[') {
            ++pos;
            if (!expression(']', depth + 1)) return false;
        }
        for (int i = 0; i < formula_group_arguments(command); ++i) {
            whitespace();
            if (pos == text.size() || text[pos++] != '{' || !expression('}', depth + 1)) return false;
        }
        // Delimiter commands consume one delimiter token, including escaped
        // braces. It is not a TeX grouping brace at this position.
        if (command == "left" || command == "right" || command == "big" || command == "Big" ||
            command == "bigl" || command == "bigr" || command == "Bigl" || command == "Bigr") {
            whitespace();
            if (pos == text.size()) return false;
            if (text[pos++] == '\\') {
                if (pos == text.size()) return false;
                if (letter(text[pos])) while (pos < text.size() && letter(text[pos])) ++pos;
                else ++pos;
            }
        }
        return true;
    }
    bool expression(char close, unsigned depth) {
        if (depth > 256) return false;
        while (true) {
            whitespace();
            if (pos == text.size()) return close == 0;
            if (text[pos] == close) { ++pos; return true; }
            unsigned scripts = 0;
            bool primes = false;
            // Optional labels can contain nested brackets. Only the matching
            // closing bracket terminates the current optional argument.
            if (close == ']' && text[pos] == '[') {
                ++pos;
                if (!expression(']', depth + 1)) return false;
            }
            // TeX permits a script on an implicit empty base, e.g. ^{14}C.
            else if (text[pos] != '^' && text[pos] != '_' && text[pos] != '\'' && !atom(depth)) return false;
            while (true) {
                whitespace();
                if (pos == text.size()) break;
                if (text[pos] == '\'') {
                    if ((scripts & 1) && !primes) return false;
                    scripts |= 1;
                    primes = true;
                    ++pos;
                    continue;
                }
                if (text[pos] != '^' && text[pos] != '_') break;
                const unsigned bit = text[pos++] == '^' ? 1 : 2;
                if ((scripts & bit) && !(bit == 1 && primes)) return false;
                scripts |= bit;
                if (bit == 1) primes = false;
                if (!atom(depth)) return false;
            }
        }
    }
};
}
bool valid_formula_scripts(std::string_view formula) { return ScriptParser(formula).parse(); }
}
