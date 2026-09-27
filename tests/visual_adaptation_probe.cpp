#include "visual_adaptation.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>

using dococr::Image;
using dococr::adapt_visual;

int main(int argc, char** argv) {
    if (argc == 3 || argc == 5) {
        int width = 0, height = 0, channels = 0;
        unsigned char* pixels = stbi_load(argv[1], &width, &height, &channels, 3);
        if (!pixels) return 2;
        Image source{width, height, std::vector<uint8_t>(pixels, pixels + size_t(width) * height * 3)};
        stbi_image_free(pixels);
        dococr::VisualBudget budget;
        if (argc == 5) {
            budget.min_pixels = std::stoll(argv[3]);
            budget.max_pixels = std::stoll(argv[4]);
        }
        const auto adapted = adapt_visual(source, budget);
        if (!stbi_write_png(argv[2], adapted.canvas.width, adapted.canvas.height, 3,
                            adapted.canvas.rgb.data(), adapted.canvas.width * 3)) return 3;
        std::cout << width << 'x' << height << " -> " << adapted.canvas.width << 'x'
                  << adapted.canvas.height << " content " << adapted.transform.content_width << 'x'
                  << adapted.transform.content_height << " pad " << adapted.transform.pad_x << ',' << adapted.transform.pad_y << '\n';
        return 0;
    }
    for (auto [width, height] : {std::pair{1,31}, {31,1}, {31,31}, {32,32},
                                 {323,31}, {82,9}, {640,480}, {4000,4000}}) {
        Image source{width, height, std::vector<uint8_t>(size_t(width) * height * 3, 127)};
        const auto result = adapt_visual(source);
        const auto& canvas = result.canvas;
        const auto& transform = result.transform;
        assert(canvas.width % 32 == 0 && canvas.height % 32 == 0);
        assert(int64_t(canvas.width) * canvas.height >= 65536);
        assert(int64_t(canvas.width) * canvas.height <= 560 * 560);
        assert(transform.pad_x >= 0 && transform.pad_y >= 0);
        assert(transform.pad_x + transform.content_width <= canvas.width);
        assert(transform.pad_y + transform.content_height <= canvas.height);
        const double sx = double(transform.content_width) / width;
        const double sy = double(transform.content_height) / height;
        assert(std::abs(transform.content_width - width * transform.scale - transform.rounding_error_x) < 1e-9);
        assert(std::abs(transform.content_height - height * transform.scale - transform.rounding_error_y) < 1e-9);
        assert(std::abs(sx - sy) <= .5 / std::min(width, height) + .5 / std::max(width, height));
        if (width == 323) assert(canvas.width == 832 && canvas.height == 96);
        if (width == 82) assert(canvas.width == 416 && canvas.height == 160);
        if (width == 640) assert(canvas.width == 640 && canvas.height == 480);
        if (width == 4000) assert(canvas.width == 544 && canvas.height == 544);
    }
    {
        Image large{819, 566, std::vector<uint8_t>(size_t(819) * 566 * 3, 127)};
        const auto limited = adapt_visual(large, {65536, 160000});
        const auto chosen = adapt_visual(large);
        assert(int64_t(limited.canvas.width) * limited.canvas.height <= 160000);
        assert(int64_t(chosen.canvas.width) * chosen.canvas.height <= 560 * 560);
        assert(chosen.transform.content_width > limited.transform.content_width);
        assert(chosen.transform.content_height > limited.transform.content_height);
    }
    {
        Image strip{10000, 100, std::vector<uint8_t>(size_t(10000) * 100 * 3, 127)};
        const auto result = adapt_visual(strip);
        assert(result.canvas.width == 4896 && result.canvas.height == 64);
        assert(result.transform.content_width == 4896 && result.transform.content_height == 49);
        assert(result.transform.pad_y == 7);
    }
    try {
        adapt_visual(Image{32, 32, std::vector<uint8_t>(32 * 32 * 3)}, {32000, 560 * 560});
        return 5;
    } catch (const std::runtime_error&) {}
    try {
        Image too_wide{6400, 31, std::vector<uint8_t>(size_t(6400) * 31 * 3)};
        adapt_visual(too_wide);
        return 4;
    } catch (const std::runtime_error&) {}
    return 0;
}
