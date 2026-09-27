#pragma once
#include "dococr/inference.hpp"

namespace dococr {
struct AdaptedVisual {
    Image canvas;
    VisualTransform transform;
};

// Produces a fixed point of the pinned Qwen3-VL smartresize (factor 32,
// min 65536, max 16777216). The runtime's subsequent resize is identity.
AdaptedVisual adapt_visual(const Image& source);
}
