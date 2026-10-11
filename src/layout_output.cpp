#include "layout_output.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace dococr {
namespace {
void append_le32(std::vector<uint8_t>& data, uint32_t value) {
    for (int i = 0; i < 4; ++i) data.push_back(uint8_t(value >> (8*i)));
}

std::vector<uint8_t> encode_masks_rle(const uint8_t* masks) {
    const std::string magic = "DOCOCR_MASK_RLE_V1\n";
    std::vector<uint8_t> encoded(magic.begin(), magic.end());
    for (uint32_t dimension : {300u, 200u, 200u}) append_le32(encoded, dimension);
    for (int row = 0; row < 300; ++row) {
        const uint8_t* raw = masks + size_t(row)*200*200*sizeof(int32_t);
        uint32_t current = 0, length = 0;
        std::memcpy(&current, raw, sizeof(current));
        std::vector<uint32_t> runs;
        for (int pixel = 0; pixel < 200*200; ++pixel) {
            uint32_t bit;
            std::memcpy(&bit, raw + size_t(pixel)*sizeof(bit), sizeof(bit));
            if (bit == current) ++length;
            else { runs.push_back(length); current = bit; length = 1; }
        }
        runs.push_back(length);
        uint32_t first;
        std::memcpy(&first, raw, sizeof(first));
        append_le32(encoded, first);
        append_le32(encoded, uint32_t(runs.size()));
        for (uint32_t run : runs) append_le32(encoded, run);
    }
    return encoded;
}

} // namespace

std::optional<std::vector<GenericLayoutCandidate>> decode_generic_layout_output(
    const TensorOutput& result, int page_width, int page_height) {
    std::vector<GenericLayoutCandidate> candidates;
    if (result.outputs.size() != 1) return std::nullopt;
    const Tensor& tensor = result.outputs[0];
    if (tensor.name != "layout_candidates" || tensor.dtype != DataType::Float32 ||
        tensor.layout != TensorLayout::Matrix || tensor.shape.size() != 2 ||
        tensor.shape[0] < 0 || tensor.shape[0] > 10000 || tensor.shape[1] != 7 ||
        tensor.data.size() != size_t(tensor.shape[0]) * 7 * sizeof(float)) return std::nullopt;
    const char* labels[] = {"text", "formula", "table", "image"};
    for (int64_t i = 0; i < tensor.shape[0]; ++i) {
        float row[7];
        std::memcpy(row, tensor.data.data() + size_t(i)*7*sizeof(float), sizeof(row));
        for (float v : row) if (!std::isfinite(v)) return std::nullopt;
        if (row[0] < 0 || row[0] > 1000000 || std::trunc(row[0]) != row[0] ||
            row[1] < 0 || row[1] > 1 || std::trunc(row[6]) != row[6] ||
            row[6] < 0 || row[6] > 1000000) return std::nullopt;
        for (int j = 2; j <= 5; ++j)
            if (row[j] < -1000000 || row[j] > 1000000 || std::trunc(row[j]) != row[j]) return std::nullopt;
        int class_id = int(row[0]);
        candidates.push_back({class_id < 4 ? labels[class_id] : "unknown",
            {int(row[2]), int(row[3]), int(row[4]), int(row[5])}, int(row[6]), row[1], class_id});
    }
    for (const auto& candidate : candidates) {
        const Box b = candidate.box;
        if (b.x0 < 0 || b.y0 < 0 || b.x1 > page_width || b.y1 > page_height ||
            b.x0 >= b.x1 || b.y0 >= b.y1) return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.box.y0 != b.box.y0) return a.box.y0 < b.box.y0;
        if (a.box.x0 != b.box.x0) return a.box.x0 < b.box.x0;
        if (a.box.y1 != b.box.y1) return a.box.y1 < b.box.y1;
        if (a.box.x1 != b.box.x1) return a.box.x1 < b.box.x1;
        return a.label < b.label;
    });
    return candidates;
}

DecodedLayoutOutput::DecodedLayoutOutput(TensorOutput tensors, std::vector<LayoutCandidateRecord> records,
                                         size_t rows_index, size_t masks_index, size_t count)
    : candidates(std::move(records)), tensors_(std::move(tensors)),
      rows_index_(rows_index), masks_index_(masks_index), count_(count) {}

LayoutMaskRow DecodedLayoutOutput::mask_row(size_t index) const {
    if (index >= mask_count) throw std::out_of_range("layout_mask_row");
    return LayoutMaskRow(tensors_.outputs[masks_index_].data.data() +
                         index * LayoutMaskRow::pixel_count * sizeof(int32_t));
}
std::string_view DecodedLayoutOutput::candidate_row_bytes(size_t index) const {
    if (index >= count_) throw std::out_of_range("layout_candidate_row");
    return {reinterpret_cast<const char*>(tensors_.outputs[rows_index_].data.data() +
                                         index * 7 * sizeof(float)), 7 * sizeof(float)};
}
std::vector<uint8_t> DecodedLayoutOutput::masks_rle() const {
    return encode_masks_rle(tensors_.outputs[masks_index_].data.data());
}

std::optional<DecodedLayoutOutput> decode_layout_output(TensorOutput output, const LayoutDecodeOptions& options) {
    if (output.outputs.size() != 3) return std::nullopt;
    const Tensor *rows = nullptr, *count = nullptr, *mask = nullptr;
    size_t rows_index = 0, masks_index = 0;
    for (size_t i = 0; i < output.outputs.size(); ++i) {
        const Tensor& tensor = output.outputs[i];
        if (tensor.name == "fetch_name_0") { rows = &tensor; rows_index = i; }
        else if (tensor.name == "fetch_name_1") count = &tensor;
        else if (tensor.name == "fetch_name_2") { mask = &tensor; masks_index = i; }
        else return std::nullopt;
    }
    if (!rows || !count || !mask || rows->dtype != DataType::Float32 ||
        rows->layout != TensorLayout::Matrix || rows->shape != std::vector<int64_t>{300,7} ||
        rows->data.size() != 300*7*sizeof(float) || count->dtype != DataType::Int32 ||
        count->layout != TensorLayout::Matrix ||
        count->shape != std::vector<int64_t>{1} || count->data.size() != sizeof(int32_t) ||
        mask->dtype != DataType::Int32 || mask->layout != TensorLayout::Matrix ||
        mask->shape != std::vector<int64_t>{300,200,200} ||
        mask->data.size() != 300*200*200*sizeof(int32_t)) return std::nullopt;
    int32_t n;
    std::memcpy(&n, count->data.data(), sizeof(n));
    if (n < 0 || n > 300) return std::nullopt;
    const uint8_t* masks = mask->data.data();
    const auto* transform = &options.transform;
    std::vector<LayoutCandidateRecord> records;
    for (int i = 0; i < n; ++i) {
        float row[7];
        std::memcpy(row, rows->data.data() + size_t(i)*7*sizeof(float), sizeof(row));
        for (float value : row) if (!std::isfinite(value)) return std::nullopt;
        float model_box[4];
        std::copy(row+2, row+6, model_box);
        if (transform && transform->applied) {
            row[2] = float((row[2] - transform->pad_x) / transform->scale_x);
            row[4] = float((row[4] - transform->pad_x) / transform->scale_x);
            row[3] = float((row[3] - transform->pad_y) / transform->scale_y);
            row[5] = float((row[5] - transform->pad_y) / transform->scale_y);
            for (int k = 2; k < 6; ++k) if (!std::isfinite(row[k])) return std::nullopt;
        }
        if (row[1] < 0 || row[1] > 1 || row[0] < 0 || row[0] > 1000000 ||
            std::trunc(row[0]) != row[0] || row[6] < -1 || row[6] > 1000000 ||
            std::trunc(row[6]) != row[6]) return std::nullopt;
        LayoutCandidateRecord candidate;
        candidate.id = i;
        candidate.class_id = int(row[0]);
        candidate.score = row[1];
        candidate.rank = int(row[6]);
        for (int k = 0; k < 4; ++k) {
            candidate.box[k] = row[k+2];
            candidate.model_box[k] = model_box[k];
        }
        const char* label = layout_region_policy(candidate.class_id).model_label;
        candidate.label = label ? label : "unknown";
        if (candidate.score < float(options.score_threshold)) candidate.reason = "below_score_threshold";
        else if (row[4] <= row[2] || row[5] <= row[3]) candidate.reason = "degenerate_box";
        else if (row[4] <= 0 || row[5] <= 0 || row[2] >= options.width || row[3] >= options.height)
            candidate.reason = "outside_page";
        else if (std::abs(double(row[2])) > 10000000 || std::abs(double(row[3])) > 10000000 ||
                 std::abs(double(row[4])) > 10000000 || std::abs(double(row[5])) > 10000000)
            candidate.reason = "box_out_of_supported_range";
        else {
            candidate.crop = {int(std::floor(std::clamp(double(row[2]), 0.0, double(options.width)))),
                              int(std::floor(std::clamp(double(row[3]), 0.0, double(options.height)))),
                              int(std::ceil(std::clamp(double(row[4]), 0.0, double(options.width)))),
                              int(std::ceil(std::clamp(double(row[5]), 0.0, double(options.height))))};
            if (candidate.crop.x0 >= candidate.crop.x1 || candidate.crop.y0 >= candidate.crop.y1)
                candidate.reason = "empty_clamped_box";
            else {
                candidate.selected = true;
                candidate.clamped = row[2] < 0 || row[3] < 0 || row[4] > options.width || row[5] > options.height;
            }
        }
        const uint8_t* mask_row = masks + size_t(i)*200*200*sizeof(int32_t);
        for (int j = 0; j < 200*200; ++j) {
            int32_t value;
            std::memcpy(&value, mask_row + size_t(j)*sizeof(value), sizeof(value));
            if (value != 0 && value != 1) return std::nullopt;
            candidate.mask_nonzero += value;
        }
        records.push_back(std::move(candidate));
    }
    for (int i = n; i < 300; ++i) for (int j = 0; j < 200*200; ++j) {
        int32_t value;
        std::memcpy(&value, masks + (size_t(i)*200*200+j)*sizeof(value), sizeof(value));
        if (value != 0 && value != 1) return std::nullopt;
    }
    DecodedLayoutOutput result(std::move(output), std::move(records), rows_index, masks_index, size_t(n));
    return std::optional<DecodedLayoutOutput>(std::move(result));
}
} // namespace dococr
