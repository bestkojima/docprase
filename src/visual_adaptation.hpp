#pragma once
#include "dococr/inference.hpp"

namespace dococr {
struct AdaptedVisual {
    Image canvas;
    VisualTransform transform;
};

struct VisualBudget {
    int64_t min_pixels = 65536;
    int64_t max_pixels = 560 * 560;
};

// Produces a fixed point of the pinned Qwen3-VL smartresize (factor 32,
// runtime min 65536, max 16777216). The local budget limits OCR cost while
// keeping the runtime's subsequent resize an identity operation.
AdaptedVisual adapt_visual(const Image& source, VisualBudget budget = {});
}
