#pragma once
#include "config.hpp"
#include <optional>

namespace dococr {
struct RecognitionAttempt {
    uint64_t max_new_tokens = 0, generation_timeout_ms = 0, elapsed_ms = 0;
    int64_t visual_min_pixels = 0, visual_max_pixels = 0;
    std::string correction;
    GenerationOutput output;
    std::optional<VisualTransform> input_visual;
};
struct RegionRecognition {
    std::vector<RecognitionAttempt> attempts;
    bool reset_failed = false, backend_unavailable = false;
};
RegionRecognition recognize_region(IInferenceEngine& backend, const Image& crop, Box source_box,
    const std::string& type, const std::string& request_id, const ExecutionPlan& plan,
    ExecutionContext& context);
}
