#include "dococr/inference.hpp"
#include "dococr/dococr.h"
#include "config.hpp"
#include "layout_preprocess.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
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

namespace dococr {
namespace {
constexpr uint64_t max_pixels = 16000000;
constexpr size_t max_encoded_bytes = 64 * 1024 * 1024;

std::string id(const char prefix, size_t index) {
    std::ostringstream out;
    out << prefix << std::setw(4) << std::setfill('0') << index;
    return out.str();
}

std::string box_json(Box b) {
    return "[" + std::to_string(b.x0) + "," + std::to_string(b.y0) + "," +
           std::to_string(b.x1) + "," + std::to_string(b.y1) + "]";
}

bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
        if (!n || i + static_cast<size_t>(n) > s.size()) return false;
        unsigned char c1 = static_cast<unsigned char>(s[i+1]);
        if ((c1 & 0xC0) != 0x80 || (c == 0xE0 && c1 < 0xA0) ||
            (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) ||
            (c == 0xF4 && c1 >= 0x90)) return false;
        for (int j = 2; j < n; ++j)
            if ((static_cast<unsigned char>(s[i+j]) & 0xC0) != 0x80) return false;
        i += static_cast<size_t>(n);
    }
    return true;
}

std::string base64(const std::string& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t a = static_cast<uint8_t>(bytes[i]);
        uint32_t b = i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i+1]) : 0;
        uint32_t c = i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i+2]) : 0;
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return out;
}

bool decode(InputView in, Image& image) {
    if (!in.data || in.size == 0) return false;
    if (in.format == DOCOCR_IMAGE_RGB8 || in.format == DOCOCR_IMAGE_GRAY8) {
        uint64_t pixels = uint64_t(in.width) * in.height;
        size_t channels = in.format == DOCOCR_IMAGE_RGB8 ? 3 : 1;
        if (!in.width || !in.height || pixels > max_pixels ||
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
        w <= 0 || h <= 0 || uint64_t(w) * h > max_pixels ||
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

Image crop_rgb(const Image& image, Box b) {
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
    Image crop = crop_rgb(image, b);
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_write, &png, crop.width, crop.height, 3,
                                crop.rgb.data(), crop.width*3))
        throw std::runtime_error("PNG encoding failed");
    return png;
}

std::string document_id(const Image& image) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ull; };
    for (int i = 0; i < 4; ++i) { mix(uint8_t(uint32_t(image.width) >> (8*i))); mix(uint8_t(uint32_t(image.height) >> (8*i))); }
    for (uint8_t byte : image.rgb) mix(byte);
    std::ostringstream out;
    out << "doc-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

struct Block {
    std::string id, layout_id, region_id, type, status, text, raw, raw_base64, text_base64,
                error, error_base64, resource;
    Box box;
    float detection_score = 0;
    int candidate_rank = 0;
    int original_class_id = 0;
    int candidate_id = -1, mask_row = -1, mask_nonzero = 0;
    std::string model_label;
    float raw_box[4]{};
    bool clamped = false;
};

struct RawLayoutCandidate {
    int id = 0, class_id = 0, rank = 0, mask_nonzero = 0;
    float score = 0, box[4]{};
    std::string label, reason;
    std::string mask_asset;
    bool selected = false, clamped = false;
    Box crop;
};

const char* layout_label(int id) {
    static const char* labels[] = {
        "abstract", "algorithm", "aside_text", "chart", "content", "formula", "doc_title",
        "figure_title", "footer", "footer", "footnote", "formula_number", "header", "header",
        "image", "formula", "number", "paragraph_title", "reference", "reference_content",
        "seal", "table", "text", "text", "vision_footnote"};
    return id >= 0 && id < 25 ? labels[id] : nullptr;
}

std::string canonical_label(int id) {
    if (id == 5 || id == 15) return "formula";
    if (id == 21) return "table";
    if (id == 3 || id == 14 || id == 20) return "image";
    return layout_label(id) ? "text" : "unknown";
}

bool decode_real_layout(const TensorOutput& output, const Image& image,
                        std::vector<RawLayoutCandidate>& records,
                        const uint8_t*& masks) {
    if (output.outputs.size() != 3) return false;
    const Tensor *rows = nullptr, *count = nullptr, *mask = nullptr;
    for (const Tensor& tensor : output.outputs) {
        if (tensor.name == "fetch_name_0") rows = &tensor;
        else if (tensor.name == "fetch_name_1") count = &tensor;
        else if (tensor.name == "fetch_name_2") mask = &tensor;
        else return false;
    }
    if (!rows || !count || !mask || rows->dtype != DataType::Float32 ||
        rows->layout != TensorLayout::Matrix || rows->shape != std::vector<int64_t>{300,7} ||
        rows->data.size() != 300*7*sizeof(float) || count->dtype != DataType::Int32 ||
        count->layout != TensorLayout::Matrix ||
        count->shape != std::vector<int64_t>{1} || count->data.size() != sizeof(int32_t) ||
        mask->dtype != DataType::Int32 || mask->layout != TensorLayout::Matrix ||
        mask->shape != std::vector<int64_t>{300,200,200} ||
        mask->data.size() != 300*200*200*sizeof(int32_t)) return false;
    int32_t n;
    std::memcpy(&n, count->data.data(), sizeof(n));
    if (n < 0 || n > 300) return false;
    masks = mask->data.data();
    for (int i = 0; i < n; ++i) {
        float row[7];
        std::memcpy(row, rows->data.data() + size_t(i)*7*sizeof(float), sizeof(row));
        for (float value : row) if (!std::isfinite(value)) return false;
        if (row[1] < 0 || row[1] > 1 || row[0] < 0 || row[0] > 1000000 ||
            std::trunc(row[0]) != row[0] || row[6] < 0 || row[6] > 1000000 ||
            std::trunc(row[6]) != row[6]) return false;
        RawLayoutCandidate candidate;
        candidate.id = i;
        candidate.class_id = int(row[0]);
        candidate.score = row[1];
        candidate.rank = int(row[6]);
        for (int k = 0; k < 4; ++k) candidate.box[k] = row[k+2];
        const char* label = layout_label(candidate.class_id);
        candidate.label = label ? label : "unknown";
        if (candidate.score < 0.5f) candidate.reason = "below_score_threshold";
        else if (row[4] <= row[2] || row[5] <= row[3]) candidate.reason = "degenerate_box";
        else if (row[4] <= 0 || row[5] <= 0 || row[2] >= image.width || row[3] >= image.height)
            candidate.reason = "outside_page";
        else if (std::abs(double(row[2])) > 10000000 || std::abs(double(row[3])) > 10000000 ||
                 std::abs(double(row[4])) > 10000000 || std::abs(double(row[5])) > 10000000)
            candidate.reason = "box_out_of_supported_range";
        else {
            candidate.crop = {int(std::floor(std::clamp(double(row[2]), 0.0, double(image.width)))),
                              int(std::floor(std::clamp(double(row[3]), 0.0, double(image.height)))),
                              int(std::ceil(std::clamp(double(row[4]), 0.0, double(image.width)))),
                              int(std::ceil(std::clamp(double(row[5]), 0.0, double(image.height))))};
            if (candidate.crop.x0 >= candidate.crop.x1 || candidate.crop.y0 >= candidate.crop.y1)
                candidate.reason = "empty_clamped_box";
            else {
                candidate.selected = true;
                candidate.clamped = row[2] < 0 || row[3] < 0 || row[4] > image.width || row[5] > image.height;
            }
        }
        const uint8_t* mask_row = masks + size_t(i)*200*200*sizeof(int32_t);
        for (int j = 0; j < 200*200; ++j) {
            int32_t value;
            std::memcpy(&value, mask_row + size_t(j)*sizeof(value), sizeof(value));
            if (value != 0 && value != 1) return false;
            candidate.mask_nonzero += value;
        }
        records.push_back(std::move(candidate));
    }
    for (int i = n; i < 300; ++i) for (int j = 0; j < 200*200; ++j) {
        int32_t value;
        std::memcpy(&value, masks + (size_t(i)*200*200+j)*sizeof(value), sizeof(value));
        if (value != 0 && value != 1) return false;
    }
    return true;
}

struct LayoutCandidate {
    std::string label;
    Box box;
    int rank;
    float detection_score;
    int class_id;
};

bool decode_layout(const TensorOutput& result, std::vector<LayoutCandidate>& candidates) {
    if (result.outputs.size() != 1) return false;
    const Tensor& tensor = result.outputs[0];
    if (tensor.name != "layout_candidates" || tensor.dtype != DataType::Float32 ||
        tensor.layout != TensorLayout::Matrix || tensor.shape.size() != 2 ||
        tensor.shape[0] < 0 || tensor.shape[0] > 10000 || tensor.shape[1] != 7 ||
        tensor.data.size() != size_t(tensor.shape[0]) * 7 * sizeof(float)) return false;
    const char* labels[] = {"text", "formula", "table", "image"};
    for (int64_t i = 0; i < tensor.shape[0]; ++i) {
        float row[7];
        std::memcpy(row, tensor.data.data() + size_t(i)*7*sizeof(float), sizeof(row));
        for (float v : row) if (!std::isfinite(v)) return false;
        if (row[0] < 0 || row[0] > 1000000 || std::trunc(row[0]) != row[0] ||
            row[1] < 0 || row[1] > 1 || std::trunc(row[6]) != row[6] ||
            row[6] < 0 || row[6] > 1000000) return false;
        for (int j = 2; j <= 5; ++j)
            if (row[j] < -1000000 || row[j] > 1000000 || std::trunc(row[j]) != row[j]) return false;
        int class_id = int(row[0]);
        candidates.push_back({class_id < 4 ? labels[class_id] : "unknown",
            {int(row[2]), int(row[3]), int(row[4]), int(row[5])}, int(row[6]), row[1], class_id});
    }
    return true;
}

std::string render(const Block& b) {
    if (b.status != "ok")
        return "[" + std::string(b.status == "skipped" ? "未处理：" : "识别失败：") +
               b.id + "](" + b.resource + ")";
    if (b.type == "image") return "![插图](" + b.resource + ")";
    if (b.type == "formula") return "$$\n" + b.text + "\n$$";
    return b.text;
}

std::string serialize(const Image& image, const std::string& state,
                      const std::vector<Block>& blocks, const std::string& profile,
                      const std::vector<RawLayoutCandidate>* raw = nullptr,
                      const std::string& overlay = {}) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    out << "{\"schema_version\":\"1.0\",\"document_id\":" << json_quote(document_id(image))
        << ",\"status\":" << json_quote(state)
        << ",\"source\":{\"type\":\"image\"},\"pages\":[{\"page_id\":\"p0001\",\"page_index\":0,"
        << "\"raster_size\":[" << image.width << ',' << image.height << "],\"coordinate_space\":\"raster_page\","
        << "\"status\":" << json_quote(state) << ",\"reading_order\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        out << json_quote(blocks[i].id);
    }
    out << "],\"layout_blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << json_quote(b.layout_id) << ",\"page_id\":\"p0001\",\"label\":" << json_quote(b.type)
            << ",\"bbox\":" << box_json(b.box) << ",\"coordinate_space\":\"raster_page\","
            << "\"detection_score\":" << b.detection_score << ",\"candidate_rank\":" << b.candidate_rank
            << ",\"original_class_id\":" << b.original_class_id
            << (b.candidate_id < 0 ? "" : ",\"candidate_id\":" + std::to_string(b.candidate_id) +
                ",\"mask_row\":" + std::to_string(b.mask_row) +
                ",\"mask_nonzero\":" + std::to_string(b.mask_nonzero) +
                ",\"model_label\":" + json_quote(b.model_label) +
                ",\"original_bbox\":[" + std::to_string(b.raw_box[0]) + "," +
                std::to_string(b.raw_box[1]) + "," + std::to_string(b.raw_box[2]) + "," +
                std::to_string(b.raw_box[3]) + "]" +
                ",\"clamped\":" + (b.clamped ? "true" : "false"))
            << ",\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":\"layout-p0001\"}}";
    }
    out << "],\"regions\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << json_quote(b.region_id) << ",\"page_id\":\"p0001\",\"source_layout_block_ids\":["
            << json_quote(b.layout_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\"}";
    }
    out << "],\"blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << json_quote(b.id) << ",\"page_id\":\"p0001\",\"type\":" << json_quote(b.type)
            << ",\"source_region_ids\":[" << json_quote(b.region_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\",\"geometry_granularity\":\"region\","
            << "\"reading_order_source\":\"geometry\","
            << "\"status\":" << json_quote(b.status) << ",\"confidence\":null,\"content\":{\"format\":"
            << json_quote(b.type == "image" || b.type == "unknown" ? "resource" :
                     b.type == "formula" ? "latex" : b.type == "table" ? "html" : "markdown")
            << ",\"text\":" << json_quote(b.text) << ",\"resource\":"
            << (b.resource.empty() ? "null" : json_quote(b.resource));
        if (b.type == "formula") out << ",\"display\":true";
        out
            << "},\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("req" + id('r', i+1))
            << ",\"raw_output\":" << (b.raw_base64.empty() ? json_quote(b.raw) : "null")
            << ",\"raw_output_base64\":" << (b.raw_base64.empty() ? "null" : json_quote(b.raw_base64))
            << ",\"text_base64\":" << (b.text_base64.empty() ? "null" : json_quote(b.text_base64))
            << "},\"error\":" << (b.error.empty() ? "null" : json_quote(b.error))
            << ",\"error_base64\":" << (b.error_base64.empty() ? "null" : json_quote(b.error_base64)) << "}";
    }
    out << "],\"relations\":[]}],\"resources\":[";
    bool first = true;
    for (const Block& b : blocks) if (!b.resource.empty()) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << json_quote("asset-" + b.id) << ",\"path\":" << json_quote(b.resource)
            << ",\"media_type\":\"image/png\",\"source_block_id\":" << json_quote(b.id)
            << ",\"width\":" << b.box.x1-b.box.x0 << ",\"height\":" << b.box.y1-b.box.y0
            << ",\"bbox\":" << box_json(b.box) << ",\"coordinate_space\":\"raster_page\"}";
    }
    out << ']';
    if (raw) {
        out << ",\"layout_diagnostics\":{\"score_threshold\":0.5,\"candidate_count\":" << raw->size()
            << ",\"overlay_asset\":" << json_quote(overlay)
            << ",\"raw_tensor_assets\":{\"image\":\"assets/p0001-image.f32\","
               "\"im_shape\":\"assets/p0001-im_shape.f32\","
               "\"scale_factor\":\"assets/p0001-scale_factor.f32\","
               "\"fetch_name_0\":\"assets/p0001-fetch_name_0.f32\","
               "\"fetch_name_1\":\"assets/p0001-fetch_name_1.i32\","
               "\"fetch_name_2\":\"assets/p0001-fetch_name_2.rle\"},\"candidates\":[";
        for (size_t i = 0; i < raw->size(); ++i) {
            const auto& c = (*raw)[i];
            if (i) out << ',';
            out << "{\"candidate_id\":" << c.id << ",\"mask_row\":" << c.id
                << ",\"class_id\":" << c.class_id << ",\"model_label\":" << json_quote(c.label)
                << ",\"score\":" << c.score << ",\"original_bbox\":["
                << c.box[0] << ',' << c.box[1] << ',' << c.box[2] << ',' << c.box[3]
                << "],\"rank\":" << c.rank << ",\"mask_nonzero\":" << c.mask_nonzero
                << ",\"selected\":" << (c.selected ? "true" : "false")
                << ",\"filter_reason\":" << (c.reason.empty() ? "null" : json_quote(c.reason))
                << ",\"handling_reason\":" << (c.selected && c.label == "unknown" ? "\"unknown_class\"" : "null")
                << ",\"mask_asset\":" << (c.mask_asset.empty() ? "null" : json_quote(c.mask_asset))
                << ",\"clamped\":" << (c.clamped ? "true" : "false")
                << ",\"crop_bbox\":" << (c.selected ? box_json(c.crop) : "null") << '}';
        }
        out << "]}";
    }
    out << '}';
    return out.str();
}
} // namespace

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

Tensor geometry_tensor(const std::string& name, float first, float second) {
    Tensor tensor{name, DataType::Float32, TensorLayout::Matrix, {1,2},
                  std::vector<uint8_t>(2*sizeof(float))};
    float values[] = {first, second};
    std::memcpy(tensor.data.data(), values, sizeof(values));
    return tensor;
}

std::vector<uint8_t> layout_page_mask(const Image& image, const RawLayoutCandidate& c,
                                      const uint8_t* masks);

std::vector<uint8_t> layout_overlay(const Image& image,
                                    const std::vector<RawLayoutCandidate>& records,
                                    const uint8_t* masks) {
    Image overlay = image;
    for (const auto& c : records) {
        if (!c.selected) continue;
        auto page_mask = layout_page_mask(image, c, masks);
        for (int y = c.crop.y0; y < c.crop.y1; ++y) for (int x = c.crop.x0; x < c.crop.x1; ++x) {
            if (page_mask[size_t(y)*image.width+x]) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*image.width+x)*3;
                pixel[0] = uint8_t(pixel[0]/2);
                pixel[1] = uint8_t(pixel[1]/2 + 127);
                pixel[2] = uint8_t(pixel[2]/2);
            }
        }
        for (int x = c.crop.x0; x < c.crop.x1; ++x) {
            for (int y : {c.crop.y0, c.crop.y1-1}) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*image.width+x)*3;
                pixel[0] = 255; pixel[1] = 0; pixel[2] = 0;
            }
        }
        for (int y = c.crop.y0; y < c.crop.y1; ++y) {
            for (int x : {c.crop.x0, c.crop.x1-1}) {
                uint8_t* pixel = overlay.rgb.data() + (size_t(y)*image.width+x)*3;
                pixel[0] = 255; pixel[1] = 0; pixel[2] = 0;
            }
        }
    }
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(png_write, &png, image.width, image.height, 3,
                                overlay.rgb.data(), image.width*3))
        throw std::runtime_error("layout overlay PNG encoding failed");
    return png;
}

std::vector<uint8_t> layout_page_mask(const Image& image, const RawLayoutCandidate& c,
                                      const uint8_t* masks) {
    std::vector<uint8_t> pixels(size_t(image.width)*image.height);
    int x0 = int(c.box[0]), y0 = int(c.box[1]);
    int x1 = int(c.box[2]), y1 = int(c.box[3]);
    if (x1 <= x0 || y1 <= y0) return pixels;
    int mx0 = std::clamp(int(std::nearbyint(double(x0)*200/image.width)), 0, 200);
    int my0 = std::clamp(int(std::nearbyint(double(y0)*200/image.height)), 0, 200);
    int mx1 = std::clamp(int(std::nearbyint(double(x1)*200/image.width)), 0, 200);
    int my1 = std::clamp(int(std::nearbyint(double(y1)*200/image.height)), 0, 200);
    if (mx1 <= mx0 || my1 <= my0) return pixels;
    const uint8_t* mask = masks + size_t(c.id)*200*200*sizeof(int32_t);
    for (int y = std::max(0,y0); y < std::min(image.height,y1); ++y)
        for (int x = std::max(0,x0); x < std::min(image.width,x1); ++x) {
        int mx = std::clamp(mx0 + int((int64_t(x-x0)*(mx1-mx0))/(x1-x0)), mx0, mx1-1);
        int my = std::clamp(my0 + int((int64_t(y-y0)*(my1-my0))/(y1-y0)), my0, my1-1);
        int32_t bit;
        std::memcpy(&bit, mask + (size_t(my)*200+mx)*sizeof(bit), sizeof(bit));
        pixels[size_t(y)*image.width+x] = bit ? 255 : 0;
    }
    return pixels;
}

RunResult run_layout_only(IInferenceEngine* backend, const Image& image, std::atomic_bool& cancelled,
                          const ExecutionPlan* plan, RunResult audit) {
    using Clock = std::chrono::steady_clock;
    if (!backend->capabilities().tensor || backend->capabilities().max_concurrent_requests != 1)
        return {RunCode::Unsupported, {}};
    if (cancelled) return {RunCode::Cancelled, {}};
    auto start = Clock::now();
    TensorRequest request;
    request.inputs.push_back(layout_image_tensor(image));
    request.inputs.push_back(geometry_tensor("im_shape", 800, 800));
    request.inputs.push_back(geometry_tensor("scale_factor", 800.0f/image.height, 800.0f/image.width));
    request.requested_outputs = {"fetch_name_0", "fetch_name_1", "fetch_name_2"};
    audit.layout_attempted = true;
    audit.output.assets.push_back({"assets/p0001-image.f32", request.inputs[0].data});
    audit.output.assets.push_back({"assets/p0001-im_shape.f32", request.inputs[1].data});
    audit.output.assets.push_back({"assets/p0001-scale_factor.f32", request.inputs[2].data});
    audit.did_normalize = true;
    ExecutionContext context{cancelled};
    InferenceResponse response;
    try { response = backend->execute({"layout-p0001", std::move(request)}, context); }
    catch (const std::exception& e) {
        audit.code = RunCode::Failed;
        audit.error_code = "layout_inference_failed";
        audit.error_message = "版面推理异常：" + std::string(e.what());
        audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
        return audit;
    }
    audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
    if (cancelled) return {RunCode::Cancelled, {}};
    auto* output = std::get_if<TensorOutput>(&response.payload);
    std::vector<RawLayoutCandidate> records;
    const uint8_t* masks = nullptr;
    if (!output || !decode_real_layout(*output, image, records, masks)) {
        audit.code = RunCode::Failed;
        audit.error_code = "layout_output_contract_mismatch";
        audit.error_message = "PP-DocLayoutV3 候选/数量/mask 张量契约不符";
        return audit;
    }
    audit.did_layout = true;
    for (const auto& tensor : output->outputs) {
        if (tensor.name == "fetch_name_0")
            audit.output.assets.push_back({"assets/p0001-fetch_name_0.f32", tensor.data});
        else if (tensor.name == "fetch_name_1")
            audit.output.assets.push_back({"assets/p0001-fetch_name_1.i32", tensor.data});
    }
    audit.output.assets.push_back({"assets/p0001-fetch_name_2.rle", encode_masks_rle(masks)});
    std::vector<const RawLayoutCandidate*> selected;
    for (const auto& candidate : records) if (candidate.selected) selected.push_back(&candidate);
    std::stable_sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
        if (a->crop.y0 != b->crop.y0) return a->crop.y0 < b->crop.y0;
        return a->crop.x0 < b->crop.x0;
    });
    std::vector<Block> blocks;
    JobOutput result;
    result.assets = std::move(audit.output.assets);
    for (const auto* candidate : selected) {
        if (cancelled) return {RunCode::Cancelled, {}};
        Block block;
        size_t n = blocks.size()+1;
        block.id = id('b', n); block.layout_id = id('l', n); block.region_id = id('r', n);
        block.type = canonical_label(candidate->class_id);
        block.model_label = candidate->label;
        block.box = candidate->crop;
        block.candidate_id = block.mask_row = candidate->id;
        block.mask_nonzero = candidate->mask_nonzero;
        block.detection_score = candidate->score;
        block.candidate_rank = candidate->rank;
        block.original_class_id = candidate->class_id;
        block.clamped = candidate->clamped;
        std::copy(std::begin(candidate->box), std::end(candidate->box), block.raw_box);
        block.status = "skipped";
        block.error = block.type == "unknown" ? "unknown_layout_class" : "recognition_not_executed";
        block.resource = "assets/p0001-" + block.id + ".png";
        result.assets.push_back({block.resource, crop_png(image, block.box)});
        auto& mutable_candidate = records[size_t(candidate->id)];
        mutable_candidate.mask_asset = "assets/p0001-mask-c" + std::to_string(candidate->id) + ".png";
        std::vector<uint8_t> mask_pixels = layout_page_mask(image, *candidate, masks);
        std::vector<uint8_t> mask_png;
        if (!stbi_write_png_to_func(png_write, &mask_png, image.width, image.height, 1,
                                    mask_pixels.data(), image.width))
            throw std::runtime_error("layout mask PNG encoding failed");
        result.assets.push_back({mutable_candidate.mask_asset, std::move(mask_png)});
        audit.did_crop = true;
        blocks.push_back(std::move(block));
    }
    const std::string overlay_name = "assets/p0001-layout-overlay.png";
    result.assets.push_back({overlay_name, layout_overlay(image, records, masks)});
    const std::string state = records.empty() ? "blank" : "partial";
    start = Clock::now();
    for (const auto& block : blocks) {
        if (!result.markdown.empty()) result.markdown += "\n\n";
        result.markdown += render(block);
    }
    if (!result.markdown.empty()) result.markdown += '\n';
    result.json = serialize(image, state, blocks, backend->profile(), &records, overlay_name);
    audit.did_export = true;
    audit.export_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
    uint64_t bytes = result.json.size() + result.markdown.size();
    for (const auto& asset : result.assets) bytes += asset.png.size();
    if (plan && bytes > plan->max_output_bytes) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "output_bytes";
        return audit;
    }
    audit.code = records.empty() ? RunCode::Blank : RunCode::Partial;
    audit.output = std::move(result);
    return audit;
}
} // namespace

RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled,
                   const ExecutionPlan* plan) {
    using Clock = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point from) {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count());
    };
    RunResult audit{RunCode::Failed, {}};
    auto start = Clock::now();
    Image image;
    if (!decode(input, image)) return {RunCode::InputError, {}};
    audit.did_decode = true;
    audit.page_pixels = uint64_t(image.width) * image.height;
    audit.decode_ms = elapsed(start);
    if (plan && audit.page_pixels > plan->max_page_pixels) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "input_pixels";
        return audit;
    }
    if (!backend) return {RunCode::Unsupported, {}};
    if (plan && plan->layout_only) return run_layout_only(backend, image, cancelled, plan, audit);
    if (!backend->capabilities().tensor || !backend->capabilities().generation ||
        backend->capabilities().max_concurrent_requests < 1) return {RunCode::Unsupported, {}};
    if (cancelled) return {RunCode::Cancelled, {}};
    ExecutionContext context{cancelled};
    Tensor page_tensor{"page_rgb", DataType::UInt8, TensorLayout::HWC,
                       {image.height, image.width, 3}, image.rgb};
    if (plan && std::any_of(plan->processing.begin(), plan->processing.end(),
                            [](const ProcessingStep& step) { return step.id == "normalize" && step.owner == "adapter"; })) {
        page_tensor.name = "page_rgb_normalized";
        page_tensor.dtype = DataType::Float32;
        page_tensor.data.resize(image.rgb.size() * sizeof(float));
        for (size_t i = 0; i < image.rgb.size(); ++i) {
            float value = float(image.rgb[i]) / 255.0f;
            std::memcpy(page_tensor.data.data() + i*sizeof(float), &value, sizeof(float));
        }
        audit.did_normalize = true;
    }
    start = Clock::now();
    auto response = backend->execute({"layout-p0001", TensorRequest{{std::move(page_tensor)}, {"layout_candidates"}}}, context);
    audit.layout_ms = elapsed(start);
    if (cancelled) return {RunCode::Cancelled, {}};
    auto* layout = std::get_if<TensorOutput>(&response.payload);
    if (!layout) return {RunCode::Failed, {}};
    audit.did_layout = true;
    std::vector<LayoutCandidate> candidates;
    if (!decode_layout(*layout, candidates)) return {RunCode::Failed, {}};
    start = Clock::now();
    for (const auto& candidate : candidates) {
        audit.did_recognition = true;
        Box b = candidate.box;
        if (b.x0 < 0 || b.y0 < 0 || b.x1 > image.width || b.y1 > image.height ||
            b.x0 >= b.x1 || b.y0 >= b.y1) return {RunCode::Failed, {}};
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.box.y0 != b.box.y0) return a.box.y0 < b.box.y0;
        if (a.box.x0 != b.box.x0) return a.box.x0 < b.box.x0;
        if (a.box.y1 != b.box.y1) return a.box.y1 < b.box.y1;
        if (a.box.x1 != b.box.x1) return a.box.x1 < b.box.x1;
        return a.label < b.label;
    });
    std::vector<Block> blocks;
    JobOutput output;
    bool partial = false;
    bool recognition_unavailable = false;
    for (const auto& candidate : candidates) {
        if (cancelled) return {RunCode::Cancelled, {}};
        size_t n = blocks.size() + 1;
        Block block;
        block.id = id('b', n); block.layout_id = id('l', n); block.region_id = id('r', n);
        block.type = candidate.label; block.box = candidate.box;
        block.detection_score = candidate.detection_score;
        block.candidate_rank = candidate.rank;
        block.original_class_id = candidate.class_id;
        block.status = "ok";
        if (block.type == "image" || block.type == "chart") {
            block.type = "image";
            block.resource = "assets/p0001-" + block.id + ".png";
            audit.did_crop = true;
            output.assets.push_back({block.resource, crop_png(image, block.box)});
        } else if (block.type == "text" || block.type == "formula" || block.type == "table") {
            Image crop = crop_rgb(image, block.box);
            audit.did_crop = true;
            GenerationOutput generation;
            std::string local_error;
            bool skipped_after_backend_failure = recognition_unavailable;
            bool reset_completed = false;
            try {
                if (recognition_unavailable) local_error = "region backend unavailable after prior failure";
                else if (!backend->reset()) {
                    local_error = "region reset failed";
                    audit.reset_failed = true;
                    recognition_unavailable = true;
                }
                else {
                    reset_completed = true;
                    audit.did_reset = true;
                    auto recognized = backend->execute({"req" + block.region_id,
                        GenerationRequest{std::move(crop), block.box, block.type, "req" + block.region_id,
                                          plan ? plan->max_new_tokens : 4096}}, context);
                    if (auto* value = std::get_if<GenerationOutput>(&recognized.payload)) generation = std::move(*value);
                    else {
                        local_error = "generation response type mismatch";
                        recognition_unavailable = true;
                    }
                }
            } catch (const std::bad_alloc&) { throw; }
              catch (...) {
                  local_error = "region inference exception";
                  if (!reset_completed) audit.reset_failed = true;
                  recognition_unavailable = true;
              }
            if (cancelled) return {RunCode::Cancelled, {}};
            bool text_valid = valid_utf8(generation.text);
            bool raw_valid = valid_utf8(generation.raw_output);
            bool error_valid = valid_utf8(generation.error);
            if (raw_valid) block.raw = generation.raw_output;
            else block.raw_base64 = base64(generation.raw_output);
            if (!text_valid) block.text_base64 = base64(generation.text);
            if (!error_valid) block.error_base64 = base64(generation.error);
            if (!local_error.empty() || generation.finish_reason != "complete" ||
                !text_valid || !raw_valid || !error_valid) {
                block.status = skipped_after_backend_failure ? "skipped" :
                               generation.finish_reason == "truncated" ? "partial" : "failed";
                block.error = !local_error.empty() ? local_error :
                              !error_valid ? "invalid backend error encoding" :
                              (!text_valid || !raw_valid) ? "invalid backend UTF-8" :
                              generation.error.empty() ? "recognition incomplete" : generation.error;
                block.resource = "assets/p0001-" + block.id + ".png";
                audit.did_crop = true;
                output.assets.push_back({block.resource, crop_png(image, block.box)});
                partial = true;
            } else block.text = generation.text;
        } else {
            block.status = "skipped";
            block.error = "unsupported layout label";
            block.resource = "assets/p0001-" + block.id + ".png";
            audit.did_crop = true;
            output.assets.push_back({block.resource, crop_png(image, block.box)});
            partial = true;
        }
        blocks.push_back(std::move(block));
    }
    audit.recognition_ms = elapsed(start);
    start = Clock::now();
    std::string state = candidates.empty() ? "blank" : partial ? "partial" : "ok";
    for (const auto& block : blocks) {
        if (!output.markdown.empty()) output.markdown += "\n\n";
        output.markdown += render(block);
    }
    if (!output.markdown.empty()) output.markdown += '\n';
    output.json = serialize(image, state, blocks, backend->profile());
    audit.did_export = true;
    audit.export_ms = elapsed(start);
    if (plan) {
        uint64_t output_size = output.json.size() + output.markdown.size();
        for (const auto& asset : output.assets) output_size += asset.png.size();
        if (output_size > plan->max_output_bytes) {
            audit.code = RunCode::BudgetExceeded;
            audit.budget_stage = "output_bytes";
            return audit;
        }
    }
    audit.code = candidates.empty() ? RunCode::Blank : partial ? RunCode::Partial : RunCode::Ok;
    audit.output = std::move(output);
    return audit;
}
} // namespace dococr
