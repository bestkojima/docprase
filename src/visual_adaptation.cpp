#include "visual_adaptation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace dococr {
AdaptedVisual adapt_visual(const Image& source, VisualBudget budget) {
    constexpr int factor = 32;
    constexpr int64_t runtime_minimum = 65536, runtime_maximum = 16777216;
    if (budget.min_pixels < runtime_minimum || budget.max_pixels > runtime_maximum ||
        budget.max_pixels < budget.min_pixels)
        throw std::runtime_error("visual_budget_invalid");
    const int64_t minimum = budget.min_pixels, maximum = budget.max_pixels;
    if (source.width <= 0 || source.height <= 0 ||
        int64_t(source.width) * source.height > runtime_maximum * 16 ||
        source.rgb.size() != size_t(source.width) * source.height * 3)
        throw std::runtime_error("visual_image_invalid");
    if (double(std::max(source.width, source.height)) / std::min(source.width, source.height) > 200)
        throw std::runtime_error("visual_aspect_ratio_unsupported");
    const int seed_w = std::max(factor, source.width);
    const int seed_h = std::max(factor, source.height);
    auto round_even = [](double value) {
        const double floor_value = std::floor(value);
        if (value - floor_value < .5) return int(floor_value);
        if (value - floor_value > .5) return int(floor_value + 1);
        return int(floor_value) % 2 == 0 ? int(floor_value) : int(floor_value + 1);
    };
    int height = round_even(double(seed_h) / factor) * factor;
    int width = round_even(double(seed_w) / factor) * factor;
    int64_t pixels = int64_t(height) * width;
    if (pixels > maximum) {
        // Rounding both axes down can collapse a narrow side to 32 and waste
        // the remaining area. Search the small set of 32-aligned heights for
        // the canvas that preserves the largest isotropic content scale.
        double best_scale = -1;
        int best_width = 0, best_height = 0;
        for (int candidate_h = factor; candidate_h <= maximum / factor; candidate_h += factor) {
            int candidate_w = int(maximum / candidate_h / factor) * factor;
            candidate_w = std::min(candidate_w, candidate_h * 200);
            if (candidate_w < factor || double(candidate_h) / candidate_w > 200) continue;
            const double candidate_scale = std::min(double(candidate_w) / source.width,
                                                    double(candidate_h) / source.height);
            if (candidate_scale > best_scale) {
                best_scale = candidate_scale;
                best_width = candidate_w;
                best_height = candidate_h;
            }
        }
        if (best_scale < 0) throw std::runtime_error("visual_canvas_unsupported");
        width = int(std::ceil(std::round(source.width * best_scale) / factor)) * factor;
        height = int(std::ceil(std::round(source.height * best_scale) / factor)) * factor;
        if (int64_t(width) * height < minimum) {
            width = best_width;
            height = best_height;
        }
    } else if (pixels < minimum) {
        const double beta = std::sqrt(double(minimum) / (double(seed_h) * seed_w));
        height = int(std::ceil(seed_h * beta / factor)) * factor;
        width = int(std::ceil(seed_w * beta / factor)) * factor;
    }
    pixels = int64_t(height) * width;
    if (height < factor || width < factor || pixels < minimum || pixels > maximum ||
        double(std::max(width, height)) / std::min(width, height) > 200)
        throw std::runtime_error("visual_canvas_unsupported");

    // The same scale is applied to both axes. Integer dimensions are rounded
    // once; pad offsets make the inverse mapping unambiguous.
    const double scale = std::min(double(width) / source.width, double(height) / source.height);
    AdaptedVisual result;
    auto& transform = result.transform;
    transform.canvas_width = width;
    transform.canvas_height = height;
    transform.scale = scale;
    transform.content_width = std::clamp(int(std::round(source.width * scale)), 1, width);
    transform.content_height = std::clamp(int(std::round(source.height * scale)), 1, height);
    transform.rounding_error_x = transform.content_width - source.width * scale;
    transform.rounding_error_y = transform.content_height - source.height * scale;
    transform.pad_x = (width - transform.content_width) / 2;
    transform.pad_y = (height - transform.content_height) / 2;
    result.canvas.width = width;
    result.canvas.height = height;
    result.canvas.rgb.assign(size_t(pixels) * 3, 255);
    for (int y = 0; y < transform.content_height; ++y) {
        const double sy = std::clamp((y + .5) / scale - .5, 0., double(source.height - 1));
        const int y0 = int(sy), y1 = std::min(y0 + 1, source.height - 1);
        const double fy = sy - y0;
        for (int x = 0; x < transform.content_width; ++x) {
            const double sx = std::clamp((x + .5) / scale - .5, 0., double(source.width - 1));
            const int x0 = int(sx), x1 = std::min(x0 + 1, source.width - 1);
            const double fx = sx - x0;
            for (int c = 0; c < 3; ++c) {
                auto at = [&](int xx, int yy) { return source.rgb[(size_t(yy) * source.width + xx) * 3 + c]; };
                const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
                const double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
                result.canvas.rgb[(size_t(y + transform.pad_y) * width + x + transform.pad_x) * 3 + c] =
                    uint8_t(std::round(top * (1 - fy) + bottom * fy));
            }
        }
    }
    return result;
}
}
