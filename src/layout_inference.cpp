#include "layout_inference.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace dococr {
namespace {
Tensor geometry_tensor(const std::string& name, float first, float second) {
    Tensor tensor{name, DataType::Float32, TensorLayout::Matrix, {1,2},
                  std::vector<uint8_t>(2*sizeof(float))};
    float values[] = {first, second};
    std::memcpy(tensor.data.data(), values, sizeof(values));
    return tensor;
}

} // namespace

std::optional<DocLayoutInference> infer_doclayout(const PagePipelineContext& context,
    bool transcribe, RunResult& audit) {
    using Clock = std::chrono::steady_clock;
    auto* backend = &context.backend;
    const auto& image = context.image;
    const auto& identity = context.identity;
    const auto* plan = context.plan;
    auto& cancelled = context.cancelled;
    const auto& progress = context.progress;
    ExecutionContext inference_context{cancelled};
    if (!backend->capabilities().tensor ||
        (transcribe && (!backend->capabilities().generation || !backend->capabilities().isolated_sessions)) ||
        backend->capabilities().max_concurrent_requests != 1)
        { audit = {RunCode::Unsupported, {}}; return std::nullopt; }
    if (cancelled) { audit.code = RunCode::Cancelled; return std::nullopt; }
    auto start = Clock::now();
    TensorRequest request;
    const std::string preprocessing = plan ? plan->layout_preprocess : "auto";
    const double score_threshold = plan ? plan->layout_score_threshold : .5;
    const bool resize_page = preprocessing == "smartresize_area" || preprocessing == "smartresize_bilinear" ||
        preprocessing == "smartresize_lanczos" ||
        (preprocessing == "auto" && uint64_t(image.width)*image.height > max_standard_page_pixels);
    const LayoutResample resample = preprocessing == "smartresize_area" ? LayoutResample::Area :
        preprocessing == "smartresize_lanczos" ? LayoutResample::Lanczos : LayoutResample::Bilinear;
    LayoutPageInput layout_input = prepare_layout_page(image, resize_page, resample);
    request.inputs.push_back(std::move(layout_input.tensor));
    request.inputs.push_back(geometry_tensor("im_shape", 800, 800));
    request.inputs.push_back(layout_input.transform.applied ?
        geometry_tensor("scale_factor", 1, 1) :
        geometry_tensor("scale_factor", 800.0f/image.height, 800.0f/image.width));
    request.requested_outputs = {"fetch_name_0", "fetch_name_1", "fetch_name_2"};
    audit.layout_attempted = true;
    audit.output.assets.push_back({identity.page_asset("image.f32"), request.inputs[0].data});
    audit.output.assets.push_back({identity.page_asset("im_shape.f32"), request.inputs[1].data});
    audit.output.assets.push_back({identity.page_asset("scale_factor.f32"), request.inputs[2].data});
    audit.did_normalize = true;
    InferenceResponse response;
    if (progress) progress("layout_started", identity.progress_page(), "", 0, 0);
    try { response = backend->execute({"layout-" + identity.page_id(), std::move(request)}, inference_context); }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception& e) {
        audit.code = cancelled ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = cancelled ? "layout_interrupted_error" : "layout_inference_failed";
        audit.error_message = "版面推理异常：" + std::string(e.what());
        audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
        return std::nullopt;
    }
    audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
    if (cancelled) { audit.code = RunCode::Cancelled; return std::nullopt; }
    auto* output = std::get_if<TensorOutput>(&response.payload);
    auto decoded = output ? decode_layout_output(std::move(*output),
        {image.width, image.height, layout_input.transform, score_threshold}) : std::nullopt;
    if (!decoded) {
        audit.code = RunCode::Failed;
        audit.error_code = "layout_output_contract_mismatch";
        audit.error_message = "PP-DocLayoutV3 候选/数量/mask 张量契约不符";
        return std::nullopt;
    }
    return DocLayoutInference{layout_input.transform, std::move(layout_input.preview), std::move(*decoded)};
}

std::optional<std::vector<GenericLayoutCandidate>> infer_generic_layout(
    const PagePipelineContext& context, RunResult& audit) {
    using Clock = std::chrono::steady_clock;
    auto* backend = &context.backend;
    const auto& image = context.image;
    const auto& identity = context.identity;
    const auto* plan = context.plan;
    auto& cancelled = context.cancelled;
    const auto& progress = context.progress;
    ExecutionContext inference_context{cancelled};
    if (!backend->capabilities().tensor || !backend->capabilities().generation ||
        backend->capabilities().max_concurrent_requests < 1) { audit = {RunCode::Unsupported, {}}; return std::nullopt; }
    if (cancelled) { audit.code = RunCode::Cancelled; return std::nullopt; }
    Tensor page_tensor{"page_rgb", DataType::UInt8, TensorLayout::HWC,
                       {image.height, image.width, 3}, image.rgb};
    if (plan && std::any_of(plan->processing.begin(), plan->processing.end(),
                            [](const ProcessingStep& step) { return step.id == "normalize" && step.owner == "adapter"; })) {
        page_tensor.name = "page_rgb_normalized";
        page_tensor.dtype = DataType::Float32;
        page_tensor.data.resize(image.rgb.size() * sizeof(float));
        for (size_t i = 0; i < image.rgb.size(); ++i) {
            float value = float(image.rgb[i]) / 255.0f;
            std::memcpy(page_tensor.data.data() + i*sizeof(float), &value, sizeof(float));
        }
        audit.did_normalize = true;
    }
    const auto start = Clock::now();
    if (progress) progress("layout_started", identity.progress_page(), "", 0, 0);
    auto response = backend->execute({"layout-" + identity.page_id(), TensorRequest{{std::move(page_tensor)}, {"layout_candidates"}}}, inference_context);
    audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    if (cancelled) { audit.code = RunCode::Cancelled; return std::nullopt; }
    auto* layout = std::get_if<TensorOutput>(&response.payload);
    if (!layout) { audit = {RunCode::Failed, {}}; return std::nullopt; }
    audit.did_layout = true;
    if (progress) progress("layout_completed", identity.progress_page(), "", 0, 0);
    auto decoded_layout = decode_generic_layout_output(*layout, image.width, image.height);
    if (!decoded_layout) { audit = {RunCode::Failed, {}}; return std::nullopt; }
    return decoded_layout;
}
} // namespace dococr
