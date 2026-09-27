#pragma once
#include <string>
#include <vector>

namespace dococr {
struct TableCell {
    int row = 0, column = 0, rowspan = 1, colspan = 1;
    bool header = false;
    std::string text;
};
struct ParsedTable {
    bool valid = false;
    std::string html;
    std::vector<TableCell> cells;
    int rows = 0, columns = 0;
};
ParsedTable parse_table(const std::string& raw);
} // namespace dococr
