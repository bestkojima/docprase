#include "backend_factory.hpp"
namespace dococr {
bool config_supported(const std::string& config) { return config == "none"; }
std::unique_ptr<IInferenceEngine> make_backend(const std::string&) { return nullptr; }
std::string backend_capabilities(const std::string&) {
    return "{\"schema_version\":\"1.0\",\"backend\":\"none\",\"model_available\":false,\"tasks\":[]}";
}
}
