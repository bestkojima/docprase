#include "image_resources.hpp"
#include "dococr/dococr.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBIW_ONLY_PNG
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace dococr::image_resources {
namespace {
constexpr size_t max_encoded_bytes = 64 * 1024 * 1024;
void validate_image(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        uint64_t(image.width) * uint64_t(image.height) > std::numeric_limits<size_t>::max() / 3 ||
        image.rgb.size() != size_t(image.width) * size_t(image.height) * 3)
        throw std::invalid_argument("invalid_rgb_image");
}
bool decode_pixels(InputView in, Image& image, uint64_t pixel_limit) {
    if (!in.data || in.size == 0) return false;
    if (in.format == DOCOCR_IMAGE_RGB8 || in.format == DOCOCR_IMAGE_GRAY8) {
        uint64_t pixels = uint64_t(in.width) * in.height;
        size_t channels = in.format == DOCOCR_IMAGE_RGB8 ? 3 : 1;
        if (!in.width || !in.height || pixels > pixel_limit ||
            in.width > std::numeric_limits<size_t>::max() / channels) return false;
        size_t row_size = size_t(in.width) * channels;
        if (in.row_stride < row_size || in.row_stride > in.size ||
            (size_t(in.height)-1) > (in.size-row_size)/in.row_stride) return false;
        image.width = int(in.width);
        image.height = int(in.height);
        image.rgb.resize(size_t(pixels) * 3);
        for (uint32_t y = 0; y < in.height; ++y) {
            const uint8_t* src = in.data + size_t(y) * in.row_stride;
            uint8_t* dst = image.rgb.data() + size_t(y) * in.width * 3;
            if (channels == 3) std::memcpy(dst, src, row_size);
            else for (uint32_t x = 0; x < in.width; ++x)
                dst[3*x] = dst[3*x+1] = dst[3*x+2] = src[x];
        }
        return true;
    }
    if (in.format != DOCOCR_IMAGE_PNG && in.format != DOCOCR_IMAGE_JPEG) return false;
    if (in.size > max_encoded_bytes || in.size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        in.width || in.height || in.row_stride) return false;
    const bool png = in.size >= 8 && std::memcmp(in.data, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = in.size >= 3 && in.data[0] == 0xff && in.data[1] == 0xd8 && in.data[2] == 0xff;
    if ((in.format == DOCOCR_IMAGE_PNG && !png) || (in.format == DOCOCR_IMAGE_JPEG && !jpeg)) return false;
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(in.data, int(in.size), &w, &h, &channels) ||
        stbi_is_16_bit_from_memory(in.data, int(in.size)) ||
        w <= 0 || h <= 0 || uint64_t(w) * h > pixel_limit ||
        (channels != 1 && channels != 3)) return false;
    int decoded_channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(in.data, int(in.size), &w, &h, &decoded_channels, 3);
    if (!decoded) return false;
    std::unique_ptr<stbi_uc, void(*)(void*)> decoded_owner(decoded, stbi_image_free);
    image.width = w; image.height = h;
    image.rgb.assign(decoded, decoded + size_t(w) * h * 3);
    return true;
}

void png_write(void* context, void* data, int size) {
    auto* result = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<uint8_t*>(data);
    result->insert(result->end(), bytes, bytes + size);
}

std::vector<uint8_t> layout_page_mask(const LayoutCandidateRecord& c, const LayoutRasterSource& source) {
    const auto& output = source.output;
    const auto* transform = &source.transform;
    std::vector<uint8_t> pixels(size_t(source.width)*source.height);
    if (transform && transform->applied) {
        const auto mask = output.mask_row(size_t(c.id));
        for (int y = c.crop.y0; y < c.crop.y1; ++y) {
            int my = std::clamp(int(((y + 0.5) * transform->scale_y +
                                      transform->pad_y) / 4), 0, 199);
            for (int x = c.crop.x0; x < c.crop.x1; ++x) {
                int mx = std::clamp(int(((x + 0.5) * transform->scale_x +
                                          transform->pad_x) / 4), 0, 199);
                int32_t bit;
                bit = mask.pixel(size_t(my)*200+mx);
                pixels[size_t(y)*source.width+x] = bit ? 255 : 0;
            }
        }
        return pixels;
    }
    int x0 = int(c.box[0]), y0 = int(c.box[1]);
    int x1 = int(c.box[2]), y1 = int(c.box[3]);
    if (x1 <= x0 || y1 <= y0) return pixels;
    int mx0 = std::clamp(int(std::nearbyint(double(x0)*200/source.width)), 0, 200);
    int my0 = std::clamp(int(std::nearbyint(double(y0)*200/source.height)), 0, 200);
    int mx1 = std::clamp(int(std::nearbyint(double(x1)*200/source.width)), 0, 200);
    int my1 = std::clamp(int(std::nearbyint(double(y1)*200/source.height)), 0, 200);
    if (mx1 <= mx0 || my1 <= my0) return pixels;
    const auto mask = output.mask_row(size_t(c.id));
    for (int y = std::max(0,y0); y < std::min(source.height,y1); ++y)
        for (int x = std::max(0,x0); x < std::min(source.width,x1); ++x) {
        int mx = std::clamp(mx0 + int((int64_t(x-x0)*(mx1-mx0))/(x1-x0)), mx0, mx1-1);
        int my = std::clamp(my0 + int((int64_t(y-y0)*(my1-my0))/(y1-y0)), my0, my1-1);
        int32_t bit;
        bit = mask.pixel(size_t(my)*200+mx);
        pixels[size_t(y)*source.width+x] = bit ? 255 : 0;
    }
    return pixels;
}

} // namespace

std::optional<Image> decode(InputView input, uint64_t pixel_limit) {
    Image image;
    if (!decode_pixels(input, image, pixel_limit)) return std::nullopt;
    return image;
}

Image crop(const Image& image, Box b) {
    validate_image(image);
    if (b.x0 < 0 || b.y0 < 0 || b.x1 > image.width || b.y1 > image.height ||
        b.x0 >= b.x1 || b.y0 >= b.y1) throw std::invalid_argument("invalid_crop_box");
    int w = b.x1 - b.x0, h = b.y1 - b.y0;
    Image crop;
    crop.width = w; crop.height = h;
    crop.rgb.resize(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        std::memcpy(crop.rgb.data() + size_t(y)*w*3,
                    image.rgb.data() + (size_t(b.y0+y)*image.width+b.x0)*3,
                    size_t(w)*3);
    return crop;
}

std::vector<uint8_t> crop_png(const Image& image, Box b) {
    Image cropped = crop(image, b);
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_write, &png, cropped.width, cropped.height, 3,
                                cropped.rgb.data(), cropped.width*3))
        throw std::runtime_error("PNG encoding failed");
    return png;
}

std::vector<uint8_t> layout_mask_png(const LayoutCandidateRecord& candidate,
    const LayoutRasterSource& source, MaskPurpose purpose) {
    auto pixels = layout_page_mask(candidate, source);
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_write, &png, source.width, source.height, 1,
                                pixels.data(), source.width))
        throw std::runtime_error(purpose == MaskPurpose::CandidateReview ?
            "review mask PNG encoding failed" : "layout mask PNG encoding failed");
    return png;
}

std::vector<uint8_t> layout_overlay_png(const Image& image,
                                    const std::vector<LayoutCandidateRecord>& records,
                                    const LayoutRasterSource& source, const Image& preview_image) {
    const auto& output = source.output;
    const auto* transform = &source.transform;
    const auto* preview = &preview_image;
    validate_image(image);
    const bool smart = transform->applied;
    if (smart) validate_image(*preview);
    Image overlay = smart ? *preview : image;
    for (const auto& c : records) {
        if (!c.selected) continue;
        Box draw = smart ? Box{
            std::clamp(int(std::floor(c.model_box[0])), 0, overlay.width),
            std::clamp(int(std::floor(c.model_box[1])), 0, overlay.height),
            std::clamp(int(std::ceil(c.model_box[2])), 0, overlay.width),
            std::clamp(int(std::ceil(c.model_box[3])), 0, overlay.height)} : c.crop;
        if (draw.x0 >= draw.x1 || draw.y0 >= draw.y1) continue;
        std::vector<uint8_t> page_mask;
        if (!smart) page_mask = layout_page_mask(c, source);
        const auto mask = output.mask_row(size_t(c.id));
        for (int y = draw.y0; y < draw.y1; ++y) for (int x = draw.x0; x < draw.x1; ++x) {
            int32_t bit = 0;
            if (smart) bit = mask.pixel(size_t(std::clamp(y/4, 0, 199))*200 +
                                         std::clamp(x/4, 0, 199));
            if ((smart && bit) || (!smart && page_mask[size_t(y)*image.width+x])) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*overlay.width+x)*3;
                pixel[0] = uint8_t(pixel[0]/2);
                pixel[1] = uint8_t(pixel[1]/2 + 127);
                pixel[2] = uint8_t(pixel[2]/2);
            }
        }
        for (int x = draw.x0; x < draw.x1; ++x) {
            for (int y : {draw.y0, draw.y1-1}) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*overlay.width+x)*3;
                pixel[0] = 255; pixel[1] = 0; pixel[2] = 0;
            }
        }
        for (int y = draw.y0; y < draw.y1; ++y) {
            for (int x : {draw.x0, draw.x1-1}) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*overlay.width+x)*3;
                pixel[0] = 255; pixel[1] = 0; pixel[2] = 0;
            }
        }
    }
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_write, &png, overlay.width, overlay.height, 3,
                                overlay.rgb.data(), overlay.width*3))
        throw std::runtime_error("layout overlay PNG encoding failed");
    return png;
}

} // namespace dococr::image_resources
