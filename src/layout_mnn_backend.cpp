#include "backend_factory.hpp"
#include "config.hpp"
#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dococr {
namespace {
const std::string model_hash = "5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67";
using TensorMap = decltype(std::declval<MNN::Interpreter>().getSessionInputAll(nullptr));
bool shape_type(const TensorMap& tensors, const std::string& name,
                const std::vector<int>& shape, halide_type_code_t code) {
    auto found = tensors.find(name);
    if (found == tensors.end() || found->second->shape() != shape) return false;
    auto type = found->second->getType();
    return type.code == code && type.bits == 32 && type.lanes == 1;
}
const Tensor& required(const TensorRequest& request, const std::string& name,
                       DataType dtype, TensorLayout layout, const std::vector<int64_t>& shape) {
    for (const Tensor& tensor : request.inputs) if (tensor.name == name) {
        size_t count = dtype == DataType::Float32 || dtype == DataType::Int32 ? 4 : 1;
        for (auto dimension : shape) count *= size_t(dimension);
        if (tensor.dtype != dtype || tensor.layout != layout || tensor.shape != shape ||
            tensor.data.size() != count) throw std::runtime_error("input_contract_mismatch:" + name);
        return tensor;
    }
    throw std::runtime_error("input_contract_mismatch:" + name);
}
class LayoutMnnBackend final : public IInferenceEngine {
public:
    bool load(const BackendLoadSpec& spec) override {
        last_error_.clear();
        unload();
        if (spec.backend_id != "mnn:pp-doclayout-v3" || spec.device != "cpu" ||
            spec.artifacts.size() != 1 || spec.artifacts[0].model != "layout" ||
            spec.artifacts[0].contract_status != "contract_verified" ||
            spec.artifacts[0].sha256 != model_hash ||
            sha256_file(spec.artifacts[0].path) != model_hash) {
            last_error_ = "layout_artifact_contract_mismatch";
            return false;
        }
        interpreter_.reset(MNN::Interpreter::createFromFile(spec.artifacts[0].path.c_str()));
        if (!interpreter_) { last_error_ = "layout_model_load_failed"; return false; }
        MNN::ScheduleConfig config;
        config.type = MNN_FORWARD_CPU;
        config.numThread = 1;
        session_ = interpreter_->createSession(config);
        if (!session_) { last_error_ = "layout_session_create_failed"; unload(); return false; }
        auto inputs = interpreter_->getSessionInputAll(session_);
        auto outputs = interpreter_->getSessionOutputAll(session_);
        if (inputs.size() != 3 || outputs.size() != 3 ||
            !shape_type(inputs, "image", {1,3,800,800}, halide_type_float) ||
            !shape_type(inputs, "im_shape", {1,2}, halide_type_float) ||
            !shape_type(inputs, "scale_factor", {1,2}, halide_type_float) ||
            !shape_type(outputs, "fetch_name_0", {300,7}, halide_type_float) ||
            !shape_type(outputs, "fetch_name_1", {1}, halide_type_int) ||
            !shape_type(outputs, "fetch_name_2", {300,200,200}, halide_type_int)) {
            last_error_ = "layout_tensor_contract_mismatch"; unload(); return false;
        }
        loaded_ = spec.artifacts;
        return true;
    }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return loaded_; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override {
        return "MNN/" + std::string(MNN::getVersion()) + "/PP-DocLayoutV3=" + model_hash;
    }
    std::string last_error() const override { return last_error_; }
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext& context) override {
        if (!session_) throw std::runtime_error("layout_session_unavailable");
        auto* payload = std::get_if<TensorRequest>(&request.payload);
        if (!payload || payload->inputs.size() != 3 || payload->requested_outputs !=
            std::vector<std::string>{"fetch_name_0", "fetch_name_1", "fetch_name_2"})
            throw std::runtime_error("layout_request_contract_mismatch");
        const Tensor& image = required(*payload, "image", DataType::Float32, TensorLayout::NCHW, {1,3,800,800});
        const Tensor& im_shape = required(*payload, "im_shape", DataType::Float32, TensorLayout::Matrix, {1,2});
        const Tensor& scale = required(*payload, "scale_factor", DataType::Float32, TensorLayout::Matrix, {1,2});
        const std::pair<std::string, const Tensor*> inputs[] = {
            {"image", &image}, {"im_shape", &im_shape}, {"scale_factor", &scale}};
        for (const auto& entry : inputs) {
            MNN::Tensor* device = interpreter_->getSessionInput(session_, entry.first.c_str());
            MNN::Tensor host(device, MNN::Tensor::CAFFE);
            std::memcpy(host.host<void>(), entry.second->data.data(), entry.second->data.size());
            if (!device->copyFromHostTensor(&host)) throw std::runtime_error("layout_input_copy_failed:" + entry.first);
        }
        if (context.cancelled) throw std::runtime_error("layout_cancelled");
        auto code = interpreter_->runSession(session_);
        if (code != MNN::NO_ERROR) throw std::runtime_error("layout_inference_failed:" + std::to_string(int(code)));
        if (context.cancelled) throw std::runtime_error("layout_cancelled");
        auto runtime_outputs = interpreter_->getSessionOutputAll(session_);
        if (runtime_outputs.size() != 3 ||
            !shape_type(runtime_outputs, "fetch_name_0", {300,7}, halide_type_float) ||
            !shape_type(runtime_outputs, "fetch_name_1", {1}, halide_type_int) ||
            !shape_type(runtime_outputs, "fetch_name_2", {300,200,200}, halide_type_int))
            throw std::runtime_error("layout_output_contract_changed_after_run");
        TensorOutput result;
        const struct { const char* name; DataType dtype; TensorLayout layout; std::vector<int64_t> shape; } outputs[] = {
            {"fetch_name_0", DataType::Float32, TensorLayout::Matrix, {300,7}},
            {"fetch_name_1", DataType::Int32, TensorLayout::Matrix, {1}},
            {"fetch_name_2", DataType::Int32, TensorLayout::Matrix, {300,200,200}}};
        for (const auto& entry : outputs) {
            MNN::Tensor* device = interpreter_->getSessionOutput(session_, entry.name);
            if (!device) throw std::runtime_error("layout_output_missing:" + std::string(entry.name));
            MNN::Tensor host(device, MNN::Tensor::CAFFE);
            if (!device->copyToHostTensor(&host)) throw std::runtime_error("layout_output_copy_failed:" + std::string(entry.name));
            size_t count = 4;
            for (int64_t dimension : entry.shape) count *= size_t(dimension);
            if (host.size() != count) throw std::runtime_error("layout_output_size_mismatch:" + std::string(entry.name));
            Tensor tensor{entry.name, entry.dtype, entry.layout, entry.shape, std::vector<uint8_t>(count)};
            std::memcpy(tensor.data.data(), host.host<void>(), count);
            result.outputs.push_back(std::move(tensor));
        }
        return {std::move(result)};
    }
    bool reset() override { return session_ != nullptr; }
    void unload() override {
        loaded_.clear();
        if (interpreter_ && session_) interpreter_->releaseSession(session_);
        session_ = nullptr;
        interpreter_.reset();
    }
private:
    EngineCapabilities capabilities_{true, false, false, 1};
    std::unique_ptr<MNN::Interpreter> interpreter_;
    MNN::Session* session_ = nullptr;
    std::vector<ArtifactInfo> loaded_;
    std::string last_error_;
};
} // namespace
std::unique_ptr<IInferenceEngine> make_layout_mnn_backend() { return std::make_unique<LayoutMnnBackend>(); }
} // namespace dococr
