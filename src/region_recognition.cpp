#include "region_recognition.hpp"
#include "output_assessment.hpp"
#include "visual_adaptation.hpp"
#include "layout_region_policy.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dococr {
namespace {
// PaddleX crop_margin: normalize grayscale contrast, then retain pixels at
// or below 200. Uniform and tiny inputs keep their full source crop.
Box formula_content_box(const Image& image) {
    const Box full{0, 0, image.width, image.height};
    if (image.width <= 2 || image.height <= 2 ||
        image.rgb.size() != size_t(image.width) * image.height * 3) return full;
    auto gray = [&](size_t pixel) {
        const auto* p = image.rgb.data() + pixel * 3;
        return (4899 * p[0] + 9617 * p[1] + 1868 * p[2] + 8192) >> 14;
    };
    int low = 255, high = 0;
    for (size_t i = 0; i < image.rgb.size() / 3; ++i) {
        const int value = gray(i);
        low = std::min(low, value); high = std::max(high, value);
    }
    if (low == high) return full;
    Box result{image.width, image.height, 0, 0};
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        if ((gray(size_t(y) * image.width + x) - low) * 255 / (high - low) > 200) continue;
        result.x0 = std::min(result.x0, x); result.y0 = std::min(result.y0, y);
        result.x1 = std::max(result.x1, x + 1); result.y1 = std::max(result.y1, y + 1);
    }
    return result.x1 - result.x0 > 2 && result.y1 - result.y0 > 2 ? result : full;
}
}
RegionRecognition recognize_region(IInferenceEngine& backend, const Image& crop, Box source_box,
    const std::string& type, const std::string& request_id, const ExecutionPlan& plan,
    ExecutionContext& context) {
    if (!requires_ovis(type)) throw std::invalid_argument("unsupported_ovis_task");
    using Clock = std::chrono::steady_clock;
    RegionRecognition result;
    GenerationRequest request{crop, source_box, type, request_id, plan.max_new_tokens,
                              plan.generation_timeout_ms};
    if (type == "formula" && plan.formula_crop_margin) {
        const Box bounds = formula_content_box(crop);
        if (bounds.x0 || bounds.y0 || bounds.x1 != crop.width || bounds.y1 != crop.height) {
            request.image.width = bounds.x1 - bounds.x0;
            request.image.height = bounds.y1 - bounds.y0;
            request.image.rgb.resize(size_t(request.image.width) * request.image.height * 3);
            for (int y = 0; y < request.image.height; ++y)
                std::memcpy(request.image.rgb.data() + size_t(y) * request.image.width * 3,
                    crop.rgb.data() + (size_t(y + bounds.y0) * crop.width + bounds.x0) * 3,
                    size_t(request.image.width) * 3);
            request.source_box = {source_box.x0 + bounds.x0, source_box.y0 + bounds.y0,
                                  source_box.x0 + bounds.x1, source_box.y0 + bounds.y1};
            result.input_crop_bbox = request.source_box;
        }
    }
    std::string correction = "initial";
    std::optional<VisualTransform> corrected_visual;
    for (unsigned index = 0; index < 2 && !context.cancelled; ++index) {
        RecognitionAttempt attempt;
        attempt.max_new_tokens = request.max_new_tokens;
        attempt.generation_timeout_ms = request.generation_timeout_ms;
        attempt.visual_min_pixels = request.visual_min_pixels;
        attempt.visual_max_pixels = request.visual_max_pixels;
        attempt.correction = correction;
        attempt.input_visual = corrected_visual;
        const auto start = Clock::now();
        try {
            if (!backend.reset()) {
                result.reset_failed = result.backend_unavailable = true;
                attempt.output = GenerationOutput{"", "", "failed", "region_reset_failed", "reset_failed"};
            } else {
                auto response = backend.execute({request_id, request}, context);
                if (auto* output = std::get_if<GenerationOutput>(&response.payload))
                    attempt.output = std::move(*output);
                else {
                    result.backend_unavailable = true;
                    attempt.output = GenerationOutput{"", "", "failed", "generation_response_type_mismatch", "error"};
                }
            }
        } catch (const std::bad_alloc&) { throw; }
          catch (const std::exception& error) {
            result.backend_unavailable = true;
            attempt.output = GenerationOutput{"", "", "failed", error.what(), "error"};
        }
        attempt.elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        result.attempts.push_back(std::move(attempt));
        const auto& output = result.attempts.back().output;
        if (index == 1 || context.cancelled || result.backend_unavailable ||
            output.stop_reason == "timeout" || output.stop_reason == "cancelled") break;
        const auto assessment = assess_output(output, type);
        if (assessment.state == "incomplete" && output.stop_reason == "token_limit" && request.max_new_tokens < 4096) {
            request.max_new_tokens = 4096;
            correction = "increase_token_budget";
        } else if (output.stop_reason == "vision_missing" &&
                   output.visual_evidence == "no_visual_tokens") {
            // Only retry if a larger pixel budget really changes the input.
            try {
                const auto before = adapt_visual(request.image, {request.visual_min_pixels, request.visual_max_pixels});
                const auto after = adapt_visual(request.image, {262144, 1120000});
                if (after.transform.scale <= before.transform.scale ||
                    (before.canvas.width == after.canvas.width && before.canvas.height == after.canvas.height)) break;
                request.visual_min_pixels = 262144;
                request.visual_max_pixels = 1120000;
                result.attempts.back().input_visual = before.transform;
                corrected_visual = after.transform;
                correction = "increase_visual_resolution";
            } catch (const std::bad_alloc&) { throw; }
              catch (const std::exception&) { break; }
        } else break;
    }
    return result;
}
}
