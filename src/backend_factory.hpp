#ifndef DOCOCR_BACKEND_FACTORY_HPP
#define DOCOCR_BACKEND_FACTORY_HPP
#include "dococr/inference.hpp"
#include <memory>
#include <string>
namespace dococr {
bool config_supported(const std::string& config);
std::unique_ptr<IInferenceEngine> make_backend(const std::string& config);
std::string backend_capabilities(const std::string& config);
}
#endif
