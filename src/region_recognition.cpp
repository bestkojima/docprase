#include "region_recognition.hpp"
#include "output_assessment.hpp"
#include "visual_adaptation.hpp"
#include "layout_region_policy.hpp"
#include <chrono>
#include <stdexcept>

namespace dococr {
RegionRecognition recognize_region(IInferenceEngine& backend, const Image& crop, Box source_box,
    const std::string& type, const std::string& request_id, const ExecutionPlan& plan,
    ExecutionContext& context) {
    if (!requires_ovis(type)) throw std::invalid_argument("unsupported_ovis_task");
    using Clock = std::chrono::steady_clock;
    RegionRecognition result;
    GenerationRequest request{crop, source_box, type, request_id, plan.max_new_tokens,
                              plan.generation_timeout_ms};
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
                const auto before = adapt_visual(crop, {request.visual_min_pixels, request.visual_max_pixels});
                const auto after = adapt_visual(crop, {262144, 1120000});
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
