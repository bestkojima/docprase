#include "dococr/inference.hpp"
#include "dococr/dococr.h"
#include "config.hpp"
#include "layout_preprocess.hpp"
#include "pdf_page_id.hpp"
#include "table_parser.hpp"
#include "markdown.hpp"
#include "output_assessment.hpp"
#include "region_recognition.hpp"
#include "region_structure.hpp"
#include "json.hpp"
#include "content_validation.hpp"
#include "formula_symbols.hpp"
#include "formula_scripts.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <set>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

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
thread_local uint32_t current_pdf_page = 0;
struct PageScope {
    uint32_t previous;
    explicit PageScope(uint32_t page) : previous(current_pdf_page) { current_pdf_page = page; }
    ~PageScope() { current_pdf_page = previous; }
};

std::string page_id() {
    return pdf_page_id(current_pdf_page ? current_pdf_page : 1);
}

std::string page_asset(const std::string& suffix) {
    return "assets/" + page_id() + "-" + suffix;
}
std::string block_asset(const std::string& block_id) {
    return current_pdf_page ? "assets/" + block_id + ".png" :
                              page_asset(block_id + ".png");
}

std::string id(const char prefix, size_t index) {
    std::ostringstream out;
    if (current_pdf_page) out << page_id() << '-';
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

bool decode(InputView in, Image& image, uint64_t pixel_limit = max_pixels) {
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
                error, error_base64, resource, format_override;
    Box box, layout_box;
    bool crop_expanded = false;
    float detection_score = 0;
    int candidate_rank = 0;
    int original_class_id = 0;
    int candidate_id = -1, mask_row = -1, mask_nonzero = 0;
    std::string model_label;
    float raw_box[4]{};
    bool clamped = false;
    bool display_formula = true;
    std::vector<std::string> owned_layout_ids;
    ParsedTable table;
    GenerationOutput visual;
    OutputAssessment assessment;
    RegionRecognition recognition;
};

std::string recognition_json(const RegionRecognition& recognition) {
    using Json = nlohmann::json;
    auto transform_json = [](const VisualTransform& t) {
        return Json{{"canvas_size", {t.canvas_width, t.canvas_height}},
            {"content_size", {t.content_width, t.content_height}}, {"pad_offset", {t.pad_x, t.pad_y}},
            {"scale", t.scale}, {"rounding_error", {t.rounding_error_x, t.rounding_error_y}}};
    };
    Json attempts = Json::array();
    uint64_t budget = 0, generation_ms = 0;
    for (const auto& attempt : recognition.attempts) {
        const auto& output = attempt.output;
        auto visual = transform_json(output.visual_transform);
        visual["evidence"] = output.visual_evidence;
        visual["token_count"] = output.visual_tokens;
        Json saved = {{"finish_reason", output.finish_reason}, {"stop_reason", output.stop_reason},
            {"generation_elapsed_ms", output.generation_elapsed_ms}};
        for (const auto& value : {std::make_pair("text", output.text),
             std::make_pair("raw_output", output.raw_output), std::make_pair("error", output.error)}) {
            saved[value.first] = valid_utf8(value.second) ? Json(value.second) : Json(nullptr);
            saved[std::string(value.first) + "_base64"] = valid_utf8(value.second) ? Json(nullptr) : Json(base64(value.second));
        }
        attempts.push_back({{"index", attempts.size() + 1}, {"correction", attempt.correction},
            {"config", {{"max_new_tokens", attempt.max_new_tokens},
                        {"generation_timeout_ms", attempt.generation_timeout_ms},
                        {"visual_min_pixels", attempt.visual_min_pixels},
                        {"visual_max_pixels", attempt.visual_max_pixels}}},
            {"elapsed_ms", attempt.elapsed_ms}, {"output", saved},
            {"visual", visual},
            {"input_visual", attempt.input_visual ? transform_json(*attempt.input_visual) : Json(nullptr)}});
        budget += attempt.generation_timeout_ms;
        generation_ms += output.generation_elapsed_ms;
    }
    return Json{{"policy", "targeted-retry-v1"}, {"max_attempts", 2},
        {"max_total_generation_budget_ms", 240000}, {"configured_generation_budget_ms", budget},
        {"generation_elapsed_ms", generation_ms},
        {"selected_attempt", attempts.empty() ? Json(nullptr) : Json(attempts.size())},
        {"attempts", attempts}}.dump();
}

struct ReadingOrderEvidence {
    std::string source = "geometry";
    std::string reason = "page_geometry";
};

struct SemanticRelation {
    std::string type, source_block_id, target_block_id, evidence;
};

const Box& layout_geometry(const Block& block) {
    return block.crop_expanded ? block.layout_box : block.box;
}

bool spans_columns(const Block& block, int page_width) {
    const bool heading = block.model_label == "doc_title" ||
        block.model_label == "paragraph_title";
    const bool page_header = block.model_label == "header";
    return layout_geometry(block).x0 < page_width * 0.45 && layout_geometry(block).x1 > page_width * 0.55 &&
           (heading || page_header || layout_geometry(block).x1 - layout_geometry(block).x0 >= page_width * 0.65);
}

// Assign order to already identified blocks. Detection and recognition IDs never depend on this pass.
ReadingOrderEvidence arrange_reading_order(std::vector<Block>& blocks, int page_width) {
    if (blocks.empty()) return {"geometry", "empty_page"};
    const size_t size = blocks.size();
    int left_edge = page_width;
    std::vector<size_t> right_body;
    for (size_t i = 0; i < size; ++i) {
        const Block& block = blocks[i];
        if (layout_geometry(block).x0 < page_width * 0.45)
            left_edge = std::min(left_edge, layout_geometry(block).x0);
        if (layout_geometry(block).x0 >= page_width * 0.50 && block.model_label != "header" &&
            block.model_label != "footer" && block.model_label != "footnote" &&
            block.model_label != "vision_footnote") right_body.push_back(i);
    }
    auto section_title = [&](const Block& title) {
        if (right_body.empty() ||
            (title.model_label != "paragraph_title" && title.model_label != "doc_title") ||
            layout_geometry(title).x0 > left_edge + std::max(8, int(page_width * 0.03))) return false;
        const int height = layout_geometry(title).y1 - layout_geometry(title).y0;
        // A left-aligned section heading can be shorter than the column gap.
        // Treat it as a page-wide break only when the right column is clear
        // for at least its own height above and below the heading.
        for (size_t i : right_body)
            if (layout_geometry(blocks[i]).y1 >= layout_geometry(title).y0 - height &&
                layout_geometry(blocks[i]).y0 <= layout_geometry(title).y1 + height) return false;
        return true;
    };
    std::vector<size_t> spans;
    for (size_t i = 0; i < size; ++i)
        if (page_width >= 64 &&
            (spans_columns(blocks[i], page_width) || section_title(blocks[i]))) spans.push_back(i);
    std::stable_sort(spans.begin(), spans.end(), [&](size_t a, size_t b) {
        return layout_geometry(blocks[a]).y0 < layout_geometry(blocks[b]).y0;
    });
    std::vector<int> segment(size), column(size, 0);
    std::vector<bool> is_span(size, false);
    for (size_t i : spans) is_span[i] = true;
    for (size_t i = 0; i < size; ++i) {
        int before = 0;
        for (size_t span : spans) if (layout_geometry(blocks[span]).y0 < layout_geometry(blocks[i]).y0) ++before;
        segment[i] = 2 * before + (is_span[i] ? 1 : 0);
    }
    for (int band = 0; band <= 2 * int(spans.size()); band += 2) {
        if (page_width < 64) continue;
        bool left = false, right = false;
        for (size_t i = 0; i < size; ++i) if (segment[i] == band) {
            const auto purpose = layout_region_policy(blocks[i].original_class_id).purpose;
            // Marginal labels cannot establish a second prose column.
            if (purpose == LayoutPurpose::PageMarker || purpose == LayoutPurpose::Number ||
                purpose == LayoutPurpose::Footnote || purpose == LayoutPurpose::Aside) continue;
            if (layout_geometry(blocks[i]).x1 <= page_width * 0.50) left = true;
            else if (layout_geometry(blocks[i]).x0 >= page_width * 0.50) right = true;
        }
        if (!left || !right) continue;
        for (size_t i = 0; i < size; ++i) if (segment[i] == band)
            column[i] = layout_geometry(blocks[i]).x1 <= page_width * 0.50 ? 0 :
                layout_geometry(blocks[i]).x0 >= page_width * 0.50 ? 1 : 2;
    }
    std::vector<size_t> geometry(size);
    std::iota(geometry.begin(), geometry.end(), 0);
    // 图片及短图注的一像素top抖动不能改变同一行的左右顺序。
    // 固定行锚点先生成整数排序键；不用带容差的比较器，也不链式合并行。
    std::vector<int> row_top(size);
    for (size_t i = 0; i < size; ++i) row_top[i] = layout_geometry(blocks[i]).y0;
    std::stable_sort(geometry.begin(), geometry.end(), [&](size_t a, size_t b) {
        return row_top[a] < row_top[b];
    });
    std::vector<size_t> anchors;
    for (size_t i : geometry) {
        const Block& block = blocks[i];
        if (block.type != "image" && block.model_label != "figure_title" &&
            block.model_label != "vision_footnote" && block.model_label != "formula_number" &&
            block.model_label != "number") continue;
        const Box& bounds = layout_geometry(block);
        auto anchor = std::find_if(anchors.rbegin(), anchors.rend(), [&](size_t a) {
            const Box& other = layout_geometry(blocks[a]);
            return segment[a] == segment[i] && column[a] == column[i] && blocks[a].type == block.type &&
                bounds.y0 - other.y0 <= std::max(2, std::min(bounds.y1-bounds.y0, other.y1-other.y0)/10);
        });
        if (anchor == anchors.rend()) anchors.push_back(i);
        else row_top[i] = row_top[*anchor];
    }
    std::stable_sort(geometry.begin(), geometry.end(), [&](size_t a, size_t b) {
        if (segment[a] != segment[b]) return segment[a] < segment[b];
        if (column[a] != column[b]) return column[a] < column[b];
        if (row_top[a] != row_top[b]) return row_top[a] < row_top[b];
        if (layout_geometry(blocks[a]).x0 != layout_geometry(blocks[b]).x0) return layout_geometry(blocks[a]).x0 < layout_geometry(blocks[b]).x0;
        return a < b;
    });
    ReadingOrderEvidence evidence;
    std::set<int> ranks;
    bool missing = false, duplicate = false;
    for (const Block& block : blocks) {
        if (block.candidate_rank < 0) missing = true;
        else if (!ranks.insert(block.candidate_rank).second) duplicate = true;
    }
    std::vector<size_t> chosen = geometry;
    if (missing) evidence.reason = "missing_rank";
    else if (duplicate) evidence.reason = "duplicate_rank";
    else {
        std::vector<size_t> ranked(size);
        std::iota(ranked.begin(), ranked.end(), 0);
        std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
            return blocks[a].candidate_rank < blocks[b].candidate_rank;
        });
        bool conflict = false, column_conflict = false;
        for (size_t i = 1; i < size; ++i) {
            size_t previous = ranked[i-1], current = ranked[i];
            if (segment[previous] > segment[current]) conflict = true;
            if (segment[previous] == segment[current] && column[previous] > column[current])
                column_conflict = true;
            if (segment[previous] == segment[current] && column[previous] == column[current] &&
                layout_geometry(blocks[previous]).y0 > layout_geometry(blocks[current]).y0 &&
                layout_geometry(blocks[previous]).y0 >= layout_geometry(blocks[current]).y1)
                conflict = true;
        }
        if (column_conflict) evidence.reason = "model_column_conflict";
        else if (conflict) evidence.reason = "model_geometry_conflict";
        else { evidence = {"model", "unique_rank"}; chosen = std::move(ranked); }
    }
    std::vector<Block> reordered;
    reordered.reserve(size);
    for (size_t i : chosen) reordered.push_back(std::move(blocks[i]));
    blocks = std::move(reordered);
    return evidence;
}

RegionStructure arrange_structure(std::vector<Block>& blocks, int width, int height,
                                  ReadingOrderEvidence& evidence) {
    std::vector<StructureItem> items;
    std::map<std::string, size_t> by_id;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        by_id[b.id] = i;
        items.push_back({b.id, b.region_id, b.type, layout_region_policy(b.original_class_id).purpose, layout_geometry(b)});
    }
    RegionStructure plan = plan_image_structure(items, width, height);
    std::map<std::string, std::vector<std::string>> members;
    std::set<std::string> contained;
    std::map<std::string, Box> group_bounds;
    for (auto& group : plan.groups) {
        for (const auto& item : group.items) {
            for (const auto& bid : {item.image_block_id, item.caption_block_id}) {
                if (bid.empty()) continue;
                const Box crop = blocks.at(by_id.at(bid)).box;
                group.bbox = {std::min(group.bbox.x0, crop.x0), std::min(group.bbox.y0, crop.y0),
                              std::max(group.bbox.x1, crop.x1), std::max(group.bbox.y1, crop.y1)};
            }
        }
        const std::string leader = group.items.front().image_block_id;
        group_bounds[leader] = group.bbox;
        for (const auto& item : group.items) {
            members[leader].push_back(item.image_block_id);
            contained.insert(item.image_block_id);
            if (!item.caption_block_id.empty()) {
                members[leader].push_back(item.caption_block_id);
                contained.insert(item.caption_block_id);
            }
        }
    }
    for (const auto& caption : plan.captions) {
        if (contained.count(caption.image_block_id)) continue;
        members[caption.image_block_id] = {caption.image_block_id, caption.caption_block_id};
        contained.insert(caption.image_block_id);
        contained.insert(caption.caption_block_id);
    }
    std::vector<Block> units;
    for (const auto& block : blocks) {
        if (contained.count(block.id) && !members.count(block.id)) continue;
        units.push_back(block);
        if (group_bounds.count(block.id)) {
            auto& unit = units.back();
            unit.box = unit.layout_box = group_bounds.at(block.id);
            unit.crop_expanded = false;
            // A supported horizontal row is one structural unit, not several columns.
            unit.model_label = "doc_title";
        }
    }
    evidence = arrange_reading_order(units, width);
    if (!plan.groups.empty()) evidence = {"geometry", "planned_horizontal_groups"};
    std::vector<Block> ordered;
    for (const auto& unit : units) {
        const auto ids = members.count(unit.id) ? members.at(unit.id) : std::vector<std::string>{unit.id};
        for (const auto& bid : ids) {
            auto& block = blocks.at(by_id.at(bid));
            plan.block_order.push_back(block.id);
            plan.region_order.push_back(block.region_id);
            if (requires_ovis(block.type)) plan.recognition_order.push_back(block.region_id);
            ordered.push_back(std::move(block));
        }
    }
    blocks = std::move(ordered);
    return plan;
}

std::string structure_json(const RegionStructure& plan) {
    using Json = nlohmann::json;
    Json value = {{"stage", "before_recognition"}, {"policy", "image_structure_v2"},
        {"block_order", plan.block_order}, {"region_order", plan.region_order},
        {"recognition_order", plan.recognition_order},
        {"groups", Json::array()}, {"captions", Json::array()},
        {"ambiguous_caption_ids", plan.ambiguous_caption_ids}};
    for (const auto& group : plan.groups) {
        Json items = Json::array();
        for (const auto& item : group.items)
            items.push_back({{"image_block_id", item.image_block_id},
                {"caption_block_id", item.caption_block_id.empty() ? Json(nullptr) : Json(item.caption_block_id)}});
        value["groups"].push_back({{"id", group.id}, {"type", "horizontal_images"},
            {"bbox", {group.bbox.x0, group.bbox.y0, group.bbox.x1, group.bbox.y1}},
            {"evidence", "fixed_anchor_and_shared_body_or_labels"}, {"items", items}});
    }
    for (const auto& caption : plan.captions)
        value["captions"].push_back({{"image_block_id", caption.image_block_id},
                                     {"caption_block_id", caption.caption_block_id}});
    return value.dump();
}

std::string footnote_marker(const std::string& text) {
    for (const std::string& marker : {"¹", "²", "³", "⁴", "⁵", "①", "②", "③", "④", "⑤",
                                      "[1]", "[2]", "[3]", "[4]", "[5]"}) {
        if (text.rfind(marker, 0) != 0) continue;
        if (marker.front() != '[') {
            for (const std::string& digit : {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"})
                if (text.compare(marker.size(), digit.size(), digit) == 0) return {};
        }
        return marker;
    }
    return {};
}

int count_footnote_references(const std::string& text, const std::string& marker) {
    const bool superscript = marker.front() != '[';
    const std::vector<std::string> superscripts = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    int count = 0;
    for (size_t pos = text.find(marker); pos != std::string::npos;
         pos = text.find(marker, pos + marker.size())) {
        bool adjacent = false;
        if (superscript) for (const auto& digit : superscripts) {
            adjacent |= pos >= digit.size() && text.compare(pos - digit.size(), digit.size(), digit) == 0;
            adjacent |= text.compare(pos + marker.size(), digit.size(), digit) == 0;
        }
        if (!adjacent) ++count;
    }
    return count;
}

bool numbered_table_caption(const std::string& text) {
    size_t offset = text.rfind("表", 0) == 0 ? std::string("表").size() :
        text.rfind("Table ", 0) == 0 ? std::string("Table ").size() : 0;
    if (!offset) return false;
    while (offset < text.size() && text[offset] == ' ') ++offset;
    if (offset == text.size()) return false;
    if (text[offset] >= '0' && text[offset] <= '9') return true;
    for (const std::string& numeral : {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"})
        if (text.compare(offset, numeral.size(), numeral) == 0) return true;
    return false;
}

std::vector<SemanticRelation> associate_annotations(const std::vector<Block>& blocks,
                                                     int page_width, int page_height) {
    std::vector<SemanticRelation> relations;
    for (const Block& caption : blocks) {
        bool figure = caption.model_label == "figure_title";
        bool table = caption.type == "text" && caption.status == "ok" &&
            numbered_table_caption(caption.text);
        bool footnote = caption.model_label == "footnote" || caption.model_label == "vision_footnote";
        if (!figure && !table && !footnote) continue;
        const std::string marker = footnote ? footnote_marker(caption.text) : "";
        if (footnote && marker.empty()) continue;
        const Block* target = nullptr;
        int best_distance = std::numeric_limits<int>::max();
        bool tie = false;
        int footnote_matches = 0;
        for (const Block& other : blocks) {
            if (other.id == caption.id) continue;
            if (figure && other.type != "image") continue;
            if (table && other.type != "table") continue;
            if (footnote) {
                if (other.type != "text" || other.model_label == "footnote" ||
                    other.model_label == "vision_footnote" ||
                    count_footnote_references(other.text, marker) == 0)
                    continue;
            }
            int overlap = std::min(layout_geometry(caption).x1, layout_geometry(other).x1) -
                          std::max(layout_geometry(caption).x0, layout_geometry(other).x0);
            if (overlap <= 0) continue;
            int gap = layout_geometry(caption).y0 >= layout_geometry(other).y1 ? layout_geometry(caption).y0 - layout_geometry(other).y1 :
                layout_geometry(other).y0 >= layout_geometry(caption).y1 ? layout_geometry(other).y0 - layout_geometry(caption).y1 : -1;
            int max_gap = std::max(8, std::min(page_height * 3 / 100,
                std::max(layout_geometry(caption).y1 - layout_geometry(caption).y0, layout_geometry(other).y1 - layout_geometry(other).y0)));
            if (gap < 0 || (!footnote && gap > max_gap) ||
                (footnote && layout_geometry(other).y1 > layout_geometry(caption).y0)) continue;
            if (footnote) footnote_matches += count_footnote_references(other.text, marker);
            if (gap < best_distance) { target = &other; best_distance = gap; tie = false; }
            else if (gap == best_distance) tie = true;
        }
        if (target && !tie && (!footnote || footnote_matches == 1)) relations.push_back({footnote ? "footnote_of" : "caption_of",
            caption.id, target->id, footnote ? "model_footnote_marker_and_geometry" :
            figure ? "model_figure_title_and_geometry" : "table_prefix_and_geometry"});
    }
    for (const Block& heading : blocks) {
        if (heading.type != "text" || heading.status != "ok" || heading.text.empty() ||
            (heading.model_label != "doc_title" && heading.model_label != "paragraph_title") ||
            numbered_table_caption(heading.text)) continue;
        const bool span = spans_columns(heading, page_width);
        const Block* first[3] = {nullptr, nullptr, nullptr};
        int best_gap[3] = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                           std::numeric_limits<int>::max()};
        bool ties[3] = {false, false, false};
        for (const Block& other : blocks) {
            if (other.id == heading.id || other.type == "unknown" ||
                other.model_label == "figure_title" || other.model_label == "footnote" ||
                other.model_label == "vision_footnote" || other.model_label == "footer" ||
                other.model_label == "doc_title" || other.model_label == "paragraph_title" ||
                layout_geometry(other).y0 < layout_geometry(heading).y1) continue;
            int overlap = std::min(layout_geometry(heading).x1, layout_geometry(other).x1) -
                          std::max(layout_geometry(heading).x0, layout_geometry(other).x0);
            if (overlap <= 0) continue;
            int gap = layout_geometry(other).y0 - layout_geometry(heading).y1;
            int max_gap = std::max(12, std::min(page_height * 5 / 100,
                2 * (layout_geometry(heading).y1 - layout_geometry(heading).y0)));
            if (gap > max_gap) continue;
            int column = span ? layout_geometry(other).x1 <= page_width * 0.50 ? 0 :
                layout_geometry(other).x0 >= page_width * 0.50 ? 1 : 2 : 0;
            if (gap < best_gap[column]) {
                first[column] = &other; best_gap[column] = gap; ties[column] = false;
            } else if (gap == best_gap[column]) ties[column] = true;
        }
        for (int column = 0; column < (span ? 3 : 1); ++column)
            if (first[column] && !ties[column]) relations.push_back({
                "heading_precedes", heading.id, first[column]->id, "model_heading_and_geometry"});
    }
    return relations;
}

struct RawLayoutCandidate {
    int id = 0, class_id = 0, rank = 0, mask_nonzero = 0;
    float score = 0, box[4]{}, model_box[4]{};
    std::string label, reason, handling_reason;
    std::string mask_asset;
    bool selected = false, clamped = false;
    Box crop;
    Box recognition_crop;
    bool crop_expanded = false;
};

struct OwnershipEvidence {
    const RawLayoutCandidate* candidate;
    int owner_candidate_id;
    std::string layout_id;
    std::string owner_block_id;
};

std::string canonical_label(int id);

int box_area(Box box) { return (box.x1 - box.x0) * (box.y1 - box.y0); }

bool contains(Box outer, Box inner) {
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 &&
           outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

double covered_fraction(Box outer, Box inner) {
    const int area = box_area(inner);
    if (area <= 0) return 0;
    const int width = std::max(0, std::min(outer.x1, inner.x1) -
                                  std::max(outer.x0, inner.x0));
    const int height = std::max(0, std::min(outer.y1, inner.y1) -
                                   std::max(outer.y0, inner.y0));
    return double(width) * height / area;
}

// A detected line can straddle a paragraph's boundary. Preserve its complete
// pixels in that paragraph instead of recognizing the clipped line twice.
// This does not merge disjoint adjacent paragraphs or different layout classes.
bool overlapping_text_line(const RawLayoutCandidate& paragraph, const RawLayoutCandidate& line) {
    if (paragraph.class_id != 22 || line.class_id != 22 || paragraph.id == line.id) return false;
    const Box p = paragraph.crop, l = line.crop;
    const int pw = p.x1 - p.x0, ph = p.y1 - p.y0, lw = l.x1 - l.x0, lh = l.y1 - l.y0;
    if (lh <= 0 || lw < 4 * lh || ph < 2 * lh || lw < .6 * pw || lw > 1.1 * pw) return false;
    const int horizontal = std::min(p.x1, l.x1) - std::max(p.x0, l.x0);
    return horizontal >= .9 * lw && covered_fraction(p, l) >= .4 &&
        (l.y0 <= p.y0 || l.y1 >= p.y1);
}

int table_owner(const RawLayoutCandidate& child,
                const std::vector<const RawLayoutCandidate*>& selected) {
    const std::string label = canonical_label(child.class_id);
    if (label != "text" && label != "formula") return -1;
    int owner = -1, smallest_area = std::numeric_limits<int>::max(), ties = 0;
    for (const auto* candidate : selected) {
        if (canonical_label(candidate->class_id) != "table" ||
            covered_fraction(candidate->crop, child.crop) < 0.9) continue;
        int area = box_area(candidate->crop);
        if (area <= box_area(child.crop)) continue;
        if (area < smallest_area) {
            smallest_area = area; owner = candidate->id; ties = 1;
        } else if (area == smallest_area) ++ties;
    }
    return ties == 1 ? owner : -1;
}

int inline_formula_owner(const RawLayoutCandidate& formula,
                         const std::vector<const RawLayoutCandidate*>& selected,
                         const std::vector<int>& table_owners) {
    if (canonical_label(formula.class_id) != "formula") return -1;
    int owner = -1, smallest_area = std::numeric_limits<int>::max(), ties = 0;
    for (const auto* candidate : selected) {
        if (canonical_label(candidate->class_id) != "text" ||
            table_owners[size_t(candidate->id)] >= 0 ||
            candidate->class_id == 11 || candidate->class_id == 16 ||
            covered_fraction(candidate->crop, formula.crop) < 0.85) continue;
        int area = box_area(candidate->crop);
        if (area <= box_area(formula.crop)) continue;
        if (area < smallest_area) {
            smallest_area = area;
            owner = candidate->id;
            ties = 1;
        } else if (area == smallest_area) ++ties;
    }
    return ties == 1 ? owner : -1;
}

const char* layout_label(int id) {
    return layout_region_policy(id).model_label;
}

std::string canonical_label(int id) {
    return region_type_name(layout_region_policy(id).type);
}

bool decode_real_layout(const TensorOutput& output, const Image& image,
                        std::vector<RawLayoutCandidate>& records,
                        const uint8_t*& masks,
                        const LayoutPageTransform* transform = nullptr, double score_threshold = .5) {
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
        float model_box[4];
        std::copy(row+2, row+6, model_box);
        if (transform && transform->applied) {
            row[2] = float((row[2] - transform->pad_x) / transform->scale_x);
            row[4] = float((row[4] - transform->pad_x) / transform->scale_x);
            row[3] = float((row[3] - transform->pad_y) / transform->scale_y);
            row[5] = float((row[5] - transform->pad_y) / transform->scale_y);
            for (int k = 2; k < 6; ++k) if (!std::isfinite(row[k])) return false;
        }
        if (row[1] < 0 || row[1] > 1 || row[0] < 0 || row[0] > 1000000 ||
            std::trunc(row[0]) != row[0] || row[6] < -1 || row[6] > 1000000 ||
            std::trunc(row[6]) != row[6]) return false;
        RawLayoutCandidate candidate;
        candidate.id = i;
        candidate.class_id = int(row[0]);
        candidate.score = row[1];
        candidate.rank = int(row[6]);
        for (int k = 0; k < 4; ++k) {
            candidate.box[k] = row[k+2];
            candidate.model_box[k] = model_box[k];
        }
        const char* label = layout_label(candidate.class_id);
        candidate.label = label ? label : "unknown";
        if (candidate.score < float(score_threshold)) candidate.reason = "below_score_threshold";
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

// PaddleX release/3.7 object_detection.processors.nms, applied to the raw model boxes.
void deduplicate_layout(std::vector<RawLayoutCandidate>& records, const Image& image) {
    std::vector<size_t> order;
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].selected) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (records[a].score != records[b].score) return records[a].score > records[b].score;
        return a > b; // numpy argsort(scores)[::-1] on equal scores
    });
    std::vector<size_t> kept;
    for (size_t index : order) {
        auto& candidate = records[index];
        for (size_t previous : kept) {
            const auto& winner = records[previous];
            const double left = std::max(candidate.box[0], winner.box[0]);
            const double top = std::max(candidate.box[1], winner.box[1]);
            const double right = std::min(candidate.box[2], winner.box[2]);
            const double bottom = std::min(candidate.box[3], winner.box[3]);
            // The frozen PaddleX iou() uses inclusive coordinates, even for float boxes.
            const double intersection = std::max(0.0, right-left+1) * std::max(0.0, bottom-top+1);
            const double candidate_area = double(candidate.box[2]-candidate.box[0]+1) *
                                          (candidate.box[3]-candidate.box[1]+1);
            const double winner_area = double(winner.box[2]-winner.box[0]+1) *
                                       (winner.box[3]-winner.box[1]+1);
            const double iou = intersection / (candidate_area + winner_area - intersection);
            const bool same_class = candidate.class_id == winner.class_id;
            if (iou >= (same_class ? 0.6 : 0.98)) {
                candidate.selected = false;
                candidate.reason = same_class ? "nms_same_class" : "nms_cross_class";
                break;
            }
        }
        if (candidate.selected) kept.push_back(index);
    }
    if (kept.size() <= 1) return;
    const double limit = image.width > image.height ? 0.82 : 0.93;
    std::vector<size_t> large_images;
    for (size_t index : kept) {
        auto& candidate = records[index];
        if (candidate.class_id != 14) continue; // official label "image"
        const double x0 = std::max(0.0, double(candidate.box[0]));
        const double y0 = std::max(0.0, double(candidate.box[1]));
        const double x1 = std::min(double(image.width), double(candidate.box[2]));
        const double y1 = std::min(double(image.height), double(candidate.box[3]));
        if ((x1-x0)*(y1-y0) > limit * image.width * image.height) {
            large_images.push_back(index);
        }
    }
    // PaddleX restores the input when the area filter would remove every box.
    if (large_images.size() == kept.size()) return;
    for (size_t index : large_images) {
        records[index].selected = false;
        records[index].reason = "large_page_image";
    }
}

double overlap_of_smaller(const Box& a, const Box& b) {
    const double area = std::min(box_area(a), box_area(b));
    if (area <= 0) return 0;
    const int width = std::max(0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
    const int height = std::max(0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    return double(width) * height / area;
}

// PaddleX 的 large 模式只筛除被该类别包含的框；union 不改变几何形状。
void filter_layout_containment(std::vector<RawLayoutCandidate>& records) {
    constexpr int large_classes[] = {3, 5, 6, 15, 17};
    std::vector<bool> active;
    active.reserve(records.size());
    for (const auto& candidate : records) active.push_back(candidate.selected);
    for (auto& inner : records) {
        if (!active[size_t(inner.id)]) continue;
        for (int large_class : large_classes) {
            bool contained = false;
            for (const auto& outer : records) {
                if (!active[size_t(outer.id)] || inner.id == outer.id || outer.class_id != large_class)
                    continue;
                // 官方 check_containment 对公式类别 5 的这一方向做保护。
                if (inner.class_id == 5 && outer.class_id != 5) continue;
                const double inner_area = double(inner.box[2]-inner.box[0]) *
                                          (inner.box[3]-inner.box[1]);
                const double width = std::max(0.0, double(std::min(inner.box[2], outer.box[2]) -
                                                    std::max(inner.box[0], outer.box[0])));
                const double height = std::max(0.0, double(std::min(inner.box[3], outer.box[3]) -
                                                     std::max(inner.box[1], outer.box[1])));
                if (inner_area > 0 && width*height / inner_area >= 0.9) {
                    contained = true;
                    break;
                }
            }
            if (contained) {
                inner.selected = false;
                inner.reason = "contained_by_large_class";
                break;
            }
        }
    }
}

// 外层 filter_boxes 使用裁到页面后的矩形；合法内容父子关系由区域规划负责。
void filter_layout_overlap(std::vector<RawLayoutCandidate>& records, const Image& image) {
    if (image.width < 6 || image.height < 6) return; // 旧版 2x2 契约夹具
    for (auto& candidate : records) {
        if (!candidate.selected) continue;
        if (candidate.class_id == 18) {
            candidate.selected = false;
            candidate.reason = "outer_reference";
        }
    }
    for (size_t i = 0; i < records.size(); ++i) {
        auto& first = records[i];
        if (!first.selected) continue;
        for (size_t j = i+1; j < records.size(); ++j) {
            auto& second = records[j];
            if (!second.selected || overlap_of_smaller(first.crop, second.crop) <= 0.7) continue;
            if (overlapping_text_line(first, second) || overlapping_text_line(second, first)) continue;
            const std::string first_label = first.label, second_label = second.label;
            const bool table_child =
                (first.class_id == 21 && (second_label == "text" || second_label == "formula") &&
                 covered_fraction(first.crop, second.crop) >= 0.9) ||
                (second.class_id == 21 && (first_label == "text" || first_label == "formula") &&
                 covered_fraction(second.crop, first.crop) >= 0.9);
            const bool inline_formula =
                (first_label == "formula" && second_label == "text" &&
                 covered_fraction(second.crop, first.crop) >= 0.85) ||
                (second_label == "formula" && first_label == "text" &&
                 covered_fraction(first.crop, second.crop) >= 0.85);
            const bool annotation = first_label == "figure_title" ||
                second_label == "figure_title" || first_label == "footnote" ||
                second_label == "footnote" || first_label == "vision_footnote" ||
                second_label == "vision_footnote";
            const double size_ratio = double(std::min(box_area(first.crop), box_area(second.crop))) /
                                      std::max(box_area(first.crop), box_area(second.crop));
            if (table_child || inline_formula || (annotation && size_ratio < 0.8)) continue;
            const bool special_first = first_label == "image" || first_label == "table" ||
                                       first_label == "chart" || first_label == "seal";
            const bool special_second = second_label == "image" || second_label == "table" ||
                                        second_label == "chart" || second_label == "seal";
            if ((special_first || special_second) && first_label != second_label &&
                (first_label != "table" && second_label != "table" ||
                 (special_first && special_second))) continue;
            auto& loser = box_area(first.crop) >= box_area(second.crop) ? second : first;
            loser.selected = false;
            loser.reason = "outer_overlap";
            if (&loser == &first) break;
        }
    }
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

std::string trim_formula(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

size_t formula_argument_end(const std::string& value, size_t pos) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' ||
                                  value[pos] == '\n')) ++pos;
    if (pos == value.size() || value[pos] != '{') return std::string::npos;
    int depth = 0;
    for (size_t i = pos; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() &&
            (value[i+1] == '{' || value[i+1] == '}')) { ++i; continue; }
        if (value[i] == '{') ++depth;
        else if (value[i] == '}' && --depth == 0)
            return trim_formula(value.substr(pos + 1, i - pos - 1)).empty() ?
                std::string::npos : i + 1;
    }
    return std::string::npos;
}

size_t formula_arrow_argument_end(const std::string& value, size_t pos) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == '\n')) ++pos;
    if (pos < value.size() && value[pos] == '[') {
        int brackets = 1, braces = 0;
        for (++pos; pos < value.size(); ++pos) {
            const char ch = value[pos];
            if (ch == '\\' && pos + 1 < value.size()) { ++pos; continue; }
            if (ch == '{') ++braces;
            else if (ch == '}' && --braces < 0) return std::string::npos;
            else if (braces == 0 && ch == '[') ++brackets;
            else if (braces == 0 && ch == ']' && --brackets == 0) { ++pos; break; }
        }
        if (brackets != 0 || braces != 0) return std::string::npos;
    }
    return formula_argument_end(value, pos);
}

struct ScalableDelimiter {
    char symbol = 0;
    size_t width = 0;
};

ScalableDelimiter scalable_delimiter(const std::string& value, size_t pos) {
    if (pos >= value.size()) return {};
    char ch = value[pos];
    if (ch == '\\' && pos + 1 < value.size() &&
        (value[pos+1] == '{' || value[pos+1] == '}' ||
         value[pos+1] == '|')) return {value[pos+1], 2};
    for (const auto& named : {
             std::pair<const char*, char>{"\\langle", '<'}, {"\\rangle", '>'},
             {"\\lvert", '|'}, {"\\rvert", '|'},
             {"\\lbrace", '{'}, {"\\rbrace", '}'}}) {
        size_t width = std::strlen(named.first);
        if (value.compare(pos, width, named.first) == 0)
            return {named.second, width};
    }
    return std::string("()[]|.<>{}").find(ch) == std::string::npos ?
        ScalableDelimiter{} : ScalableDelimiter{ch, 1};
}

ParsedFormula parse_formula(const std::string& raw, bool check_scripts = true) {
    ParsedFormula result;
    std::string value = trim_formula(raw);
    if (value.empty()) return result;
    const auto wrapped = [&](const std::string& open, const std::string& close,
                             bool display) {
        if (value.compare(0, open.size(), open) != 0) return false;
        if (value.size() < open.size() + close.size() ||
            value.compare(value.size() - close.size(), close.size(), close) != 0)
            return false;
        value = trim_formula(value.substr(open.size(), value.size() - open.size() - close.size()));
        result.display = display;
        return true;
    };
    bool has_wrapper = false;
    if (value.rfind("$$", 0) == 0) has_wrapper = wrapped("$$", "$$", true);
    else if (value.rfind("\\[", 0) == 0) has_wrapper = wrapped("\\[", "\\]", true);
    else if (value.rfind("\\(", 0) == 0) has_wrapper = wrapped("\\(", "\\)", false);
    else if (value[0] == '$') has_wrapper = wrapped("$", "$", false);
    else if (value.find("$$") != std::string::npos || value.find("\\]") != std::string::npos ||
             value.find("\\)") != std::string::npos) return result;
    if (!has_wrapper && (raw.find("$$") != std::string::npos ||
                         raw.find("\\[") != std::string::npos ||
                         raw.find("\\(") != std::string::npos || raw[0] == '$'))
        return result;
    if (value.empty() || value.find("$$") != std::string::npos ||
        value.find("\\[") != std::string::npos || value.find("\\]") != std::string::npos ||
        value.find("\\(") != std::string::npos || value.find("\\)") != std::string::npos ||
        value.find("![") != std::string::npos || value.find("\x60\x60\x60") != std::string::npos ||
        value.find("\\tag") != std::string::npos || value.find("\\label") != std::string::npos)
        return result;
    int depth = 0, text_depth = -1;
    size_t annotation_end = 0, script_end = 0;
    std::vector<char> delimiters;
    std::vector<int> scalable;
    struct FormulaEnvironment {
        std::string name;
        std::vector<char> delimiters;
        std::vector<int> scalable;
        size_t content_begin;
    };
    std::vector<FormulaEnvironment> environments;
    bool next_text_brace = false, math_evidence = has_wrapper;
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '_' || ch == '^') {
            const size_t end = formula_argument_end(value, i + 1);
            if (end != std::string::npos) script_end = std::max(script_end, end);
        }
        if (ch == '\\') {
            size_t start = ++i;
            size_t skip_delimiter = 0;
            while (i < value.size() && ((value[i] >= 'A' && value[i] <= 'Z') ||
                   (value[i] >= 'a' && value[i] <= 'z'))) ++i;
            if (i == start) {
                if (i >= value.size() || value[i] == '\n' || value[i] == '\r') return result;
            } else {
                std::string command = value.substr(start, i - start);
                // Commands with arguments or parser effects stay explicit here.
                // The versioned catalog contains only standalone math symbols
                // and operators, verified through public exports and KaTeX.
                static const std::set<std::string> symbols = [] {
                    std::set<std::string> names;
                    const auto catalog = nlohmann::json::parse(formula_symbols_json);
                    for (const auto& category : catalog.at("categories"))
                        for (const auto& name : category) names.insert(name.get<std::string>());
                    return names;
                }();
                static const std::vector<std::string> structural = {
                    "frac", "dfrac", "tfrac", "sqrt", "left", "right", "text",
                    "mathrm", "mathbf", "mathbb", "operatorname",
                    "xrightarrow", "xleftarrow",
                    "overline", "overrightarrow", "hat", "vec", "bar",
                    "begin", "end", "quad", "qquad", "big", "Big", "bigl",
                    "bigr", "Bigl", "Bigr", "langle", "rangle", "lvert",
                    "rvert", "lbrace", "rbrace"
                };
                if (!symbols.count(command) &&
                    std::find(structural.begin(), structural.end(), command) == structural.end())
                    return result;
                if (command == "xrightarrow" || command == "xleftarrow") {
                    const size_t end = formula_arrow_argument_end(value, i);
                    if (end == std::string::npos) return result;
                    // Arrow labels are annotations and may contain Chinese or
                    // words. Keep scanning their contents to validate structure.
                    annotation_end = std::max(annotation_end, end);
                } else if (command == "sqrt") {
                    size_t argument = i;
                    if (argument < value.size() && value[argument] == '[') {
                        argument = value.find(']', argument);
                        if (argument == std::string::npos) return result;
                        ++argument;
                    }
                    if (formula_argument_end(value, argument) == std::string::npos)
                        return result;
                } else {
                    size_t argument = i;
                    for (int group = 0; group < formula_group_arguments(command); ++group) {
                        argument = formula_argument_end(value, argument);
                        if (argument == std::string::npos) return result;
                    }
                }
                if (command == "left" || command == "right") {
                    ScalableDelimiter delimiter = scalable_delimiter(value, i);
                    if (!delimiter.symbol) return result;
                    skip_delimiter = delimiter.width;
                    if (command == "left") {
                        scalable.push_back(depth);
                    } else {
                        if (scalable.empty() || scalable.back() != depth) return result;
                        if (!environments.empty() && scalable.size() <= environments.back().scalable.size())
                            return result;
                        scalable.pop_back();
                    }
                }
                if (command == "begin" || command == "end") {
                    size_t end = formula_argument_end(value, i);
                    size_t environment_name_start = value.find('{', i);
                    std::string environment = value.substr(environment_name_start + 1, end - environment_name_start - 2);
                    if (environment != "aligned" && environment != "array" &&
                        environment != "matrix" && environment != "pmatrix" &&
                        environment != "bmatrix" && environment != "cases")
                        return result;
                    if (command == "begin") {
                        if (environment == "array") {
                            size_t columns_end = formula_argument_end(value, end);
                            if (columns_end == std::string::npos) return result;
                            size_t columns_start = value.find('{', end);
                            const std::string columns = value.substr(columns_start + 1, columns_end - columns_start - 2);
                            if (columns.find_first_not_of("clr| \t\n") != std::string::npos ||
                                columns.find_first_of("clr") == std::string::npos) return result;
                            end = columns_end;
                        }
                        environments.push_back({environment, delimiters, scalable, end});
                    }
                    else {
                        if (environments.empty() || environments.back().name != environment ||
                            environments.back().delimiters != delimiters ||
                            environments.back().scalable != scalable ||
                            trim_formula(value.substr(environments.back().content_begin,
                                start - 1 - environments.back().content_begin)).empty())
                            return result;
                        environments.pop_back();
                    }
                    // The checked environment name is metadata, not a math/prose
                    // token. Its braces were already validated by argument_end.
                    skip_delimiter = end - i;
                }
                next_text_brace = command == "text" || command == "mathrm" ||
                                  command == "operatorname";
                math_evidence = true;
                if (skip_delimiter) i += skip_delimiter - 1;
                else --i;
            }
            continue;
        }
        if (ch == '{') {
            delimiters.push_back('{');
            ++depth;
            if (next_text_brace) text_depth = depth;
            next_text_brace = false;
        } else if (ch == '}') {
            if (delimiters.empty() || delimiters.back() != '{') return result;
            if (!scalable.empty() && scalable.back() == depth) return result;
            if (!environments.empty() && delimiters.size() <= environments.back().delimiters.size())
                return result;
            delimiters.pop_back();
            if (--depth < 0) return result;
            if (text_depth > depth) text_depth = -1;
        }
        // Round and square brackets are printed symbols in TeX, not grouping
        // syntax. Mixed interval endpoints such as [a,b) are valid content.
        else if (ch >= 0x80 && text_depth < 0 && i >= annotation_end && i >= script_end) return result;
        else if (ch == '$' || ch == '#' || ch == '%' || ch == '\x60' ||
                 (ch == '&' && environments.empty())) return result;
        else if (ch >= 'A' && ch <= 'Z' && text_depth < 0 && i >= annotation_end) {
            size_t end = i + 1;
            while (end < value.size() && ((value[end] >= 'A' && value[end] <= 'Z') ||
                   (value[end] >= 'a' && value[end] <= 'z'))) ++end;
            const bool geometry_points = end - i <= 4 && std::all_of(
                value.begin() + i, value.begin() + end, [](char letter) {
                    return letter >= 'A' && letter <= 'Z';
                });
            // Chemical symbols are uppercase letters with an optional lowercase
            // suffix. This checks notation (FeO, NaOH), not chemical correctness.
            bool chemical_symbols = true;
            for (size_t symbol = i; symbol < end;) {
                if (value[symbol] < 'A' || value[symbol] > 'Z') {
                    chemical_symbols = false; break;
                }
                ++symbol;
                if (symbol < end && value[symbol] >= 'a' && value[symbol] <= 'z') ++symbol;
            }
            if (end - i > 2 && !geometry_points && !chemical_symbols && i >= script_end) return result;
            i = end - 1;
        } else if (ch >= 'a' && ch <= 'z' && text_depth < 0 && i >= annotation_end) {
            size_t end = i + 1;
            while (end < value.size() && ((value[end] >= 'A' && value[end] <= 'Z') ||
                   (value[end] >= 'a' && value[end] <= 'z'))) ++end;
            if (end - i > 2 && i >= script_end) return result;
            i = end - 1;
        }
        else if (ch == '^' || ch == '_' || ch == '=' || ch == '<' || ch == '>' ||
                 ch == '+' || ch == '-' ||
                 ch == '*' || ch == '/' || ch == '{' || ch == '}') math_evidence = true;
        else if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') return result;
    }
    if (depth != 0 || !delimiters.empty() || !scalable.empty() ||
        !environments.empty() ||
        value.back() == '\\' || value.back() == '^' || value.back() == '_' ||
        // A complete transcription may faithfully end with an exam answer blank,
        // such as "$z=$". Generation completeness is checked separately.
        value.back() == '+' || value.back() == '-' ||
        !math_evidence || (check_scripts && !valid_formula_scripts(value))) return result;
    result.valid = true;
    result.latex = std::move(value);
    return result;
}

bool valid_text_math(const std::string& text, size_t* math_spans = nullptr, bool check_scripts = true) {
    if (math_spans) *math_spans = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i+1] == '$' || text[i+1] == '\\')) { i += 2; continue; }
        const size_t amount_end = currency_amount_end(text, i);
        if (amount_end > i) { i = amount_end; continue; }
        std::string close;
        size_t open_length = 0;
        if (text[i] == '$') {
            open_length = i + 1 < text.size() && text[i+1] == '$' ? 2 : 1;
            close = std::string(open_length, '$');
        } else if (text[i] == '\\' && i + 1 < text.size() &&
                   (text[i+1] == '(' || text[i+1] == '[')) {
            open_length = 2;
            close = text[i+1] == '(' ? "\\)" : "\\]";
        } else if (text[i] == '\\' && i + 1 < text.size() &&
                   (text[i+1] == ')' || text[i+1] == ']')) {
            return false;
        } else { ++i; continue; }
        size_t end = i + open_length;
        bool found = false;
        for (; end + close.size() <= text.size(); ++end) {
            if (text.compare(end, close.size(), close) != 0) continue;
            size_t slashes = 0;
            for (size_t k = end; k > 0 && text[k-1] == '\\'; --k) ++slashes;
            if (slashes % 2 == 0 || close == "\\)" || close == "\\]") {
                found = true; break;
            }
        }
        if (!found || !parse_formula(text.substr(i, end + close.size() - i), check_scripts).valid)
            return false;
        if (math_spans) ++*math_spans;
        i = end + close.size();
    }
    return true;
}

ParsedFormulaRegion parse_formula_region_policy(const std::string& generated, bool check_scripts) {
    const auto single = parse_formula(generated, check_scripts);
    if (single.valid) return {true, "latex", single.latex, single.display};
    size_t spans = 0;
    if (valid_text_math(generated, &spans, check_scripts) && spans > 0)
        return {true, "markdown", generated, false};
    return {};
}

std::string render(const Block& b, bool uncertain_caption = false, bool semantic_text = false) {
    bool safe_table = b.table.valid;
    if (b.type == "table" && b.status == "ok" && !safe_table) {
        auto table = parse_table(b.text);
        safe_table = table.valid && table.html == b.text;
    }
    return render_markdown_block({b.id, b.type, b.status, b.text, b.resource,
                                  b.display_formula, safe_table, b.assessment.state,
                                  b.assessment.state == "skipped" ? "" : b.assessment.reason,
                                  semantic_text ? layout_semantic_label(b.original_class_id) : "",
                                  b.type == "formula" && b.format_override == "markdown"}, uncertain_caption);
}

std::string serialize(const Image& image, const std::string& state,
                      const std::vector<Block>& blocks, const std::string& profile,
                      const std::vector<RawLayoutCandidate>* raw = nullptr,
                      const std::string& overlay = {},
                      const std::vector<OwnershipEvidence>& ownership = {},
                      bool structured_tables = false,
                      const ReadingOrderEvidence* order_evidence = nullptr,
                      const std::vector<SemanticRelation>& semantic = {},
                      const LayoutPageTransform* layout_transform = nullptr, double score_threshold = .5,
                      const RegionStructure* structure = nullptr,
                      const LabelExportPolicy* label_export = nullptr,
                      const std::string& candidate_reviews = {}) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    bool has_table = structured_tables &&
        std::any_of(blocks.begin(), blocks.end(), [](const Block& b) { return b.type == "table"; });
    const bool has_visual = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) {
        return !b.visual.visual_evidence.empty();
    });
    const bool mixed_formula = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) {
        return b.type == "formula" && b.status == "ok" && b.format_override == "markdown";
    });
    out << "{\"schema_version\":" << json_quote(!candidate_reviews.empty() ? "1.12" : label_export ? (mixed_formula ? "1.11" : "1.10") : structure ? "1.9" : structured_tables ? "1.7" : has_visual ? "1.5" : order_evidence ? "1.3" :
        has_table ? "1.2" : ownership.empty() ? "1.0" : "1.1")
        << ",\"document_id\":" << json_quote(document_id(image))
        << ",\"status\":" << json_quote(state);
    if (label_export) out << ",\"export_policy\":" << label_export_policy_json(*label_export);
    out << ",\"source\":{\"type\":" << json_quote(current_pdf_page ? "pdf" : "image")
        << "},\"pages\":[{\"page_id\":" << json_quote(page_id())
        << ",\"page_index\":" << (current_pdf_page ? current_pdf_page - 1 : 0) << ','
        << "\"raster_size\":[" << image.width << ',' << image.height << "],\"coordinate_space\":\"raster_page\","
        << "\"status\":" << json_quote(state) << ",\"reading_order\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        out << json_quote(blocks[i].id);
    }
    out << ']';
    if (order_evidence)
        out << ",\"reading_order_evidence\":{\"source\":" << json_quote(order_evidence->source)
            << ",\"reason\":" << json_quote(order_evidence->reason) << '}';
    if (structure) out << ",\"structure_plan\":" << structure_json(*structure);
    out << ",\"layout_blocks\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        if (label_export) out << "{\"semantic_label\":" << json_quote(layout_semantic_label(b.original_class_id)) << ',';
        else out << '{';
        out << "\"id\":" << json_quote(b.layout_id) << ",\"page_id\":" << json_quote(page_id()) << ",\"label\":" << json_quote(b.type)
            << ",\"bbox\":" << box_json(b.crop_expanded ? b.layout_box : b.box) << ",\"coordinate_space\":\"raster_page\","
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
            << ",\"request_id\":" << json_quote("layout-" + page_id()) << "}}";
    }
    for (const auto& evidence : ownership) {
        const auto& c = *evidence.candidate;
        if (!blocks.empty() || &evidence != &ownership.front()) out << ',';
        if (label_export) out << "{\"semantic_label\":" << json_quote(layout_semantic_label(c.class_id)) << ',';
        else out << '{';
        out << "\"id\":" << json_quote(evidence.layout_id)
            << ",\"page_id\":" << json_quote(page_id()) << ",\"label\":" << json_quote(canonical_label(c.class_id))
            << ",\"bbox\":" << box_json(c.crop)
            << ",\"coordinate_space\":\"raster_page\",\"detection_score\":" << c.score
            << ",\"candidate_rank\":" << c.rank << ",\"original_class_id\":" << c.class_id
            << ",\"candidate_id\":" << c.id << ",\"mask_row\":" << c.id
            << ",\"mask_nonzero\":" << c.mask_nonzero
            << ",\"model_label\":" << json_quote(c.label)
            << ",\"original_bbox\":[" << c.box[0] << ',' << c.box[1] << ','
            << c.box[2] << ',' << c.box[3] << "],\"clamped\":"
            << (c.clamped ? "true" : "false")
            << ",\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("layout-" + page_id()) << "}}";
    }
    out << "],\"regions\":[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << "{\"id\":" << json_quote(b.region_id) << ",\"page_id\":" << json_quote(page_id()) << ",\"source_layout_block_ids\":["
            << json_quote(b.layout_id);
        for (const auto& owned : b.owned_layout_ids) out << ',' << json_quote(owned);
        out << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\"";
        if (structure) out << ",\"recognition_type\":" << json_quote(b.type);
        out << '}';
    }
    out << "],\"blocks\":[";
    size_t body_order = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out << ',';
        const Block& b = blocks[i];
        out << '{';
        if (label_export) out << "\"block_order\":" << (label_export->has_block_order(b.original_class_id) ? std::to_string(++body_order) : "null") << ',';
        out << "\"id\":" << json_quote(b.id) << ",\"page_id\":" << json_quote(page_id()) << ",\"type\":" << json_quote(b.type)
            << ",\"source_region_ids\":[" << json_quote(b.region_id) << "],\"bbox\":" << box_json(b.box)
            << ",\"coordinate_space\":\"raster_page\",\"geometry_granularity\":\"region\","
            << "\"reading_order_source\":" << json_quote(order_evidence ? order_evidence->source : "geometry") << ','
            << "\"status\":" << json_quote(b.status) << ",\"confidence\":null,\"content\":{\"format\":"
            << json_quote(!b.format_override.empty() ? b.format_override :
                     b.type == "image" || b.type == "unknown" ? "resource" :
                     b.type == "formula" ? "latex" : b.type == "table" ? "html" : "markdown")
            << ",\"text\":" << json_quote(b.text) << ",\"resource\":"
            << (b.resource.empty() ? "null" : json_quote(b.resource));
        if (b.type == "formula") out << ",\"display\":" << (b.display_formula ? "true" : "false");
        if ((has_table || order_evidence) && b.type == "table") {
            out << ",\"table\":";
            if (!b.table.valid) out << "null";
            else {
                out << "{\"rows\":" << b.table.rows << ",\"columns\":" << b.table.columns
                    << ",\"cells\":[";
                for (size_t j = 0; j < b.table.cells.size(); ++j) {
                    if (j) out << ',';
                    const auto& cell = b.table.cells[j];
                    out << "{\"row\":" << cell.row << ",\"column\":" << cell.column
                        << ",\"rowspan\":" << cell.rowspan << ",\"colspan\":" << cell.colspan
                        << ",\"header\":" << (cell.header ? "true" : "false")
                        << ",\"text\":" << json_quote(cell.text) << ",\"bbox\":null}";
                }
                out << "]}";
            }
        }
        out
            << "},\"provenance\":{\"model_profile\":" << json_quote(profile)
            << ",\"request_id\":" << json_quote("req" + b.region_id)
            << ",\"raw_output\":" << (b.raw_base64.empty() ? json_quote(b.raw) : "null")
            << ",\"raw_output_base64\":" << (b.raw_base64.empty() ? "null" : json_quote(b.raw_base64))
            << ",\"text_base64\":" << (b.text_base64.empty() ? "null" : json_quote(b.text_base64));
        if (has_visual && !b.visual.visual_evidence.empty()) {
            const auto& transform = b.visual.visual_transform;
            const double inverse_x = transform.content_width > 0 ?
                double(b.box.x1 - b.box.x0) / transform.content_width : 0;
            const double inverse_y = transform.content_height > 0 ?
                double(b.box.y1 - b.box.y0) / transform.content_height : 0;
            out << std::setprecision(17);
            out << ",\"visual\":{\"evidence\":" << json_quote(b.visual.visual_evidence)
                << ",\"token_count\":" << b.visual.visual_tokens
                << ",\"source_crop\":" << json_quote(b.resource)
                << ",\"source_bbox\":" << box_json(b.box)
                << ",\"canvas_size\":[" << transform.canvas_width << ',' << transform.canvas_height << ']'
                << ",\"content_size\":[" << transform.content_width << ',' << transform.content_height << ']'
                << ",\"pad_offset\":[" << transform.pad_x << ',' << transform.pad_y << ']'
                << ",\"scale\":" << transform.scale
                << ",\"rounding_error\":[" << transform.rounding_error_x << ',' << transform.rounding_error_y << ']'
                << ",\"canvas_to_page_affine\":[" << inverse_x << ",0,"
                << b.box.x0 - transform.pad_x * inverse_x << ",0," << inverse_y << ','
                << b.box.y0 - transform.pad_y * inverse_y << ']'
                << ",\"stop_reason\":" << json_quote(b.visual.stop_reason) << '}';
            out << std::setprecision(9);
        }
        if (structured_tables) {
            out << ",\"recognition\":" << recognition_json(b.recognition);
            const auto& a = b.assessment;
            out << ",\"assessment\":{\"policy\":" << json_quote(assessment_policy)
                << ",\"state\":" << json_quote(a.state) << ",\"reason\":" << json_quote(a.reason)
                << ",\"finish_reason\":" << json_quote(a.finish_reason)
                << ",\"stop_reason\":" << json_quote(a.stop_reason)
                << ",\"evidence_source\":" << json_quote(a.evidence_source)
                << ",\"backend_error\":" << (b.visual.error.empty() || !valid_utf8(b.visual.error) ?
                    "null" : json_quote(b.visual.error))
                << ",\"generation_text\":" << (valid_utf8(b.visual.text) ? json_quote(b.visual.text) : "null")
                << ",\"nonempty_lines\":" << a.nonempty_lines
                << ",\"empty_number_lines\":" << a.empty_number_lines
                << ",\"repeat_offset\":" << a.repeat_offset
                << ",\"repeat_unit_bytes\":" << a.repeat_unit_bytes
                << ",\"repeat_count\":" << a.repeat_count
                << ",\"repeat_coverage\":" << std::setprecision(17) << a.repeat_coverage << '}';
            out << std::setprecision(9);
        }
        out
            << "},\"error\":" << (b.error.empty() ? "null" : json_quote(b.error))
            << ",\"error_base64\":" << (b.error_base64.empty() ? "null" : json_quote(b.error_base64)) << "}";
    }
    out << "],\"relations\":[";
    for (size_t i = 0; i < ownership.size(); ++i) {
        if (i) out << ',';
        const auto& evidence = ownership[i];
        out << "{\"type\":\"content_owned_by\",\"source_layout_block_id\":"
            << json_quote(evidence.layout_id) << ",\"owner_block_id\":"
            << json_quote(evidence.owner_block_id) << "}";
    }
    for (const auto& relation : semantic) {
        if (!ownership.empty() || &relation != &semantic.front()) out << ',';
        out << "{\"type\":" << json_quote(relation.type)
            << ",\"source_block_id\":" << json_quote(relation.source_block_id)
            << ",\"target_block_id\":" << json_quote(relation.target_block_id)
            << ",\"evidence\":" << json_quote(relation.evidence) << '}';
    }
    out << "]}],\"resources\":[";
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
        out << ",\"layout_diagnostics\":{\"score_threshold\":" << score_threshold
            << ",\"candidate_count\":" << raw->size()
            << ",\"overlay_asset\":" << json_quote(overlay)
            << ",\"raw_tensor_assets\":{\"image\":" << json_quote(page_asset("image.f32"))
            << ",\"im_shape\":" << json_quote(page_asset("im_shape.f32"))
            << ",\"scale_factor\":" << json_quote(page_asset("scale_factor.f32"))
            << ",\"fetch_name_0\":" << json_quote(page_asset("fetch_name_0.f32"))
            << ",\"fetch_name_1\":" << json_quote(page_asset("fetch_name_1.i32"))
            << ",\"fetch_name_2\":" << json_quote(page_asset("fetch_name_2.rle"))
            << '}';
        if (layout_transform && layout_transform->applied) {
            out << std::setprecision(17)
                << ",\"input_transform\":{\"mode\":\"smartresize_800\",\"resample\":"
                << json_quote(layout_transform->resample == LayoutResample::Area ? "area" :
                              layout_transform->resample == LayoutResample::Lanczos ? "lanczos" : "bilinear")
                << ",\"source_size\":["
                << image.width << ',' << image.height << "],\"canvas_size\":[800,800],\"content_size\":["
                << layout_transform->content_width << ',' << layout_transform->content_height
                << "],\"pad_offset\":[" << layout_transform->pad_x << ',' << layout_transform->pad_y
                << "],\"canvas_to_page_affine\":[" << 1/layout_transform->scale_x << ",0,"
                << -layout_transform->pad_x/layout_transform->scale_x << ",0,"
                << 1/layout_transform->scale_y << ','
                << -layout_transform->pad_y/layout_transform->scale_y << "]}";
            out << std::setprecision(9);
        }
        out << ",\"candidates\":[";
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
                << ",\"handling_reason\":" << (c.handling_reason.empty() ?
                    (c.selected && c.label == "unknown" ? "\"unknown_class\"" : "null") :
                    json_quote(c.handling_reason))
                << ",\"mask_asset\":" << (c.mask_asset.empty() ? "null" : json_quote(c.mask_asset))
                << ",\"clamped\":" << (c.clamped ? "true" : "false")
                << ",\"crop_bbox\":" << (c.selected ? box_json(c.crop) : "null")
                << ",\"recognition_crop_bbox\":" << (c.crop_expanded ? box_json(c.recognition_crop) : "null")
                << ",\"crop_expansion_reason\":" << (c.crop_expanded ? "\"owned_content_union\"" : "null") << '}';
        }
        out << ']';
        if (!candidate_reviews.empty()) out << ",\"candidate_reviews\":" << candidate_reviews;
        out << '}';
    }
    out << '}';
    return out.str();
}
} // namespace

size_t currency_amount_end(const std::string& text, size_t i) {
    if (i >= text.size() || text[i] != '$' || i + 1 == text.size() ||
        text[i+1] < '0' || text[i+1] > '9') return 0;
    size_t slashes = 0;
    for (size_t k = i; k > 0 && text[k-1] == '\\'; --k) ++slashes;
    if (slashes % 2) return 0;
    size_t closing_math = text.find('$', i + 1);
    while (closing_math != std::string::npos) {
        size_t slashes = 0;
        for (size_t k = closing_math; k > 0 && text[k-1] == '\\'; --k)
            ++slashes;
        if (slashes % 2 == 0) break;
        closing_math = text.find('$', closing_math + 1);
    }
    bool complete_math = closing_math != std::string::npos &&
        parse_formula(text.substr(i, closing_math - i + 1)).valid;
    size_t amount_end = i + 1;
    while (amount_end < text.size() &&
           ((text[amount_end] >= '0' && text[amount_end] <= '9') ||
            text[amount_end] == ',' || text[amount_end] == '.'))
        ++amount_end;
    size_t after_space = amount_end;
    while (after_space < text.size() &&
           (text[after_space] == ' ' || text[after_space] == '\t' ||
            text[after_space] == '\r' || text[after_space] == '\n'))
        ++after_space;
    // A following legacy math opener (or an escaped dollar) starts separate
    // content. Other TeX commands still continue the numeric math candidate,
    // including non-letter commands such as \{ and \,.
    const bool separate_content = text.compare(after_space, 2, "\\(") == 0 ||
        text.compare(after_space, 2, "\\[") == 0 || text.compare(after_space, 2, "\\$") == 0;
    bool math_continues = !separate_content && after_space < text.size() &&
        std::string("$=+-*/_^<>\\").find(text[after_space]) != std::string::npos;
    if (!complete_math && !math_continues &&
        (amount_end == text.size() ||
         static_cast<unsigned char>(text[amount_end]) >= 0x80 ||
         text[amount_end] == ' ' || text[amount_end] == '\t' ||
         text[amount_end] == '\r' || text[amount_end] == '\n' ||
         text[amount_end] == ';' || text[amount_end] == ':')) {
        return amount_end;
    }
    return 0;
}
ParsedFormula parse_formula_content(const std::string& generated) {
    return parse_formula(generated);
}
ParsedFormulaRegion parse_formula_region(const std::string& generated) {
    return parse_formula_region_policy(generated, true);
}
bool matches_saved_formula_content(const std::string& generated, const std::string& content,
                                   const std::string& format, bool display) {
    // Only provenance matching uses the pre-script-check policy. Current
    // recognition, rendering and explicit revalidation always use strict rules.
    const auto parsed = parse_formula_region_policy(generated, false);
    return parsed.valid && parsed.text == content && parsed.format == format && parsed.display == display;
}
bool valid_text_math_content(const std::string& content) {
    return valid_text_math(content);
}
bool valid_table_math_content(const ParsedTable& table) {
    return table.valid && std::all_of(table.cells.begin(), table.cells.end(),
        [](const TableCell& cell) { return valid_text_math(cell.text); });
}
bool matches_formula_content(const std::string& generated, const std::string& content, bool display) {
    const auto formula = parse_formula(generated);
    return formula.valid && formula.latex == content && formula.display == display;
}

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
                                      const uint8_t* masks,
                                      const LayoutPageTransform* transform);

std::vector<uint8_t> layout_overlay(const Image& image,
                                    const std::vector<RawLayoutCandidate>& records,
                                    const uint8_t* masks,
                                    const LayoutPageTransform* transform,
                                    const Image* preview) {
    const bool smart = transform && transform->applied && preview;
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
        if (!smart) page_mask = layout_page_mask(image, c, masks, transform);
        const uint8_t* mask = masks + size_t(c.id)*200*200*sizeof(int32_t);
        for (int y = draw.y0; y < draw.y1; ++y) for (int x = draw.x0; x < draw.x1; ++x) {
            int32_t bit = 0;
            if (smart) std::memcpy(&bit, mask +
                (size_t(std::clamp(y/4, 0, 199))*200 + std::clamp(x/4, 0, 199))*sizeof(bit),
                sizeof(bit));
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

std::vector<uint8_t> layout_page_mask(const Image& image, const RawLayoutCandidate& c,
                                      const uint8_t* masks,
                                      const LayoutPageTransform* transform) {
    std::vector<uint8_t> pixels(size_t(image.width)*image.height);
    if (transform && transform->applied) {
        const uint8_t* mask = masks + size_t(c.id)*200*200*sizeof(int32_t);
        for (int y = c.crop.y0; y < c.crop.y1; ++y) {
            int my = std::clamp(int(((y + 0.5) * transform->scale_y +
                                      transform->pad_y) / 4), 0, 199);
            for (int x = c.crop.x0; x < c.crop.x1; ++x) {
                int mx = std::clamp(int(((x + 0.5) * transform->scale_x +
                                          transform->pad_x) / 4), 0, 199);
                int32_t bit;
                std::memcpy(&bit, mask + (size_t(my)*200+mx)*sizeof(bit), sizeof(bit));
                pixels[size_t(y)*image.width+x] = bit ? 255 : 0;
            }
        }
        return pixels;
    }
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

// An explicit review is scoped to exact decoded pixels and the full model row/mask.
// No size, color, location, GT absence, or label alone can authorize removal.
std::string apply_candidate_reviews(const ExecutionPlan* plan, const Image& image,
                                    const TensorOutput& output, const uint8_t* masks,
                                    std::vector<RawLayoutCandidate>& records,
                                    const LayoutPageTransform& transform, RunResult& audit) {
    if (!plan || !plan->collect_candidate_reviews) return {};
    using Json = nlohmann::json;
    std::string pixels = std::to_string(image.width) + "x" + std::to_string(image.height) + ":";
    pixels.append(reinterpret_cast<const char*>(image.rgb.data()), image.rgb.size());
    const auto page_hash = sha256(pixels);
    const auto source_asset = page_asset("review-source.png");
    const bool relevant = plan->layout_candidate_reviews.empty() ||
        std::any_of(plan->layout_candidate_reviews.begin(), plan->layout_candidate_reviews.end(),
                    [&](const LayoutCandidateReview& review) { return review.page_rgb_sha256 == page_hash; });
    if (relevant) audit.output.assets.push_back({source_asset, crop_png(image, {0, 0, image.width, image.height})});
    Json evidence = {{"policy", "explicit-candidate-review-v1"}, {"page_rgb_sha256", page_hash},
                     {"source_asset", relevant ? Json(source_asset) : Json(nullptr)}, {"decisions", Json::array()}};
    const auto tensor = std::find_if(output.outputs.begin(), output.outputs.end(),
                                   [](const Tensor& t) { return t.name == "fetch_name_0"; });
    for (const auto& review : plan->layout_candidate_reviews) {
        Json decision = {{"candidate_id", review.candidate_id}, {"page_rgb_sha256", review.page_rgb_sha256},
                         {"candidate_sha256", review.candidate_sha256}, {"decision", review.decision},
                         {"reason", review.reason}, {"outcome", "page_mismatch"}, {"crop_asset", nullptr}};
        if (review.page_rgb_sha256 == page_hash) {
            decision["outcome"] = "candidate_mismatch";
            if (size_t(review.candidate_id) < records.size()) {
                auto& candidate = records[size_t(review.candidate_id)];
                std::string bytes = box_json(candidate.crop) + ":";
                bytes.append(reinterpret_cast<const char*>(tensor->data.data() + size_t(candidate.id)*7*sizeof(float)), 7*sizeof(float));
                bytes.append(reinterpret_cast<const char*>(masks + size_t(candidate.id)*200*200*sizeof(int32_t)), 200*200*sizeof(int32_t));
                if (sha256(bytes) == review.candidate_sha256) {
                    decision["outcome"] = "already_filtered";
                    if (candidate.selected) {
                        const auto crop_asset = page_asset("review-c") + std::to_string(candidate.id) + ".png";
                        audit.output.assets.push_back({crop_asset, crop_png(image, candidate.crop)});
                        decision["crop_asset"] = crop_asset;
                        decision["outcome"] = "retained";
                        const bool confirmed = review.decision == "confirmed_watermark" || review.decision == "confirmed_decoration";
                        // Only original image (class 14) may be skipped; headers and all other labels are protected.
                        if (confirmed && candidate.class_id == 14) {
                            candidate.mask_asset = page_asset("mask-c") + std::to_string(candidate.id) + ".png";
                            auto pixels = layout_page_mask(image, candidate, masks, &transform);
                            std::vector<uint8_t> png;
                            if (!stbi_write_png_to_func(png_write, &png, image.width, image.height, 1, pixels.data(), image.width))
                                throw std::runtime_error("review mask PNG encoding failed");
                            audit.output.assets.push_back({candidate.mask_asset, std::move(png)});
                            candidate.selected = false;
                            candidate.reason = "review_" + review.decision;
                            decision["outcome"] = "skipped";
                        } else if (confirmed) decision["outcome"] = "protected_content";
                    }
                }
            }
        }
        evidence["decisions"].push_back(std::move(decision));
    }
    return evidence.dump();
}

RunResult run_layout_only(IInferenceEngine* backend, const Image& image, std::atomic_bool& cancelled,
                          const ExecutionPlan* plan, RunResult audit,
                          uint32_t source_page, const ProgressCallback& progress) {
    using Clock = std::chrono::steady_clock;
    const bool transcribe = plan && (plan->backend == "mnn:pp-doclayout-v3+ovisocr2" ||
        plan->backend.rfind("fixture:printed_page", 0) == 0);
    if (!backend->capabilities().tensor ||
        (transcribe && (!backend->capabilities().generation || !backend->capabilities().isolated_sessions)) ||
        backend->capabilities().max_concurrent_requests != 1)
        return {RunCode::Unsupported, {}};
    if (cancelled) { audit.code = RunCode::Cancelled; return audit; }
    auto start = Clock::now();
    TensorRequest request;
    const std::string preprocessing = plan ? plan->layout_preprocess : "auto";
    const double score_threshold = plan ? plan->layout_score_threshold : .5;
    const bool resize_page = preprocessing == "smartresize_area" || preprocessing == "smartresize_bilinear" ||
        preprocessing == "smartresize_lanczos" ||
        (preprocessing == "auto" && uint64_t(image.width)*image.height > max_pixels);
    const LayoutResample resample = preprocessing == "smartresize_area" ? LayoutResample::Area :
        preprocessing == "smartresize_lanczos" ? LayoutResample::Lanczos : LayoutResample::Bilinear;
    LayoutPageInput layout_input = prepare_layout_page(image, resize_page, resample);
    request.inputs.push_back(std::move(layout_input.tensor));
    request.inputs.push_back(geometry_tensor("im_shape", 800, 800));
    request.inputs.push_back(layout_input.transform.applied ?
        geometry_tensor("scale_factor", 1, 1) :
        geometry_tensor("scale_factor", 800.0f/image.height, 800.0f/image.width));
    request.requested_outputs = {"fetch_name_0", "fetch_name_1", "fetch_name_2"};
    audit.layout_attempted = true;
    audit.output.assets.push_back({page_asset("image.f32"), request.inputs[0].data});
    audit.output.assets.push_back({page_asset("im_shape.f32"), request.inputs[1].data});
    audit.output.assets.push_back({page_asset("scale_factor.f32"), request.inputs[2].data});
    audit.did_normalize = true;
    ExecutionContext context{cancelled};
    InferenceResponse response;
    if (progress) progress("layout_started", source_page ? source_page : 1, "", 0, 0);
    try { response = backend->execute({"layout-" + page_id(), std::move(request)}, context); }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception& e) {
        audit.code = cancelled ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = cancelled ? "layout_interrupted_error" : "layout_inference_failed";
        audit.error_message = "版面推理异常：" + std::string(e.what());
        audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
        return audit;
    }
    audit.layout_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
    if (cancelled) { audit.code = RunCode::Cancelled; return audit; }
    auto* output = std::get_if<TensorOutput>(&response.payload);
    std::vector<RawLayoutCandidate> records;
    const uint8_t* masks = nullptr;
    if (!output || !decode_real_layout(*output, image, records, masks, &layout_input.transform, score_threshold)) {
        audit.code = RunCode::Failed;
        audit.error_code = "layout_output_contract_mismatch";
        audit.error_message = "PP-DocLayoutV3 候选/数量/mask 张量契约不符";
        return audit;
    }
    deduplicate_layout(records, image);
    if (image.width >= 6 && image.height >= 6) {
        filter_layout_containment(records);
        filter_layout_overlap(records, image);
    }
    const auto candidate_reviews = apply_candidate_reviews(plan, image, *output, masks, records, layout_input.transform, audit);
    audit.did_layout = true;
    if (progress) progress("layout_completed", source_page ? source_page : 1, "", 0, 0);
    for (const auto& tensor : output->outputs) {
        if (tensor.name == "fetch_name_0")
            audit.output.assets.push_back({page_asset("fetch_name_0.f32"), tensor.data});
        else if (tensor.name == "fetch_name_1")
            audit.output.assets.push_back({page_asset("fetch_name_1.i32"), tensor.data});
    }
    audit.output.assets.push_back({page_asset("fetch_name_2.rle"), encode_masks_rle(masks)});
    std::vector<const RawLayoutCandidate*> selected;
    for (const auto& candidate : records) if (candidate.selected) selected.push_back(&candidate);
    std::stable_sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
        if (a->crop.y0 != b->crop.y0) return a->crop.y0 < b->crop.y0;
        return a->crop.x0 < b->crop.x0;
    });
    std::vector<OwnershipEvidence> ownership;
    std::vector<int> table_owners(records.size(), -1);
    if (transcribe) for (const auto* candidate : selected) {
        int owner = table_owner(*candidate, selected);
        if (owner < 0) continue;
        table_owners[size_t(candidate->id)] = owner;
        records[size_t(candidate->id)].handling_reason = "table_content_owned_by_table";
        ownership.push_back({candidate, owner,
            id('l', selected.size() + size_t(candidate->id) + 1), {}});
    }
    std::vector<int> text_owners(records.size(), -1);
    if (transcribe) for (const auto* child : selected) {
        if (table_owners[size_t(child->id)] >= 0) continue;
        int owner = -1, count = 0;
        for (const auto* parent : selected) {
            if (table_owners[size_t(parent->id)] >= 0 || !overlapping_text_line(*parent, *child)) continue;
            owner = parent->id;
            ++count;
        }
        if (count == 1) text_owners[size_t(child->id)] = owner;
    }
    // Parent heights strictly increase, so ownership cannot cycle. Flatten
    // before constructing Regions, including formulas owned by an absorbed line.
    auto text_root = [&](int owner) {
        while (owner >= 0 && text_owners[size_t(owner)] >= 0) owner = text_owners[size_t(owner)];
        return owner;
    };
    if (transcribe) for (const auto* child : selected) {
        if (text_owners[size_t(child->id)] < 0) continue;
        const int owner = text_root(child->id);
        records[size_t(child->id)].handling_reason = "overlapping_text_owned_by_text";
        ownership.push_back({child, owner, id('l', selected.size() + size_t(child->id) + 1), {}});
    }
    if (transcribe) for (const auto* candidate : selected) {
        if (table_owners[size_t(candidate->id)] >= 0) continue;
        int owner = text_root(inline_formula_owner(*candidate, selected, table_owners));
        if (owner < 0) continue;
        auto& owned = records[size_t(candidate->id)];
        owned.handling_reason = "inline_formula_owned_by_text";
        ownership.push_back({candidate, owner,
            id('l', selected.size() + size_t(candidate->id) + 1), {}});
    }
    std::vector<Block> blocks;
    JobOutput result;
    result.assets = std::move(audit.output.assets);
    for (const auto* candidate : selected) {
        auto& mutable_candidate = records[size_t(candidate->id)];
        mutable_candidate.mask_asset = page_asset("mask-c") +
            std::to_string(candidate->id) + ".png";
        std::vector<uint8_t> mask_pixels = layout_page_mask(image, *candidate, masks,
                                                             &layout_input.transform);
        std::vector<uint8_t> mask_png;
        if (!stbi_write_png_to_func(png_write, &mask_png, image.width, image.height, 1,
                                    mask_pixels.data(), image.width))
            throw std::runtime_error("layout mask PNG encoding failed");
        result.assets.push_back({mutable_candidate.mask_asset, std::move(mask_png)});
    }
    bool recognition_unavailable = false;
    uint32_t region_total = 0, region_done = 0;
    for (const auto* candidate : selected)
        if (records[size_t(candidate->id)].handling_reason.empty() &&
            requires_ovis(canonical_label(candidate->class_id))) ++region_total;
    std::string first_region_error_code, first_region_error_message;
    auto stop_after_cancel = [&] {
        audit.code = first_region_error_code.empty() ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = first_region_error_code;
        audit.error_message = first_region_error_message;
    };
    for (const auto* candidate : selected) {
        if (cancelled) { stop_after_cancel(); return audit; }
        if (!records[size_t(candidate->id)].handling_reason.empty())
            continue;
        Block block;
        size_t n = blocks.size()+1;
        block.id = id('b', n); block.layout_id = id('l', n); block.region_id = id('r', n);
        block.type = canonical_label(candidate->class_id);
        block.model_label = candidate->label;
        block.box = candidate->crop;
        block.layout_box = candidate->crop;
        block.candidate_id = block.mask_row = candidate->id;
        block.mask_nonzero = candidate->mask_nonzero;
        block.detection_score = candidate->score;
        block.candidate_rank = candidate->rank;
        block.original_class_id = candidate->class_id;
        block.clamped = candidate->clamped;
        std::copy(std::begin(candidate->box), std::end(candidate->box), block.raw_box);
        block.status = "skipped";
        for (const auto& evidence : ownership)
            if (evidence.owner_candidate_id == candidate->id) {
                block.owned_layout_ids.push_back(evidence.layout_id);
                const Box child = evidence.candidate->crop;
                block.box = {std::min(block.box.x0, child.x0), std::min(block.box.y0, child.y0),
                             std::max(block.box.x1, child.x1), std::max(block.box.y1, child.y1)};
            }
        block.crop_expanded = block.box.x0 != block.layout_box.x0 || block.box.y0 != block.layout_box.y0 ||
                              block.box.x1 != block.layout_box.x1 || block.box.y1 != block.layout_box.y1;
        if (block.crop_expanded) {
            auto& record = records[size_t(candidate->id)];
            record.crop_expanded = true;
            record.recognition_crop = block.box;
        }
        block.error = block.type == "unknown" ? "unknown_layout_class" : "recognition_not_executed";
        if (block.type == "table" && !transcribe) block.format_override = "markdown";
        block.resource = block_asset(block.id);
        result.assets.push_back({block.resource, crop_png(image, block.box)});
        if (block.type == "image") {
            block.status = "ok";
            block.error.clear();
            block.assessment.state = "ok";
            block.assessment.reason = "resource_saved";
        }
        audit.did_crop = true;
        blocks.push_back(std::move(block));
    }
    for (auto& evidence : ownership) {
        auto owner = std::find_if(blocks.begin(), blocks.end(), [&](const Block& block) {
            return block.candidate_id == evidence.owner_candidate_id;
        });
        if (owner == blocks.end()) throw std::runtime_error("content_owner_missing");
        evidence.owner_block_id = owner->id;
    }
    // All Region identities, crop bounds, ownership, groups and scheduling order
    // are frozen before the first recognition call. Transcription cannot reorder them.
    ReadingOrderEvidence order_evidence;
    const RegionStructure structure = arrange_structure(blocks, image.width, image.height, order_evidence);
    for (auto& block : blocks) {
        if (cancelled) { stop_after_cancel(); return audit; }
        if (transcribe && requires_ovis(block.type)) {
            const std::string request_id = "req" + block.region_id;
            RunResult::RegionRun region{request_id, "skipped", block.error, 0};
            if (requires_ovis(block.type)) {
                if (progress) progress("region_started", source_page ? source_page : 1,
                                       request_id, region_done, region_total);
                auto region_start = Clock::now();
                if (recognition_unavailable) {
                    block.status = "skipped";
                    block.error = "recognition_unavailable_after_backend_failure";
                    region.stop_reason = "backend_unavailable";
                } else try {
                    block.recognition = recognize_region(*backend, crop_rgb(image, block.box), block.box,
                        block.type, request_id, *plan, context);
                    region.recognition_json = recognition_json(block.recognition);
                    recognition_unavailable = block.recognition.backend_unavailable;
                    if (cancelled && block.recognition.attempts.empty()) {
                        region.status = "cancelled"; region.stop_reason = "cancelled_before_attempt";
                        audit.regions.push_back(std::move(region));
                        stop_after_cancel(); return audit;
                    }
                    audit.reset_failed |= block.recognition.reset_failed;
                    audit.did_reset |= !block.recognition.reset_failed && !block.recognition.attempts.empty();
                    audit.did_recognition |= !block.recognition.reset_failed && !block.recognition.attempts.empty();
                    {
                        auto* generation = block.recognition.attempts.empty() ? nullptr :
                            &block.recognition.attempts.back().output;
                        if (cancelled && generation && generation->finish_reason == "failed" &&
                            generation->stop_reason == "cancelled") {
                            region.status = "cancelled";
                            region.stop_reason = generation->stop_reason;
                            region.elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - region_start).count());
                            audit.recognition_ms += region.elapsed_ms;
                            audit.regions.push_back(std::move(region));
                            stop_after_cancel();
                            return audit;
                        }
                        if (!generation) {
                            recognition_unavailable = true;
                            block.status = "failed"; block.error = "generation_response_type_mismatch";
                            region.stop_reason = "error";
                        } else {
                            block.visual = *generation;
                            block.visual.raw_output.clear();
                            region.stop_reason = generation->stop_reason;
                            region.elapsed_ms = generation->elapsed_ms;
                            if (valid_utf8(generation->raw_output)) block.raw = generation->raw_output;
                            else block.raw_base64 = base64(generation->raw_output);
                            if (valid_utf8(generation->text)) block.text = generation->text;
                            else block.text_base64 = base64(generation->text);
                            if (!valid_utf8(generation->error)) block.error_base64 = base64(generation->error);
                            block.assessment = assess_output(*generation, block.type);
                            if (block.assessment.state != "ok") {
                                block.status = assessment_block_status(block.assessment.state);
                                block.error = block.assessment.reason;
                                block.format_override = "markdown";
                                if (block.error == "visual_evidence_missing") block.text.clear();
                            } else if (generation->finish_reason == "complete" &&
                                !generation->raw_output.empty() && !generation->text.empty() &&
                                valid_utf8(generation->raw_output) && valid_utf8(generation->text)) {
                                if (block.type == "text") {
                                    if (valid_text_math(block.text)) {
                                        block.status = "ok"; block.error.clear();
                                    } else {
                                        block.status = "partial";
                                        block.error = "invalid_inline_formula_syntax";
                                    }
                                } else if (block.type == "formula") {
                                    const auto formula = parse_formula_region(block.text);
                                    if (formula.valid) {
                                        block.status = "ok"; block.error.clear();
                                        block.text = formula.text;
                                        block.format_override = formula.format;
                                        block.display_formula = formula.display;
                                    } else {
                                        block.status = "partial";
                                        block.error = "invalid_formula_syntax";
                                        block.format_override = "markdown";
                                    }
                                } else {
                                    block.table = parse_table(block.text);
                                    if (valid_table_math_content(block.table)) {
                                        block.status = "ok"; block.error.clear();
                                        block.text = block.table.html;
                                    } else {
                                        block.status = "partial";
                                        block.error = block.table.valid ? "invalid_table_formula_syntax" : "invalid_table_structure";
                                        block.format_override = "markdown";
                                    }
                                }
                            } else if (generation->finish_reason == "truncated") {
                                block.status = "partial"; block.error = "ovis_token_limit";
                                block.format_override = "markdown";
                            } else {
                                block.status = "failed";
                                block.error = !valid_utf8(generation->error) ? "invalid_backend_error_utf8" :
                                    generation->error.empty() ?
                                    (generation->text.empty() ? "ovis_empty_output" : "ovis_inference_failed") :
                                    generation->error;
                            }
                            if (!valid_utf8(generation->raw_output) || !valid_utf8(generation->text)) {
                                block.status = "failed"; block.error = "invalid_backend_utf8";
                                block.text.clear();
                            }
                            if (!valid_utf8(generation->error)) {
                                block.status = "failed"; block.error = "invalid_backend_error_utf8";
                            }
                            if (block.recognition.backend_unavailable && !block.recognition.reset_failed &&
                                generation->error != "generation_response_type_mismatch")
                                block.error = "region_inference_exception:" + (valid_utf8(generation->error) ?
                                    generation->error : "invalid_utf8");
                            if (block.status == "failed" ||
                                (block.status != "ok" && block.assessment.state == "ok"))
                                block.assessment.state = block.status == "failed" ? "failed" : "unverified";
                            if (!block.error.empty()) block.assessment.reason = block.error;
                        }
                    }
                } catch (const std::bad_alloc&) { throw; }
                  catch (const std::exception& e) {
                    recognition_unavailable = true;
                    block.status = "failed";
                    std::string detail = e.what();
                    if (valid_utf8(detail)) block.error = "region_inference_exception:" + detail;
                    else {
                        block.error = "region_inference_exception:invalid_utf8";
                        block.error_base64 = base64(detail);
                    }
                    region.stop_reason = "error";
                }
                region.elapsed_ms = std::max(region.elapsed_ms, uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-region_start).count()));
                if (block.assessment.state == "skipped") {
                    block.assessment.state = block.status;
                    block.assessment.reason = block.error;
                    block.assessment.stop_reason = region.stop_reason;
                }
                region.status = block.status;
                audit.recognition_ms += region.elapsed_ms;
                if (block.status == "failed" && first_region_error_code.empty()) {
                    first_region_error_code = block.error == "region_reset_failed" ? "region_reset_failed" :
                        block.error == "generation_response_type_mismatch" ? "generation_response_type_mismatch" :
                        region.stop_reason == "timeout" ? "region_backend_timeout" :
                        block.error.rfind("region_inference_exception:", 0) == 0 ?
                            "region_inference_exception" : "region_inference_failed";
                    first_region_error_message = block.error;
                }
                if (cancelled) {
                    if (block.status != "failed") {
                        region.status = "cancelled";
                        region.stop_reason = "cancelled_after_backend_call";
                    }
                    audit.regions.push_back(std::move(region));
                    stop_after_cancel();
                    return audit;
                }
            }
            audit.regions.push_back(std::move(region));
            if (requires_ovis(block.type)) {
                ++region_done;
                if (progress) progress("region_completed", source_page ? source_page : 1,
                                       request_id, region_done, region_total);
            }
        }
        if (transcribe && block.type == "table" && block.status != "ok") {
            block.text.clear();
            block.table = {};
            block.format_override = "markdown";
        }
        if (block.assessment.state == "skipped") block.assessment.reason = block.error;
    }
    auto semantic = associate_annotations(blocks, image.width, image.height);
    semantic.erase(std::remove_if(semantic.begin(), semantic.end(), [&](const SemanticRelation& relation) {
        if (relation.type != "caption_of") return false;
        auto target = std::find_if(blocks.begin(), blocks.end(), [&](const Block& b) { return b.id == relation.target_block_id; });
        return target != blocks.end() && target->type == "image";
    }), semantic.end());
    for (const auto& caption : structure.captions)
        semantic.push_back({"caption_of", caption.caption_block_id, caption.image_block_id,
                            "pre_recognition_geometry"});
    if (cancelled) { stop_after_cancel(); return audit; }
    const std::string overlay_name = page_asset("layout-overlay.png");
    result.assets.push_back({overlay_name, layout_overlay(image, records, masks,
                                                           &layout_input.transform,
                                                           &layout_input.preview)});
    bool incomplete = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) { return b.status != "ok"; });
    const std::string state = records.empty() ? "blank" :
        (blocks.empty() || incomplete) ? "partial" : "ok";
    start = Clock::now();
    if (progress) progress("export_started", source_page ? source_page : 1, "", region_done, region_total);
    const std::set<std::string> uncertain_captions(structure.ambiguous_caption_ids.begin(), structure.ambiguous_caption_ids.end());
    const LabelExportPolicy label_export = plan ? plan->label_export : LabelExportPolicy{};
    for (const auto& block : blocks) {
        if (!label_export.markdown_visible(block.original_class_id)) continue;
        if (!result.markdown.empty()) result.markdown += "\n\n";
        result.markdown += render(block, uncertain_captions.count(block.id), true);
    }
    if (!result.markdown.empty()) result.markdown += '\n';
    result.json = serialize(image, state, blocks, backend->profile(), &records,
                            overlay_name, ownership, true, &order_evidence, semantic,
                            &layout_input.transform, score_threshold, &structure, &label_export, candidate_reviews);
    audit.did_export = true;
    if (progress) progress("export_completed", source_page ? source_page : 1, "", region_done, region_total);
    audit.export_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
    uint64_t bytes = result.json.size() + result.markdown.size();
    for (const auto& asset : result.assets) bytes += asset.png.size();
    if (plan && bytes > plan->max_output_bytes) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "output_bytes";
        return audit;
    }
    audit.code = records.empty() ? RunCode::Blank :
        (blocks.empty() || incomplete) ? RunCode::Partial : RunCode::Ok;
    audit.output = std::move(result);
    return audit;
}
} // namespace

RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled,
                   const ExecutionPlan* plan, uint32_t source_page,
                   const ProgressCallback& progress) {
    PageScope page_scope(source_page);
    using Clock = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point from) {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count());
    };
    RunResult audit{RunCode::Failed, {}};
    auto start = Clock::now();
    Image image;
    const bool layout_job = plan && plan->uses_doclayout();
    if (!decode(input, image, layout_job ? max_layout_source_pixels : max_pixels))
        return {RunCode::InputError, {}};
    audit.did_decode = true;
    if (progress) progress("decode_completed", source_page ? source_page : 1, "", 0, 0);
    audit.page_pixels = uint64_t(image.width) * image.height;
    audit.decode_ms = elapsed(start);
    if (plan && audit.page_pixels > plan->max_page_pixels) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "input_pixels";
        return audit;
    }
    if (!backend) return {RunCode::Unsupported, {}};
    if (layout_job)
        return run_layout_only(backend, image, cancelled, plan, audit, source_page, progress);
    if (!backend->capabilities().tensor || !backend->capabilities().generation ||
        backend->capabilities().max_concurrent_requests < 1) return {RunCode::Unsupported, {}};
    if (cancelled) { audit.code = RunCode::Cancelled; return audit; }
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
    if (progress) progress("layout_started", source_page ? source_page : 1, "", 0, 0);
    auto response = backend->execute({"layout-" + page_id(), TensorRequest{{std::move(page_tensor)}, {"layout_candidates"}}}, context);
    audit.layout_ms = elapsed(start);
    if (cancelled) { audit.code = RunCode::Cancelled; return audit; }
    auto* layout = std::get_if<TensorOutput>(&response.payload);
    if (!layout) return {RunCode::Failed, {}};
    audit.did_layout = true;
    if (progress) progress("layout_completed", source_page ? source_page : 1, "", 0, 0);
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
    uint32_t region_total = 0, region_done = 0;
    for (const auto& candidate : candidates)
        if (candidate.label == "text" || candidate.label == "formula" || candidate.label == "table") ++region_total;
    std::string first_region_error_code, first_region_error_message;
    auto stop_after_cancel = [&] {
        audit.code = first_region_error_code.empty() ? RunCode::Cancelled : RunCode::Failed;
        audit.error_code = first_region_error_code;
        audit.error_message = first_region_error_message;
    };
    for (const auto& candidate : candidates) {
        if (cancelled) { stop_after_cancel(); return audit; }
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
            block.resource = block_asset(block.id);
            audit.did_crop = true;
            output.assets.push_back({block.resource, crop_png(image, block.box)});
        } else if (block.type == "text" || block.type == "formula" || block.type == "table") {
            const std::string request_id = "req" + block.region_id;
            if (progress) progress("region_started", source_page ? source_page : 1,
                                   request_id, region_done, region_total);
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
            if (cancelled) {
                bool malformed = !valid_utf8(generation.text) || !valid_utf8(generation.raw_output) ||
                                 !valid_utf8(generation.error);
                bool failed_response = generation.finish_reason != "complete" &&
                                       generation.finish_reason != "truncated" &&
                                       !(generation.finish_reason == "failed" &&
                                         generation.stop_reason == "cancelled");
                if (!local_error.empty() || malformed || failed_response) {
                    if (first_region_error_code.empty()) {
                        first_region_error_code = audit.reset_failed ? "region_reset_failed" :
                            local_error == "generation response type mismatch" ?
                                "generation_response_type_mismatch" :
                            !local_error.empty() ? "region_inference_exception" :
                            malformed ? "region_invalid_backend_utf8" :
                            generation.stop_reason == "timeout" ? "region_backend_timeout" :
                                "region_inference_failed";
                        first_region_error_message = !local_error.empty() ? local_error :
                            malformed ? "invalid backend UTF-8" :
                            generation.error.empty() ? "recognition incomplete" : generation.error;
                    }
                    audit.regions.push_back({request_id, "failed",
                        generation.stop_reason.empty() ? "backend_error" : generation.stop_reason, 0});
                } else audit.regions.push_back({request_id, "cancelled",
                    generation.stop_reason == "cancelled" ? "cancelled" :
                    "cancelled_after_backend_call", 0});
                stop_after_cancel();
                return audit;
            }
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
                block.resource = block_asset(block.id);
                audit.did_crop = true;
                output.assets.push_back({block.resource, crop_png(image, block.box)});
                partial = true;
            } else block.text = generation.text;
            if (block.status == "failed" && first_region_error_code.empty()) {
                first_region_error_code = audit.reset_failed ? "region_reset_failed" :
                    local_error == "generation response type mismatch" ?
                        "generation_response_type_mismatch" :
                    !local_error.empty() ? "region_inference_exception" :
                    generation.stop_reason == "timeout" ? "region_backend_timeout" :
                        "region_inference_failed";
                first_region_error_message = block.error;
            }
            ++region_done;
            if (progress) progress("region_completed", source_page ? source_page : 1,
                                   request_id, region_done, region_total);
        } else {
            block.status = "skipped";
            block.error = "unsupported layout label";
            block.resource = block_asset(block.id);
            audit.did_crop = true;
            output.assets.push_back({block.resource, crop_png(image, block.box)});
            partial = true;
        }
        blocks.push_back(std::move(block));
        if (cancelled) { stop_after_cancel(); return audit; }
    }
    if (cancelled) { stop_after_cancel(); return audit; }
    audit.recognition_ms = elapsed(start);
    start = Clock::now();
    if (progress) progress("export_started", source_page ? source_page : 1, "", region_done, region_total);
    std::string state = candidates.empty() ? "blank" : partial ? "partial" : "ok";
    for (const auto& block : blocks) {
        if (!output.markdown.empty()) output.markdown += "\n\n";
        output.markdown += render(block);
    }
    if (!output.markdown.empty()) output.markdown += '\n';
    output.json = serialize(image, state, blocks, backend->profile());
    audit.did_export = true;
    if (progress) progress("export_completed", source_page ? source_page : 1, "", region_done, region_total);
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
