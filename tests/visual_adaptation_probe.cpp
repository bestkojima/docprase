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
    if (argc == 3) {
        int width = 0, height = 0, channels = 0;
        unsigned char* pixels = stbi_load(argv[1], &width, &height, &channels, 3);
        if (!pixels) return 2;
        Image source{width, height, std::vector<uint8_t>(pixels, pixels + size_t(width) * height * 3)};
        stbi_image_free(pixels);
        const auto adapted = adapt_visual(source);
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
        assert(int64_t(canvas.width) * canvas.height <= 16777216);
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
    }
    try {
        Image too_wide{6400, 31, std::vector<uint8_t>(size_t(6400) * 31 * 3)};
        adapt_visual(too_wide);
        return 4;
    } catch (const std::runtime_error&) {}
    return 0;
}
