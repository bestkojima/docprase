#pragma once
#include "json.hpp"
#include <string>

namespace dococr {
// Validate against the exact published DocumentIR schema selected by schema_version.
void validate_document_schema(const nlohmann::json& document, const std::string& version);
}
