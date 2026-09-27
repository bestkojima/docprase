#pragma once
#include <string>
#include <vector>

namespace dococr {
struct ReexportDocument {
    std::string markdown;
    std::vector<std::string> asset_paths;
};
// Throws std::invalid_argument with a field-specific diagnostic for invalid DocumentIR.
ReexportDocument validate_and_render_document(const std::string& json);
}
