#pragma once
#include "dococr/inference.hpp"
#include "layout_output.hpp"
#include <optional>

namespace dococr::image_resources {
// 输入格式、步长和像素预算按既有规则检查，统一解码为连续 RGB8。
std::optional<Image> decode(InputView input, uint64_t pixel_limit);
// 输入为有效连续 RGB8；非法裁框抛出 invalid_argument，不修改原图。
Image crop(const Image& image, Box box);
std::vector<uint8_t> crop_png(const Image& image, Box box);
struct LayoutRasterSource {
    int width, height;
    const DecodedLayoutOutput& output;
    const LayoutPageTransform& transform;
};
enum class MaskPurpose { Diagnostic, CandidateReview };
// 返回整页灰度 mask PNG；来源和几何须来自同一页的解码及预处理。
std::vector<uint8_t> layout_mask_png(const LayoutCandidateRecord& candidate,
    const LayoutRasterSource& source, MaskPurpose purpose = MaskPurpose::Diagnostic);
// smartresize 使用预处理 preview；reference 使用原图，保持原候选叠加顺序。
std::vector<uint8_t> layout_overlay_png(const Image& image,
    const std::vector<LayoutCandidateRecord>& candidates, const LayoutRasterSource& source,
    const Image& preview);
}
