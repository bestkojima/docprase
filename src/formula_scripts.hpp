#pragma once
#include <string_view>

namespace dococr {
// Supported commands with mandatory brace groups. Optional arguments are
// handled separately; symbol commands have no groups.
int formula_group_arguments(std::string_view command);
// Check script attachment at atom/group boundaries after the command and
// environment validator has checked the supported TeX subset.
bool valid_formula_scripts(std::string_view formula);
}
