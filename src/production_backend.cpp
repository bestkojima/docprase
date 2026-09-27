#include "backend_factory.hpp"
namespace dococr {
bool config_supported(const std::string& config) {
    return config == "none" || config == "mnn:pp-doclayout-v3";
}
#ifdef DOCOCR_HAS_MNN
std::unique_ptr<IInferenceEngine> make_layout_mnn_backend();
#endif
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config) {
#ifdef DOCOCR_HAS_MNN
    if (config == "mnn:pp-doclayout-v3") return make_layout_mnn_backend();
#endif
    return nullptr;
}
std::string backend_capabilities(const std::string& config) {
    if (config == "mnn:pp-doclayout-v3") {
#ifdef DOCOCR_HAS_MNN
        return "{\"schema_version\":\"1.0\",\"backend\":\"mnn:pp-doclayout-v3\",\"model_available\":true,\"tasks\":[\"layout\"]}";
#else
        return "{\"schema_version\":\"1.0\",\"backend\":\"mnn:pp-doclayout-v3\",\"model_available\":false,\"tasks\":[]}";
#endif
    }
    return "{\"schema_version\":\"1.0\",\"backend\":\"none\",\"model_available\":false,\"tasks\":[]}";
}
}
