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
// 通用后端保持单次调用契约，不应用 Ovis 的视觉判定或重试规则。
struct GenericRegionRecognition {
    GenerationOutput output;
    std::string error;
    bool skipped_after_backend_failure = false;
    bool reset_completed = false, reset_failed = false, backend_unavailable = false;
};
GenericRegionRecognition recognize_generic_region(IInferenceEngine& backend, Image crop, Box source_box,
    const std::string& type, const std::string& request_id, uint64_t max_new_tokens,
    bool backend_unavailable, ExecutionContext& context);

}
