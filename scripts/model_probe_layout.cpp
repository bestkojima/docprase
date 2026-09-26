// 仅用于 Layout 契约诊断；依赖 MNN 公共 Session API。
#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using TensorMap = decltype(std::declval<MNN::Interpreter>().getSessionInputAll(nullptr));

bool shapeIs(const MNN::Tensor* tensor, const std::vector<int>& expected) {
    return tensor->shape() == expected;
}
bool typeIs(const MNN::Tensor* tensor, halide_type_code_t code) {
    auto type = tensor->getType();
    return type.code == code && type.bits == 32 && type.lanes == 1;
}
bool check(const TensorMap& tensors, const std::string& name, const std::vector<int>& shape,
           halide_type_code_t type) {
    auto found = tensors.find(name);
    return found != tensors.end() && shapeIs(found->second, shape) && typeIs(found->second, type);
}
bool writeRaw(const std::string& path, const MNN::Tensor& tensor) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(tensor.host<void>()), tensor.size());
    return file.good();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "usage: model_probe_layout_runner MODEL IMAGE_F32 OUT_PREFIX H W SCALE_MULTIPLIER\n";
        return 2;
    }
    std::unique_ptr<MNN::Interpreter> interpreter(MNN::Interpreter::createFromFile(argv[1]));
    if (!interpreter) { std::cerr << "model_load_failed\n"; return 3; }
    MNN::ScheduleConfig config;
    config.type = MNN_FORWARD_CPU;
    config.numThread = 4;
    auto session = interpreter->createSession(config);
    if (!session) { std::cerr << "session_create_failed\n"; return 4; }
    auto inputs = interpreter->getSessionInputAll(session);
    if (inputs.size() != 3 || !check(inputs, "image", {1, 3, 800, 800}, halide_type_float) ||
        !check(inputs, "im_shape", {1, 2}, halide_type_float) ||
        !check(inputs, "scale_factor", {1, 2}, halide_type_float)) {
        std::cerr << "input_contract_mismatch\n"; return 5;
    }
    auto outputs = interpreter->getSessionOutputAll(session);
    if (outputs.size() != 3 || !check(outputs, "fetch_name_0", {300, 7}, halide_type_float) ||
        !check(outputs, "fetch_name_1", {1}, halide_type_int) ||
        !check(outputs, "fetch_name_2", {300, 200, 200}, halide_type_int)) {
        std::cerr << "output_contract_mismatch\n"; return 6;
    }
    const float height = std::stof(argv[4]), width = std::stof(argv[5]), multiplier = std::stof(argv[6]);
    if (height <= 0 || width <= 0 || multiplier <= 0) { std::cerr << "invalid_geometry\n"; return 7; }
    for (const auto& entry : inputs) {
        MNN::Tensor host(entry.second, MNN::Tensor::CAFFE);
        float* data = host.host<float>();
        if (entry.first == "image") {
            std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
            if (!file || file.tellg() != static_cast<std::streamoff>(host.size())) {
                std::cerr << "input_tensor_size_mismatch\n"; return 8;
            }
            file.seekg(0);
            file.read(reinterpret_cast<char*>(data), host.size());
            if (!file) { std::cerr << "input_tensor_read_failed\n"; return 8; }
        } else if (entry.first == "im_shape") {
            data[0] = 800; data[1] = 800;
        } else {
            data[0] = 800 / (height * multiplier);
            data[1] = 800 / (width * multiplier);
        }
        if (!entry.second->copyFromHostTensor(&host)) { std::cerr << "input_copy_failed\n"; return 9; }
    }
    const auto result = interpreter->runSession(session);
    if (result != MNN::NO_ERROR) { std::cerr << "runSession=" << result << "\n"; return 10; }
    for (const auto& entry : outputs) {
        MNN::Tensor host(entry.second, MNN::Tensor::CAFFE);
        if (!entry.second->copyToHostTensor(&host)) { std::cerr << "output_copy_failed\n"; return 11; }
        if (host.shape() != entry.second->shape() || host.getType().code != entry.second->getType().code ||
            host.getType().bits != 32) {
            std::cerr << "output_contract_changed_after_run\n"; return 11;
        }
        if (!writeRaw(std::string(argv[3]) + "." + entry.first + ".bin", host)) {
            std::cerr << "output_write_failed\n"; return 12;
        }
    }
    std::cout << "MNN_VERSION=" << MNN::getVersion() << " runSession=" << result << "\n";
    return 0;
}
