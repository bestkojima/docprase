#pragma once
#include <string>

namespace dococr {
struct ParsedFormula {
    bool valid = false;
    bool display = true;
    std::string latex;
};
ParsedFormula parse_formula_content(const std::string& generated);
// Markdown preserves ordered prose/math spans within a single formula Region.
struct ParsedFormulaRegion {
    bool valid = false;
    std::string format = "latex";
    std::string text;
    bool display = true;
};
ParsedFormulaRegion parse_formula_region(const std::string& generated);
// Match historical normalized content to its source without treating its old
// successful status as proof of current mathematical validity.
bool matches_saved_formula_content(const std::string& generated, const std::string& content,
                                   const std::string& format, bool display);
bool valid_text_math_content(const std::string& content);
struct ParsedTable;
bool valid_table_math_content(const ParsedTable& table);
// Exclusive end of an unescaped currency amount, or 0 for a math opener.
// Validation and Markdown export must agree before consuming a dollar sign.
size_t currency_amount_end(const std::string& content, size_t position);
// 复用首次识别的公式校验和规范化，核对保存文档中的实际展示文字。
bool matches_formula_content(const std::string& generated, const std::string& content, bool display);
}
