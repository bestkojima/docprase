#include "dococr/inference.hpp"
#include "dococr/dococr.h"
#include <algorithm>
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

std::string quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
            else out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

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
};

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
                      const std::vector<Block>& blocks, const std::string& profile) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"schema_version\":\"1.0\",\"document_id\":" << quote(document_id(image))
        << ",\"status\":" << quote(state)
        << ",\"source\":{\"type\":\"image\"},\"pages\":[{\"page_id\":\"p0001\",\"page_index\":0,"
        << "\"raster_size\":[" << image.width << ',' << image.height << "],\"coordinate_space\":\"raster_page\","
        << "\"status\":" << quote(state) << ",\"reading_order\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        out << quote(blocks[i].id);
    }
    out << "],\"layout_blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << quote(b.layout_id) << ",\"page_id\":\"p0001\",\"label\":" << quote(b.type)
            << ",\"bbox\":" << box_json(b.box) << ",\"coordinate_space\":\"raster_page\","
            << "\"detection_score\":" << b.detection_score << ",\"candidate_rank\":" << b.candidate_rank
            << ",\"original_class_id\":" << b.original_class_id
            << ",\"provenance\":{\"model_profile\":" << quote(profile)
            << ",\"request_id\":\"layout-p0001\"}}";
    }
    out << "],\"regions\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << quote(b.region_id) << ",\"page_id\":\"p0001\",\"source_layout_block_ids\":["
            << quote(b.layout_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\"}";
    }
    out << "],\"blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << quote(b.id) << ",\"page_id\":\"p0001\",\"type\":" << quote(b.type)
            << ",\"source_region_ids\":[" << quote(b.region_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\",\"geometry_granularity\":\"region\","
            << "\"reading_order_source\":\"geometry\","
            << "\"status\":" << quote(b.status) << ",\"confidence\":null,\"content\":{\"format\":"
            << quote(b.type == "image" || b.type == "unknown" ? "resource" :
                     b.type == "formula" ? "latex" : b.type == "table" ? "html" : "markdown")
            << ",\"text\":" << quote(b.text) << ",\"resource\":"
            << (b.resource.empty() ? "null" : quote(b.resource));
        if (b.type == "formula") out << ",\"display\":true";
        out
            << "},\"provenance\":{\"model_profile\":" << quote(profile)
            << ",\"request_id\":" << quote("req" + id('r', i+1))
            << ",\"raw_output\":" << (b.raw_base64.empty() ? quote(b.raw) : "null")
            << ",\"raw_output_base64\":" << (b.raw_base64.empty() ? "null" : quote(b.raw_base64))
            << ",\"text_base64\":" << (b.text_base64.empty() ? "null" : quote(b.text_base64))
            << "},\"error\":" << (b.error.empty() ? "null" : quote(b.error))
            << ",\"error_base64\":" << (b.error_base64.empty() ? "null" : quote(b.error_base64)) << "}";
    }
    out << "],\"relations\":[]}],\"resources\":[";
    bool first = true;
    for (const Block& b : blocks) if (!b.resource.empty()) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << quote("asset-" + b.id) << ",\"path\":" << quote(b.resource)
            << ",\"media_type\":\"image/png\",\"source_block_id\":" << quote(b.id)
            << ",\"width\":" << b.box.x1-b.box.x0 << ",\"height\":" << b.box.y1-b.box.y0
            << ",\"bbox\":" << box_json(b.box) << ",\"coordinate_space\":\"raster_page\"}";
    }
    out << "]}";
    return out.str();
}
} // namespace

RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled) {
    Image image;
    if (!decode(input, image)) return {RunCode::InputError, {}};
    if (!backend) return {RunCode::Unsupported, {}};
    if (!backend->capabilities().tensor || !backend->capabilities().generation ||
        backend->capabilities().max_concurrent_requests < 1) return {RunCode::Unsupported, {}};
    if (cancelled) return {RunCode::Cancelled, {}};
    ExecutionContext context{cancelled};
    Tensor page_tensor{"page_rgb", DataType::UInt8, TensorLayout::HWC,
                       {image.height, image.width, 3}, image.rgb};
    auto response = backend->execute({"layout-p0001", TensorRequest{{std::move(page_tensor)}, {"layout_candidates"}}}, context);
    if (cancelled) return {RunCode::Cancelled, {}};
    auto* layout = std::get_if<TensorOutput>(&response.payload);
    if (!layout) return {RunCode::Failed, {}};
    std::vector<LayoutCandidate> candidates;
    if (!decode_layout(*layout, candidates)) return {RunCode::Failed, {}};
    for (const auto& candidate : candidates) {
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
            output.assets.push_back({block.resource, crop_png(image, block.box)});
        } else if (block.type == "text" || block.type == "formula" || block.type == "table") {
            Image crop = crop_rgb(image, block.box);
            GenerationOutput generation;
            std::string local_error;
            bool skipped_after_backend_failure = recognition_unavailable;
            try {
                if (recognition_unavailable) local_error = "region backend unavailable after prior failure";
                else if (!backend->reset()) {
                    local_error = "region reset failed";
                    recognition_unavailable = true;
                }
                else {
                    auto recognized = backend->execute({"req" + block.region_id,
                        GenerationRequest{std::move(crop), block.box, block.type, "req" + block.region_id}}, context);
                    if (auto* value = std::get_if<GenerationOutput>(&recognized.payload)) generation = std::move(*value);
                    else {
                        local_error = "generation response type mismatch";
                        recognition_unavailable = true;
                    }
                }
            } catch (const std::bad_alloc&) { throw; }
              catch (...) {
                  local_error = "region inference exception";
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
                output.assets.push_back({block.resource, crop_png(image, block.box)});
                partial = true;
            } else block.text = generation.text;
        } else {
            block.status = "skipped";
            block.error = "unsupported layout label";
            block.resource = "assets/p0001-" + block.id + ".png";
            output.assets.push_back({block.resource, crop_png(image, block.box)});
            partial = true;
        }
        blocks.push_back(std::move(block));
    }
    std::string state = candidates.empty() ? "blank" : partial ? "partial" : "ok";
    for (const auto& block : blocks) {
        if (!output.markdown.empty()) output.markdown += "\n\n";
        output.markdown += render(block);
    }
    if (!output.markdown.empty()) output.markdown += '\n';
    output.json = serialize(image, state, blocks, backend->profile());
    return {candidates.empty() ? RunCode::Blank : partial ? RunCode::Partial : RunCode::Ok,
            std::move(output)};
}
} // namespace dococr
