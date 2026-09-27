#ifndef DOCOCR_LAYOUT_PREPROCESS_HPP
#define DOCOCR_LAYOUT_PREPROCESS_HPP
#include "dococr/inference.hpp"
namespace dococr {
Tensor layout_image_tensor(const Image& image);
struct LayoutPageTransform {
    bool applied = false;
    int content_width = 0, content_height = 0;
    int pad_x = 0, pad_y = 0;
    double scale_x = 1, scale_y = 1;
};
struct LayoutPageInput {
    Tensor tensor;
    LayoutPageTransform transform;
    Image preview;
};
LayoutPageInput prepare_layout_page(const Image& image, bool smartresize);
}
#endif
