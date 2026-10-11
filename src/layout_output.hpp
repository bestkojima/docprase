#pragma once
#include "layout_candidate.hpp"
#include "layout_preprocess.hpp"
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace dococr {
// 通用后端的四类映射，与 PP-DocLayoutV3 的类别表及浮点框契约分别处理。
struct GenericLayoutCandidate {
    std::string label;
    Box box;
    int rank;
    float detection_score;
    int class_id;
};
// 单个 layout_candidates 张量；页面尺寸来自已解码的有效页面。
// 成功返回完整的页内整数框，按 y0/x0/y1/x1/label 排序（不使用 rank/score）。
// 任意张量、数值或裁框非法则返回空结果；不 clamp、不筛除，也不返回部分候选。
std::optional<std::vector<GenericLayoutCandidate>> decode_generic_layout_output(
    const TensorOutput& output, int page_width, int page_height);

struct LayoutDecodeOptions {
    int width = 0, height = 0;
    LayoutPageTransform transform;
    double score_threshold = .5;
};
class DecodedLayoutOutput;
// 只读行视图；有效期不超过持有原始张量的解码结果。
class LayoutMaskRow {
public:
    static constexpr size_t width = 200, pixel_count = width * width;
    int32_t pixel(size_t index) const {
        if (index >= pixel_count) throw std::out_of_range("layout_mask_pixel");
        int32_t bit;
        std::memcpy(&bit, data_ + index * sizeof(bit), sizeof(bit));
        return bit;
    }
    std::string_view bytes() const {
        return {reinterpret_cast<const char*>(data_), pixel_count * sizeof(int32_t)};
    }
private:
    explicit LayoutMaskRow(const uint8_t* data) : data_(data) {}
    const uint8_t* data_;
    friend class DecodedLayoutOutput;
};
// 成功结果拥有完整原始张量；移动交接不复制 mask，不保存指向自身成员的指针。
class DecodedLayoutOutput {
public:
    static constexpr size_t mask_count = 300;
    DecodedLayoutOutput(DecodedLayoutOutput&&) = default;
    DecodedLayoutOutput& operator=(DecodedLayoutOutput&&) = default;
    DecodedLayoutOutput(const DecodedLayoutOutput&) = delete;
    DecodedLayoutOutput& operator=(const DecodedLayoutOutput&) = delete;
    std::vector<LayoutCandidateRecord> candidates;
    const std::vector<Tensor>& tensors() const { return tensors_.outputs; }
    LayoutMaskRow mask_row(size_t index) const;
    std::string_view candidate_row_bytes(size_t index) const;
    std::vector<uint8_t> masks_rle() const;
private:
    DecodedLayoutOutput(TensorOutput tensors, std::vector<LayoutCandidateRecord> records,
                        size_t rows_index, size_t masks_index, size_t count);
    TensorOutput tensors_;
    size_t rows_index_, masks_index_, count_;
    friend std::optional<DecodedLayoutOutput> decode_layout_output(
        TensorOutput output, const LayoutDecodeOptions& options);
};
// 页面尺寸与变换来自已完成的预处理。非法模型输出返回空结果，不返回部分候选。
// 仅解码 PP-DocLayoutV3；不筛选重复框、不规划页面、不执行模型或创建图片资源。
std::optional<DecodedLayoutOutput> decode_layout_output(
    TensorOutput output, const LayoutDecodeOptions& options);
}
