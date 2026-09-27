#include "backend_factory.hpp"
namespace dococr {
bool config_supported(const std::string& config) {
    return config == "none" || config == "mnn:pp-doclayout-v3" || config == "mnn:pp-doclayout-v3+ovisocr2";
}
#ifdef DOCOCR_HAS_MNN
std::unique_ptr<IInferenceEngine> make_layout_mnn_backend();
#ifdef DOCOCR_HAS_LLM
std::unique_ptr<IInferenceEngine> make_printed_page_mnn_backend();
#endif
#endif
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config) {
#ifdef DOCOCR_HAS_MNN
    if (config == "mnn:pp-doclayout-v3") return make_layout_mnn_backend();
#ifdef DOCOCR_HAS_LLM
    if (config == "mnn:pp-doclayout-v3+ovisocr2") return make_printed_page_mnn_backend();
#endif
#endif
    return nullptr;
}
std::string backend_capabilities(const std::string& config) {
    if (config == "mnn:pp-doclayout-v3+ovisocr2") {
#ifdef DOCOCR_HAS_LLM
        return "{\"schema_version\":\"1.0\",\"backend\":\"mnn:pp-doclayout-v3+ovisocr2\",\"model_available\":true,\"tasks\":[\"layout\",\"generation\"]}";
#else
        return "{\"schema_version\":\"1.0\",\"backend\":\"mnn:pp-doclayout-v3+ovisocr2\",\"model_available\":false,\"tasks\":[]}";
#endif
    }
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
