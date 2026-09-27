#include "layout_preprocess.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dococr {
namespace {
// 固定参考的 uint8 antialias=false 路径：每轴使用有符号定点权重并分别舍入到 uint8。
double cubic(double x) {
    x = std::abs(x);
    if (x < 1.0) return (1.25*x - 2.25)*x*x + 1.0;
    if (x < 2.0) return ((-0.75*x + 3.75)*x - 6.0)*x + 3.0;
    return 0.0;
}
struct Axis {
    int first = 0, count = 0;
    double weight[4]{};
    int16_t fixed[4]{};
};
std::vector<Axis> axes(int input_size, int output_size, unsigned& precision) {
    std::vector<Axis> result(output_size);
    double maximum = 0;
    const double scale = double(input_size) / output_size;
    for (int i = 0; i < output_size; ++i) {
        const double source = (i + 0.5) * scale - 0.5;
        const int base = int(std::floor(source));
        const double lambda = source - base;
        Axis& a = result[i];
        a.first = std::max(0, base - 1);
        a.count = std::clamp(std::min(base + 3, input_size) - a.first, 1, 4);
        int cursor = 0;
        for (int j = 0; j < 4; ++j) {
            const int source_index = base - 1 + j;
            if (source_index <= 0) cursor = 0;
            else if (source_index >= input_size - 1) cursor = a.count - 1;
            a.weight[cursor] += cubic(double(j - 1) - lambda);
            maximum = std::max(maximum, a.weight[cursor]);
            ++cursor;
        }
    }
    precision = 0;
    while (precision < 22 && int(0.5 + maximum * (1u << (precision + 1))) < (1 << 15))
        ++precision;
    for (Axis& a : result) for (int j = 0; j < 4; ++j) {
        double scaled = a.weight[j] * (1u << precision);
        a.fixed[j] = int16_t(scaled < 0 ? int(scaled - 0.5) : int(scaled + 0.5));
    }
    return result;
}
uint8_t convolve(const uint8_t* input, int stride, const Axis& axis, unsigned precision) {
    int sum = 1 << (precision - 1);
    for (int k = 0; k < axis.count; ++k)
        sum += int(input[(axis.first + k)*stride]) * axis.fixed[k];
    return uint8_t(std::clamp(sum >> precision, 0, 255));
}
}
Tensor layout_image_tensor(const Image& image) {
    Tensor result{"image", DataType::Float32, TensorLayout::NCHW,
                  {1, 3, 800, 800}, std::vector<uint8_t>(3 * 800 * 800 * sizeof(float))};
    unsigned x_precision = 0, y_precision = 0;
    auto xs = axes(image.width, 800, x_precision);
    auto ys = axes(image.height, 800, y_precision);
    std::vector<uint8_t> horizontal(size_t(image.height) * 800 * 3);
    if (image.width == 800) horizontal = image.rgb;
    else for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < 800; ++x) for (int c = 0; c < 3; ++c)
            horizontal[(size_t(y)*800 + x)*3 + c] =
                convolve(image.rgb.data() + size_t(y)*image.width*3 + c, 3, xs[x], x_precision);
    for (int y = 0; y < 800; ++y) for (int x = 0; x < 800; ++x) for (int c = 0; c < 3; ++c) {
        uint8_t value = image.height == 800 ? horizontal[(size_t(y)*800+x)*3+c] :
            convolve(horizontal.data() + x*3+c, 800*3, ys[y], y_precision);
        float normalized = float(value) / 255.0f;
        size_t offset = (size_t(c)*800*800 + size_t(y)*800 + x)*sizeof(float);
        std::memcpy(result.data.data() + offset, &normalized, sizeof(float));
    }
    return result;
}

LayoutPageInput prepare_layout_page(const Image& image, bool smartresize) {
    if (!smartresize) return {layout_image_tensor(image), {}, {}};
    const double scale = std::min(800.0 / image.width, 800.0 / image.height);
    LayoutPageTransform transform;
    transform.applied = true;
    transform.content_width = std::clamp(int(std::round(image.width * scale)), 1, 800);
    transform.content_height = std::clamp(int(std::round(image.height * scale)), 1, 800);
    transform.pad_x = (800 - transform.content_width) / 2;
    transform.pad_y = (800 - transform.content_height) / 2;
    transform.scale_x = double(transform.content_width) / image.width;
    transform.scale_y = double(transform.content_height) / image.height;
    Image canvas{800, 800, std::vector<uint8_t>(800 * 800 * 3, 255)};
    for (int y = 0; y < transform.content_height; ++y) {
        double sy = std::clamp((y + 0.5) / transform.scale_y - 0.5,
                               0.0, double(image.height - 1));
        int y0 = int(sy), y1 = std::min(y0 + 1, image.height - 1);
        double fy = sy - y0;
        for (int x = 0; x < transform.content_width; ++x) {
            double sx = std::clamp((x + 0.5) / transform.scale_x - 0.5,
                                   0.0, double(image.width - 1));
            int x0 = int(sx), x1 = std::min(x0 + 1, image.width - 1);
            double fx = sx - x0;
            for (int c = 0; c < 3; ++c) {
                auto at = [&](int xx, int yy) {
                    return image.rgb[(size_t(yy) * image.width + xx) * 3 + c];
                };
                double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
                double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
                canvas.rgb[(size_t(y + transform.pad_y) * 800 + x + transform.pad_x) * 3 + c] =
                    uint8_t(std::round(top * (1 - fy) + bottom * fy));
            }
        }
    }
    return {layout_image_tensor(canvas), transform, std::move(canvas)};
}
} // namespace dococr
