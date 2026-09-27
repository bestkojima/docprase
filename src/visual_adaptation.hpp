#pragma once
#include "dococr/inference.hpp"

namespace dococr {
struct AdaptedVisual {
    Image canvas;
    int content_width = 0, content_height = 0;
    int pad_x = 0, pad_y = 0;
};

// Produces a fixed point of the pinned Qwen3-VL smartresize (factor 32,
// min 65536, max 16777216). The runtime's subsequent resize is identity.
AdaptedVisual adapt_visual(const Image& source);
}
