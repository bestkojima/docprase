#include "reexport.hpp"
#include "markdown.hpp"
#include "layout_region_policy.hpp"
#include "output_assessment.hpp"
#include "content_validation.hpp"
#include "pdf_page_id.hpp"
#include "table_parser.hpp"
#include "schema_validator.hpp"
#include "config.hpp"
#include "json.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace dococr {
namespace {
using Json = nlohmann::json;
void require(bool condition, const std::string& where) {
    if (!condition) throw std::invalid_argument("DocumentIR 无效：" + where);
}
const Json& field(const Json& object, const char* name, const std::string& where) {
    require(object.is_object() && object.contains(name), where + "." + name + " 缺失");
    return object.at(name);
}
std::string string_field(const Json& object, const char* name, const std::string& where) {
    const Json& value = field(object, name, where);
    require(value.is_string(), where + "." + name + " 须为字符串");
    return value.get<std::string>();
}
const Json& array_field(const Json& object, const char* name, const std::string& where) {
    const Json& value = field(object, name, where);
    require(value.is_array(), where + "." + name + " 须为数组");
    return value;
}
void id(const std::string& value, const std::string& pattern, const std::string& where) {
    require(std::regex_match(value, std::regex(pattern)), where + " ID 无效：" + value);
}
void status(const std::string& value, bool block, const std::string& where) {
    require(value == "ok" || value == "partial" || (block && value == "skipped") ||
            (!block && value == "blank") || value == "failed", where + " 状态无效");
}
void box(const Json& value, int width, int height, const std::string& where) {
    require(value.is_array() && value.size() == 4, where + " bbox 无效");
    for (const auto& n : value) require(n.is_number_integer() && n.get<int64_t>() >= 0 &&
                                       n.get<int64_t>() <= INT_MAX,
                                       where + " bbox 坐标无效");
    require(value[0].get<int64_t>() < value[2].get<int64_t>() &&
            value[1].get<int64_t>() < value[3].get<int64_t>() &&
            value[2].get<int64_t>() <= width && value[3].get<int64_t>() <= height,
            where + " bbox 超出页面");
}
void asset_path(const std::string& path, const std::string& where) {
    require(path.rfind("assets/", 0) == 0 &&
            std::regex_match(path, std::regex("assets/[A-Za-z0-9._/-]+")),
            where + " 资源路径无效：" + path);
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string segment = path.substr(start, end - start);
        require(!segment.empty() && segment != "." && segment != "..",
                where + " 资源路径越界：" + path);
        start = end + 1;
    }
    require(path.back() != '/', where + " 资源路径无效：" + path);
}
void diagnostics(const Json& diag, std::set<std::string>& paths, const std::string& where) {
    if (diag.is_null()) return;
    const auto& count = field(diag, "candidate_count", where);
    const auto& candidates = array_field(diag, "candidates", where);
    require(count.is_number_integer() && count.get<int64_t>() >= 0 &&
            count.get<uint64_t>() == candidates.size(), where + " candidate_count 无效");
    asset_path(string_field(diag, "overlay_asset", where), where);
    paths.insert(diag.at("overlay_asset").get<std::string>());
    const Json& tensors = field(diag, "raw_tensor_assets", where);
    require(tensors.is_object(), where + ".raw_tensor_assets 无效");
    for (auto name : {"image", "im_shape", "scale_factor", "fetch_name_0", "fetch_name_1", "fetch_name_2"})
        require(tensors.contains(name) && tensors.at(name).is_string(),
                where + ".raw_tensor_assets 缺少 " + name);
    for (const auto& item : tensors.items()) {
        require(item.value().is_string(), where + ".raw_tensor_assets 路径无效");
        auto path = item.value().get<std::string>();
        asset_path(path, where);
        paths.insert(path);
    }
    for (const auto& candidate : candidates) {
        const auto& path = field(candidate, "mask_asset", where);
        require(path.is_null() || path.is_string(), where + ".mask_asset 无效");
        if (!path.is_null()) {
            asset_path(path.get<std::string>(), where);
            paths.insert(path.get<std::string>());
        }
    }
}
void structured_table(const Json& content, const std::string& where) {
    const auto& table = field(content, "table", where);
    require(table.is_object(), where + " table 结构缺失");
    ParsedTable parsed = parse_table(content.at("text").get<std::string>());
    require(parsed.valid && parsed.html == content.at("text") &&
            field(table, "rows", where) == parsed.rows &&
            field(table, "columns", where) == parsed.columns,
            where + " 表格 HTML 与结构不一致");
    const auto& cells = array_field(table, "cells", where);
    require(cells.size() == parsed.cells.size(), where + " 表格单元格数量不一致");
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        const auto& expected = parsed.cells[i];
        require(field(cell, "row", where) == expected.row &&
                field(cell, "column", where) == expected.column &&
                field(cell, "rowspan", where) == expected.rowspan &&
                field(cell, "colspan", where) == expected.colspan &&
                field(cell, "header", where) == expected.header &&
                field(cell, "text", where) == expected.text &&
                field(cell, "bbox", where).is_null(), where + " 表格单元格不一致");
    }
}
void finite_array(const Json& value, size_t count, const std::string& where) {
    require(value.is_array() && value.size() == count, where + " 数组长度无效");
    for (const auto& number : value)
        require(number.is_number() && std::isfinite(number.get<double>()), where + " 数值无效");
}
void pdf_geometry(const Json& page, const Json& source, const std::string& where) {
    if (page.contains("pdf_page_size_points")) {
        const auto& size = page.at("pdf_page_size_points");
        finite_array(size, 2, where + ".pdf_page_size_points");
        require(size[0].get<double>() > 0 && size[1].get<double>() > 0,
                where + " PDF 页面点尺寸无效");
    }
    if (page.contains("pdf_crop_box_points")) {
        const auto& crop = page.at("pdf_crop_box_points");
        finite_array(crop, 4, where + ".pdf_crop_box_points");
        require(crop[0].get<double>() < crop[2].get<double>() &&
                crop[1].get<double>() < crop[3].get<double>(), where + " PDF CropBox 无效");
    }
    if (page.contains("pdf_points_to_raster_affine"))
        finite_array(page.at("pdf_points_to_raster_affine"), 6,
                     where + ".pdf_points_to_raster_affine");
    if (page.contains("pdf_rotation_degrees")) {
        const auto& rotation = page.at("pdf_rotation_degrees");
        require(rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270,
                where + " PDF 旋转角度无效");
    }
    if (page.contains("raster_dpi"))
        require(page.at("raster_dpi") == source.at("dpi"), where + " raster_dpi 与 source 不符");
    if (page.contains("pdf_points_per_raster_pixel"))
        require(page.at("pdf_points_per_raster_pixel").is_number() &&
                page.at("pdf_points_per_raster_pixel").get<double>() > 0,
                where + " PDF 点像素比例无效");
    if (page.contains("estimated_raster_pixels"))
        require(page.at("estimated_raster_pixels").is_number_integer() &&
                page.at("estimated_raster_pixels").get<int64_t>() > 0,
                where + " estimated_raster_pixels 无效");
}
GenerationOutput saved_output(const Json& block) {
    const auto& provenance = block.at("provenance");
    GenerationOutput output;
    output.text = block.at("content").at("text").get<std::string>();
    if (provenance.at("raw_output").is_string())
        output.raw_output = provenance.at("raw_output").get<std::string>();
    if (provenance.contains("assessment")) {
        const auto& assessment = provenance.at("assessment");
        output.text = assessment.at("generation_text").is_string() ?
            assessment.at("generation_text").get<std::string>() : "";
        output.finish_reason = assessment.at("finish_reason").get<std::string>();
        output.stop_reason = assessment.at("stop_reason").get<std::string>();
        if (assessment.at("backend_error").is_string())
            output.error = assessment.at("backend_error").get<std::string>();
    } else {
        if (block.at("type") == "table" && block.at("status") != "ok") output.text = output.raw_output;
        output.finish_reason = block.at("status") == "failed" ? "failed" : "complete";
        output.stop_reason = provenance.contains("visual") ?
            provenance.at("visual").at("stop_reason").get<std::string>() : "normal";
        if (block.at("error") == "ovis_token_limit") output.stop_reason = "token_limit";
    }
    if (provenance.contains("visual")) {
        output.visual_evidence = provenance.at("visual").at("evidence").get<std::string>();
        output.visual_tokens = provenance.at("visual").at("token_count").get<uint32_t>();
    }
    return output;
}

std::string decode_saved_bytes(const Json& saved, const char* key) {
    if (saved.at(key).is_string()) return saved.at(key).get<std::string>();
    const auto encoded = saved.at(std::string(key) + "_base64").get<std::string>();
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    require(encoded.size() % 4 == 0, "尝试 Base64 长度无效");
    std::string result;
    uint32_t bits = 0;
    unsigned count = 0, padding = 0;
    for (char character : encoded) {
        if (character == '=') { ++padding; continue; }
        const auto value = alphabet.find(character);
        require(!padding && value != std::string::npos, "尝试 Base64 字符无效");
        bits = (bits << 6) | uint32_t(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            result.push_back(char((bits >> count) & 255));
        }
    }
    require(padding <= 2 && count == padding * 2 && (bits & ((1u << count) - 1)) == 0,
            "尝试 Base64 补位无效");
    return result;
}

GenerationOutput attempt_output(const Json& attempt) {
    const auto& saved = attempt.at("output");
    GenerationOutput output;
    output.text = decode_saved_bytes(saved, "text");
    output.raw_output = decode_saved_bytes(saved, "raw_output");
    output.error = decode_saved_bytes(saved, "error");
    output.finish_reason = saved.at("finish_reason").get<std::string>();
    output.stop_reason = saved.at("stop_reason").get<std::string>();
    output.visual_evidence = attempt.at("visual").at("evidence").get<std::string>();
    output.visual_tokens = attempt.at("visual").at("token_count").get<uint32_t>();
    return output;
}

void validate_recognition(const Json& block, const std::string& where) {
    const auto& provenance = block.at("provenance");
    const auto& recognition = provenance.at("recognition");
    const auto& attempts = recognition.at("attempts");
    uint64_t budget = 0, generation_ms = 0;
    require(attempts.empty() ? recognition.at("selected_attempt").is_null() :
        recognition.at("selected_attempt") == attempts.size(), where + " 最终采用尝试无效");
    for (size_t i = 0; i < attempts.size(); ++i) {
        const auto& attempt = attempts[i];
        const auto& config = attempt.at("config");
        const auto& output = attempt.at("output");
        require(attempt.at("index") == i + 1, where + " 尝试序号无效");
        require(output.at("generation_elapsed_ms").get<uint64_t>() <= attempt.at("elapsed_ms").get<uint64_t>(),
                where + " 生成耗时超过尝试耗时");
        for (const auto* key : {"text", "raw_output", "error"}) {
            const auto& encoded = output.at(std::string(key) + "_base64");
            require(output.at(key).is_null() != encoded.is_null(), where + " 尝试输出编码无效");
            (void)decode_saved_bytes(output, key);
        }
        if (attempt.at("input_visual").is_object()) {
            const auto& input = attempt.at("input_visual");
            const auto& canvas = input.at("canvas_size");
            const auto& content = input.at("content_size");
            const auto& pad = input.at("pad_offset");
            const auto& bbox = block.at("bbox");
            const int w = bbox[2].get<int>() - bbox[0].get<int>(), h = bbox[3].get<int>() - bbox[1].get<int>();
            const int cw = canvas[0].get<int>(), ch = canvas[1].get<int>();
            const auto pixels = int64_t(cw) * ch;
            const double scale = input.at("scale").get<double>();
            require(cw > 0 && ch > 0 && cw % 32 == 0 && ch % 32 == 0 &&
                pixels >= config.at("visual_min_pixels").get<int64_t>() &&
                pixels <= config.at("visual_max_pixels").get<int64_t>() &&
                std::abs(scale - std::min(double(cw) / w, double(ch) / h)) < 1e-9 &&
                content[0] == std::round(w * scale) && content[1] == std::round(h * scale) &&
                pad[0] == (cw - content[0].get<int>()) / 2 && pad[1] == (ch - content[1].get<int>()) / 2 &&
                std::abs(input.at("rounding_error")[0].get<double>() - (content[0].get<int>() - w * scale)) < 1e-9 &&
                std::abs(input.at("rounding_error")[1].get<double>() - (content[1].get<int>() - h * scale)) < 1e-9,
                where + " 计划视觉输入变换无效");
            if (attempt.at("visual").at("scale").get<double>() > 0)
                for (const auto& item : input.items())
                    require(item.value() == attempt.at("visual").at(item.key()), where + " 实际视觉变换与计划不一致");
        }
        budget += config.at("generation_timeout_ms").get<uint64_t>();
        generation_ms += output.at("generation_elapsed_ms").get<uint64_t>();
        if (i == 0) {
            require(attempt.at("correction") == "initial" && config.at("visual_min_pixels") == 65536 &&
                config.at("visual_max_pixels") == 313600, where + " 首次尝试配置无效");
        } else {
            const auto& previous = attempts[i - 1];
            const auto& before = previous.at("config");
            const auto first = attempt_output(previous);
            const auto assessment = assess_output(first, block.at("type").get<std::string>());
            require(config.at("generation_timeout_ms") == before.at("generation_timeout_ms"),
                    where + " 重试生成时间预算改变");
            if (attempt.at("correction") == "increase_token_budget")
                require(assessment.state == "incomplete" && first.stop_reason == "token_limit" &&
                    before.at("max_new_tokens").get<uint64_t>() < 4096 &&
                    config.at("max_new_tokens") == 4096 && config.at("visual_min_pixels") == before.at("visual_min_pixels") &&
                    config.at("visual_max_pixels") == before.at("visual_max_pixels"), where + " token 重试修正无效");
            else if (attempt.at("correction") == "increase_visual_resolution")
                require(first.stop_reason == "vision_missing" && first.visual_evidence == "no_visual_tokens" &&
                    first.visual_tokens == 0 && config.at("max_new_tokens") == before.at("max_new_tokens") &&
                    config.at("visual_min_pixels") == 262144 && config.at("visual_max_pixels") == 1120000 &&
                    attempt.at("input_visual").is_object() && previous.at("input_visual").is_object() &&
                    attempt.at("input_visual").at("scale").get<double>() > previous.at("input_visual").at("scale").get<double>() &&
                    attempt.at("input_visual").at("canvas_size") != previous.at("input_visual").at("canvas_size"),
                    where + " 视觉重试没有有效输入修正");
            else require(false, where + " 重试修正缺失");
        }
    }
    require(recognition.at("configured_generation_budget_ms") == budget && budget <= 240000 &&
        recognition.at("generation_elapsed_ms") == generation_ms, where + " 尝试预算或耗时汇总无效");
    if (attempts.empty()) {
        require(block.at("status") == "skipped" || provenance.at("assessment").at("finish_reason") == "",
                where + " 已执行结果缺少尝试记录");
        return;
    }
    const auto& selected = attempts.back();
    const auto& output = selected.at("output");
    const auto& assessment = provenance.at("assessment");
    require(output.at("raw_output") == provenance.at("raw_output") &&
        output.at("raw_output_base64") == provenance.at("raw_output_base64") &&
        output.at("text") == assessment.at("generation_text") &&
        output.at("text_base64") == provenance.at("text_base64") &&
        output.at("finish_reason") == assessment.at("finish_reason") &&
        output.at("stop_reason") == assessment.at("stop_reason"), where + " 最终结果与采用尝试不一致");
    const Json error = output.at("error").is_string() && output.at("error") != "" ? output.at("error") : Json(nullptr);
    require(error == assessment.at("backend_error") && output.at("error_base64") == block.at("error_base64"),
            where + " 后端错误与采用尝试不一致");
    if (provenance.contains("visual")) {
        for (const auto& item : selected.at("visual").items())
            require(item.value() == provenance.at("visual").at(item.key()), where + " 视觉证据与采用尝试不一致");
    } else require(selected.at("visual").at("evidence") == "", where + " 最终视觉证据缺失");
}
std::string render_page_markdown(const Json& page,
                                 const std::unordered_map<std::string, const Json*>& by_id) {
    std::string markdown;
    const auto& order = page.contains("structure_plan") ? page.at("structure_plan").at("block_order") : page.at("reading_order");
    std::set<std::string> uncertain_captions;
    if (page.contains("structure_plan"))
        for (const auto& bid : page.at("structure_plan").at("ambiguous_caption_ids"))
            uncertain_captions.insert(bid.get<std::string>());
    for (const auto& value : order) {
        const auto& block = *by_id.at(value.get<std::string>());
        const auto& content = block.at("content");
        const auto& resource = content.at("resource");
        const std::string type = block.at("type").get<std::string>();
        MarkdownBlock render{value.get<std::string>(), type,
            block.at("status").get<std::string>(), content.at("text").get<std::string>(),
            resource.is_null() ? "" : resource.get<std::string>(), content.value("display", true),
            type == "table" && block.at("status") == "ok"};
        if (block.at("provenance").contains("assessment")) {
            render.assessment_state = block.at("provenance").at("assessment").at("state").get<std::string>();
            render.assessment_reason = block.at("provenance").at("assessment").at("reason").get<std::string>();
        } else if (type == "text" || type == "formula" || type == "table") {
            const auto assessment = assess_legacy_output(saved_output(block), type);
            if (assessment.state == "anomalous" || assessment.state == "incomplete" ||
                (assessment.state == "unverified" && block.at("status") == "ok")) {
                render.status = "partial";
                render.assessment_state = assessment.state;
                render.assessment_reason = assessment.reason;
            }
        }
        if (!markdown.empty()) markdown += "\n\n";
        markdown += render_markdown_block(render, uncertain_captions.count(render.id));
    }
    if (!markdown.empty()) markdown += '\n';
    return markdown;
}

void validate_structure(const Json& page, const std::unordered_map<std::string, const Json*>& blocks,
                        int width, int height, const std::string& where) {
    const auto& plan = page.at("structure_plan");
    require(plan.at("block_order") == page.at("reading_order"), where + " structure_plan 与阅读顺序不一致");
    Json region_order = Json::array();
    std::map<std::string, size_t> positions;
    size_t position = 0;
    for (const auto& bid : plan.at("block_order")) {
        const auto key = bid.get<std::string>();
        require(blocks.count(key), where + " structure_plan block 不存在");
        positions[key] = position++;
        for (const auto& rid : blocks.at(key)->at("source_region_ids")) region_order.push_back(rid);
    }
    require(region_order == plan.at("region_order"), where + " structure_plan Region 调度顺序不一致");
    std::set<std::string> planned_regions;
    for (const auto& rid : region_order)
        require(planned_regions.insert(rid.get<std::string>()).second, where + " structure_plan Region 重复");
    require(planned_regions.size() == page.at("regions").size(), where + " structure_plan 未覆盖所有 Region");
    if (plan.contains("recognition_order")) {
        Json recognition_order = Json::array();
        for (const auto& bid : plan.at("block_order")) {
            const auto& block = *blocks.at(bid.get<std::string>());
            if (requires_ovis(block.at("type").get<std::string>()))
                for (const auto& rid : block.at("source_region_ids")) recognition_order.push_back(rid);
        }
        require(recognition_order == plan.at("recognition_order"), where + " 实际识别顺序包含资源或遗漏 OCR 区域");
    }
    std::map<std::string, std::string> captions;
    std::set<std::string> caption_ids;
    for (const auto& pair : plan.at("captions")) {
        const auto image = pair.at("image_block_id").get<std::string>();
        const auto caption = pair.at("caption_block_id").get<std::string>();
        require(blocks.count(image) && blocks.count(caption) && blocks.at(image)->at("type") == "image" &&
                blocks.at(caption)->at("type") == "text" && captions.emplace(image, caption).second &&
                caption_ids.insert(caption).second, where + " structure_plan 图注引用/类型/唯一性无效");
        require(positions.at(caption) == positions.at(image) + 1, where + " structure_plan 图文不相邻");
    }
    std::set<std::pair<std::string, std::string>> saved_links;
    for (const auto& relation : page.at("relations"))
        if (relation.at("type") == "caption_of" && blocks.at(relation.at("target_block_id").get<std::string>())->at("type") == "image")
            require(saved_links.emplace(relation.at("target_block_id").get<std::string>(),
                         relation.at("source_block_id").get<std::string>()).second, where + " 图注关系重复");
    std::set<std::pair<std::string, std::string>> planned_links(captions.begin(), captions.end());
    require(saved_links == planned_links, where + " structure_plan 与图注关系不一致");
    std::set<std::string> group_ids, grouped_images;
    for (const auto& group : plan.at("groups")) {
        require(group_ids.insert(group.at("id").get<std::string>()).second, where + " structure_plan group ID 重复");
        box(group.at("bbox"), width, height, where + ".structure_plan.groups");
        const auto& bounds = group.at("bbox");
        int previous_right = -1;
        size_t next_position = 0;
        bool first = true;
        for (const auto& item : group.at("items")) {
            const auto image = item.at("image_block_id").get<std::string>();
            require(blocks.count(image) && blocks.at(image)->at("type") == "image" && grouped_images.insert(image).second,
                    where + " structure_plan 组图片无效或重复");
            const auto& bbox = blocks.at(image)->at("bbox");
            require(bbox[0].get<int>() >= previous_right, where + " structure_plan 组内图片未按左右顺序");
            previous_right = bbox[2].get<int>();
            if (first) { next_position = positions.at(image); first = false; }
            require(positions.at(image) == next_position++, where + " structure_plan 组内顺序不连续");
            require(bounds[0] <= bbox[0] && bounds[1] <= bbox[1] && bounds[2] >= bbox[2] && bounds[3] >= bbox[3],
                    where + " structure_plan 组范围未覆盖图片");
            if (!item.at("caption_block_id").is_null()) {
                const auto caption = item.at("caption_block_id").get<std::string>();
                require(captions.count(image) && captions.at(image) == caption && positions.at(caption) == next_position++,
                        where + " structure_plan 组图注与计划不一致");
                const auto& caption_bounds = blocks.at(caption)->at("bbox");
                require(bounds[0] <= caption_bounds[0] && bounds[1] <= caption_bounds[1] &&
                        bounds[2] >= caption_bounds[2] && bounds[3] >= caption_bounds[3],
                        where + " structure_plan 组范围未覆盖图注");
            } else require(!captions.count(image), where + " structure_plan 组遗漏图注");
        }
    }
    for (const auto& value : plan.at("ambiguous_caption_ids")) {
        const auto bid = value.get<std::string>();
        require(blocks.count(bid) && blocks.at(bid)->at("type") == "text" && !caption_ids.count(bid),
                where + " structure_plan 不确定图注已被绑定或引用无效");
    }
}
}

ReexportDocument validate_and_render_document(const std::string& json) {
    Json document;
    std::vector<std::set<std::string>> keys;
    auto reject_duplicates = [&keys](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("JSON 对象键重复：" + parsed.get<std::string>());
        return true;
    };
    try { document = Json::parse(json, reject_duplicates); }
    catch (const std::exception& error) { throw std::invalid_argument(std::string("JSON 解析失败：") + error.what()); }
    const auto version = string_field(document, "schema_version", "document");
    require(version == "1.0" || version == "1.1" || version == "1.2" ||
            version == "1.3" || version == "1.4" || version == "1.5" || (version == "1.6" || version == "1.7" || (version == "1.8" || version == "1.9")), "不支持 schema_version " + version);
    validate_document_schema(document, version);
    const bool pdf = document.at("source").at("type") == "pdf";
    id(string_field(document, "document_id", "document"), "doc-[0-9a-f]{16}", "document_id");
    status(string_field(document, "status", "document"), false, "document");
    require(string_field(field(document, "source", "document"), "type", "source") ==
            (pdf ? "pdf" : "image"), "source.type 与版本不符");
    const auto& source = document.at("source");
    const auto& pages = array_field(document, "pages", "document");
    require(!pages.empty() && (pdf || pages.size() == 1), "pages 数量无效");
    if (pdf) {
        id(string_field(source, "sha256", "source"), "[0-9a-f]{64}", "source.sha256");
        require(string_field(source, "renderer", "source") == "poppler", "PDF renderer 无效");
        string_field(source, "renderer_version", "source");
        const auto& dpi = field(source, "dpi", "source");
        require(dpi.is_number_integer() && dpi.get<int64_t>() >= 36 && dpi.get<int64_t>() <= 600,
                "PDF dpi 无效");
        const auto& selection = array_field(source, "selected_pages", "source");
        require(selection.size() == 2 && selection[0].is_number_integer() &&
                selection[1].is_number_integer() && selection[0].get<int64_t>() >= 1 &&
                selection[1].get<int64_t>() >= selection[0].get<int64_t>() &&
                selection[1].get<int64_t>() - selection[0].get<int64_t>() + 1 ==
                    static_cast<int64_t>(pages.size()), "PDF selected_pages 与页面数不符");
        const auto& count = field(source, "page_count", "source");
        require(count.is_number_integer() && count.get<int64_t>() >= selection[1].get<int64_t>(),
                "PDF page_count/selected_pages 无效");
    }
    const auto& resources = array_field(document, "resources", "document");
    std::set<std::string> all_blocks, all_paths, resource_paths, resource_ids, page_ids;
    std::unordered_map<std::string, const Json*> all_block_data;
    ReexportDocument result;
    for (size_t p = 0; p < pages.size(); ++p) {
        const auto& page = pages[p];
        const std::string where = "pages[" + std::to_string(p) + "]";
        const std::string page_id = string_field(page, "page_id", where);
        id(page_id, "p[0-9]{4,}", where);
        require(page_ids.insert(page_id).second, where + " 重复 page_id");
        const auto& index = field(page, "page_index", where);
        require(index.is_number_unsigned() || (index.is_number_integer() && index.get<int64_t>() >= 0),
                where + ".page_index 无效");
        require(index.get<uint64_t>() < UINT32_MAX, where + " page_index 超出范围");
        require(page_id == (pdf ? pdf_page_id(static_cast<uint32_t>(index.get<uint64_t>() + 1)) : "p0001"),
                where + " page_id 与原页不符");
        if (!pdf) require(index == 0, where + " page_index 无效");
        if (pdf) require(field(page, "pdf_page_number", where).is_number_integer() &&
                         page.at("pdf_page_number") == index.get<uint64_t>() + 1 &&
                         page.at("pdf_page_number") == source.at("selected_pages")[0].get<int64_t>() +
                             static_cast<int64_t>(p),
                         where + " pdf_page_number/selected_pages 与 page_index 不符");
        if (pdf) pdf_geometry(page, source, where);
        status(string_field(page, "status", where), false, where);
        if (pdf) {
            const auto& error = field(page, "error", where);
            require(page.at("status") == "failed" ? error.is_object() : error.is_null(),
                    where + " 页面 error 与状态不符");
        }
        require(string_field(page, "coordinate_space", where) == "raster_page", where + " 坐标系无效");
        const auto& size = field(page, "raster_size", where);
        const bool failed = page.at("status") == "failed";
        require((failed && size.is_null()) || (size.is_array() && size.size() == 2 &&
            size[0].is_number_integer() && size[1].is_number_integer() &&
            size[0].get<int64_t>() > 0 && size[1].get<int64_t>() > 0 &&
            size[0].get<int64_t>() <= INT_MAX && size[1].get<int64_t>() <= INT_MAX), where + " raster_size 无效");
        int width = size.is_null() ? 0 : size[0].get<int>();
        int height = size.is_null() ? 0 : size[1].get<int>();
        std::set<std::string> layouts, regions, blocks;
        std::unordered_map<std::string, const Json*> region_data;
        const auto layout_pattern = pdf ? page_id + "-l[0-9]{4,}" : "l[0-9]{4,}";
        const auto region_pattern = pdf ? page_id + "-r[0-9]{4,}" : "r[0-9]{4,}";
        const auto block_pattern = pdf ? page_id + "-b[0-9]{4,}" : "b[0-9]{4,}";
        for (const auto& layout : array_field(page, "layout_blocks", where)) {
            auto value = string_field(layout, "id", where + ".layout_blocks");
            id(value, layout_pattern, where);
            require(layouts.insert(value).second && string_field(layout, "page_id", where) == page_id,
                    where + " layout ID/page_id 无效");
            box(field(layout, "bbox", where), width, height, where + ".layout_blocks");
            require(string_field(layout, "coordinate_space", where) == "raster_page", where + " layout 坐标系无效");
        }
        for (const auto& region : array_field(page, "regions", where)) {
            auto value = string_field(region, "id", where + ".regions");
            id(value, region_pattern, where);
            require(regions.insert(value).second && string_field(region, "page_id", where) == page_id,
                    where + " region ID/page_id 无效");
            region_data[value] = &region;
            box(field(region, "bbox", where), width, height, where + ".regions");
            require(string_field(region, "coordinate_space", where) == "raster_page", where + " region 坐标系无效");
            const auto& sources = array_field(region, "source_layout_block_ids", where);
            require(!sources.empty(), where + " source_layout_block_ids 为空");
            for (const auto& source : sources)
                require(source.is_string() && layouts.count(source.get<std::string>()), where + " region 源版面块不存在");
        }
        std::unordered_map<std::string, const Json*> by_id;
        if (pdf && failed)
            require(page.at("layout_blocks").empty() && page.at("regions").empty() &&
                    page.at("blocks").empty() && page.at("reading_order").empty() &&
                    page.at("relations").empty(), where + " 失败页不能含伪造块或关系");
        for (const auto& block : array_field(page, "blocks", where)) {
            auto value = string_field(block, "id", where + ".blocks");
            id(value, block_pattern, where);
            require(blocks.insert(value).second && all_blocks.insert(value).second &&
                    string_field(block, "page_id", where) == page_id, where + " block ID/page_id 无效");
            by_id[value] = &block;
            all_block_data[value] = &block;
            box(field(block, "bbox", where), width, height, where + ".blocks");
            require(string_field(block, "coordinate_space", where) == "raster_page", where + " block 坐标系无效");
            status(string_field(block, "status", where), true, where + ".blocks");
            require(field(block, "confidence", where).is_null(), where + " confidence 须为 null");
            const auto& sources = array_field(block, "source_region_ids", where);
            require(!sources.empty(), where + " source_region_ids 为空");
            for (const auto& source : sources)
                require(source.is_string() && regions.count(source.get<std::string>()), where + " block 源区域不存在");
            const auto& content = field(block, "content", where);
            require(field(content, "text", where).is_string(), where + " content.text 无效");
            auto type = string_field(block, "type", where);
            require(type == "text" || type == "formula" || type == "table" ||
                    type == "image" || type == "unknown", where + " block.type 无效");
            if (version == "1.9")
                for (const auto& rid : sources)
                    require(region_data.at(rid.get<std::string>())->at("recognition_type") == type,
                            where + " Region 识别类型与输出块不一致");
            auto format = string_field(content, "format", where);
            require(format == "markdown" || format == "latex" || format == "html" ||
                    format == "resource", where + " content.format 无效");
            if (type == "image" || type == "unknown")
                require(format == "resource", where + " 图片/未知块 format 无效");
            if (type == "text") require(format == "markdown", where + " 文本 format 无效");
            if (type == "formula" && block.at("status") == "ok")
                require(format == "latex" && field(content, "display", where).is_boolean(),
                        where + " 公式格式无效");
            if (type == "table" && content.contains("table") && content.at("table").is_object())
                require(format == "html", where + " 表格 format 无效");
            if (type == "table" && (version == "1.2" || version == "1.3" || version == "1.5" || (version == "1.6" || version == "1.7" || (version == "1.8" || version == "1.9")) || pdf)) {
                if (block.at("status") == "ok") structured_table(content, where);
                else require(field(content, "table", where).is_null() &&
                             content.at("text") == "" && format == "markdown",
                             where + " 非完整表格须保留空占位");
            }
            if (type == "table" && (version == "1.0" || version == "1.1") &&
                block.at("status") == "ok") {
                const auto table = parse_table(content.at("text").get<std::string>());
                require(format == "html" && table.valid && table.html == content.at("text"),
                        where + " 旧版表格 HTML 不安全或无效");
            }
            require(field(content, "resource", where).is_null() || field(content, "resource", where).is_string(),
                    where + " content.resource 无效");
            if (type == "image" || block.at("status") != "ok")
                require(content.at("resource").is_string(), where + " 图片/占位块缺少 resource");
            const auto& provenance = field(block, "provenance", where);
            if (version == "1.9" && type == "image") {
                const auto& recognition = provenance.at("recognition");
                const auto& assessment = provenance.at("assessment");
                require(recognition.at("attempts").empty() && recognition.at("selected_attempt").is_null() &&
                        recognition.at("configured_generation_budget_ms") == 0 && recognition.at("generation_elapsed_ms") == 0 &&
                        !provenance.contains("visual") && assessment.at("finish_reason") == "" &&
                        assessment.at("stop_reason") == "" && content.at("text") == "" &&
                        provenance.at("raw_output") == "" && provenance.at("raw_output_base64").is_null() &&
                        provenance.at("text_base64").is_null(), where + " 图片资源不可携带 Ovis 识别结果");
                if (block.at("status") == "ok")
                    require(assessment.at("reason") == "resource_saved" && block.at("error").is_null(),
                            where + " 图片资源保存状态无效");
            }
            if ((version == "1.5" || (version == "1.6" || version == "1.7" || (version == "1.8" || version == "1.9"))) && block.at("status") == "ok" &&
                (type == "text" || type == "formula" || type == "table"))
                require(provenance.contains("visual"), where + " 正常识别缺少视觉证据");
            const auto& raw = field(provenance, "raw_output", where);
            require(raw.is_null() || raw.is_string(),
                    where + " provenance.raw_output 无效");
            string_field(provenance, "model_profile", where);
            string_field(provenance, "request_id", where);
            for (auto name : {"raw_output_base64", "text_base64"}) {
                const auto& value = field(provenance, name, where);
                require(value.is_null() || value.is_string(), where + " provenance base64 字段无效");
            }
            if (provenance.contains("visual")) {
                const auto& visual = provenance.at("visual");
                const auto evidence = string_field(visual, "evidence", where);
                const auto tokens = field(visual, "token_count", where);
                require(tokens.is_number_unsigned() || (tokens.is_number_integer() && tokens.get<int64_t>() >= 0),
                        where + " 视觉 token 数无效");
                require(string_field(visual, "source_crop", where) == content.at("resource"),
                        where + " 原裁图资源不一致");
                require(field(visual, "source_bbox", where) == block.at("bbox"),
                        where + " 原裁图定位不一致");
                const auto& canvas = field(visual, "canvas_size", where);
                const auto& size = field(visual, "content_size", where);
                const auto& offset = field(visual, "pad_offset", where);
                require(canvas.is_array() && size.is_array() && offset.is_array() &&
                        canvas.size() == 2 && size.size() == 2 && offset.size() == 2,
                        where + " 视觉画布结构无效");
                for (int axis = 0; axis < 2; ++axis) {
                    require(canvas[axis].is_number_integer() && size[axis].is_number_integer() &&
                            offset[axis].is_number_integer() && canvas[axis].get<int>() >= 0 &&
                            size[axis].get<int>() >= 0 && offset[axis].get<int>() >= 0 &&
                            size[axis].get<int>() + offset[axis].get<int>() <= canvas[axis].get<int>(),
                            where + " 视觉变换超出画布");
                }
                const auto& rounding = field(visual, "rounding_error", where);
                const auto& affine = field(visual, "canvas_to_page_affine", where);
                const auto& scale_value = field(visual, "scale", where);
                require(rounding.is_array() && rounding.size() == 2 &&
                        affine.is_array() && affine.size() == 6 &&
                        scale_value.is_number() && scale_value.get<double>() >= 0,
                        where + " 视觉比例与回映结构无效");
                const double scale = scale_value.get<double>();
                const auto& bbox = block.at("bbox");
                for (int axis = 0; axis < 2; ++axis) {
                    require(rounding[axis].is_number() &&
                            std::abs(rounding[axis].get<double>()) <= .500001 &&
                            std::abs(size[axis].get<double>() -
                                (bbox[axis + 2].get<double>() - bbox[axis].get<double>()) * scale -
                                rounding[axis].get<double>()) < 1e-6,
                            where + " 视觉取整误差不一致");
                }
                for (const auto& component : affine)
                    require(component.is_number(), where + " 页面回映无效");
                const double inverse_x = size[0].get<int>() > 0 ?
                    double(bbox[2].get<int>() - bbox[0].get<int>()) / size[0].get<int>() : 0;
                const double inverse_y = size[1].get<int>() > 0 ?
                    double(bbox[3].get<int>() - bbox[1].get<int>()) / size[1].get<int>() : 0;
                const double expected[] = {inverse_x, 0, bbox[0].get<double>() - offset[0].get<double>() * inverse_x,
                                           0, inverse_y, bbox[1].get<double>() - offset[1].get<double>() * inverse_y};
                for (int axis = 0; axis < 6; ++axis)
                    require(std::abs(affine[axis].get<double>() - expected[axis]) < 1e-6,
                            where + " 页面回映与原裁图不一致");
                if (evidence == "image_pad_tokens" || evidence == "explicit_success")
                    require((evidence == "image_pad_tokens" ? tokens.get<uint64_t>() > 0 :
                             tokens.get<uint64_t>() == 0) && canvas[0].get<int>() >= 32 &&
                            canvas[1].get<int>() >= 32 && canvas[0].get<int>() % 32 == 0 &&
                            canvas[1].get<int>() % 32 == 0 &&
                            int64_t(canvas[0].get<int>()) * canvas[1].get<int>() >= 65536 &&
                            int64_t(canvas[0].get<int>()) * canvas[1].get<int>() <= 16777216,
                            where + " 有效视觉证据或画布无效");
                else require(block.at("status") != "ok" && tokens.get<uint64_t>() == 0,
                             where + " 无视觉输入不可标记成功");
            }
            const auto& error = field(block, "error", where);
            const auto& error_base64 = field(block, "error_base64", where);
            require((error.is_null() || error.is_string()) &&
                    (error_base64.is_null() || error_base64.is_string()),
                    where + " error 字段无效");
            if ((version == "1.6" || version == "1.7" || (version == "1.8" || version == "1.9"))) {
                if (version == "1.7" || (version == "1.8" || version == "1.9")) validate_recognition(block, where);
                const auto& assessment = provenance.at("assessment");
                const auto state = assessment.at("state").get<std::string>();
                require(assessment_block_status(state) == block.at("status"), where + " 判定状态与块状态不一致");
                const auto output = saved_output(block);
                if (provenance.contains("visual"))
                    require(output.stop_reason == provenance.at("visual").at("stop_reason"),
                            where + " 停止原因与视觉记录不一致");
                const bool encoded = !provenance.at("raw_output_base64").is_null() ||
                    !provenance.at("text_base64").is_null() || !error_base64.is_null();
                const auto derived = assess_output(output, type);
                if (!output.finish_reason.empty() && !encoded) {
                    require(assessment.at("nonempty_lines") == derived.nonempty_lines &&
                        assessment.at("empty_number_lines") == derived.empty_number_lines &&
                        assessment.at("repeat_offset") == derived.repeat_offset &&
                        assessment.at("repeat_unit_bytes") == derived.repeat_unit_bytes &&
                        assessment.at("repeat_count") == derived.repeat_count &&
                        assessment.at("evidence_source") == derived.evidence_source &&
                        std::abs(assessment.at("repeat_coverage").get<double>() - derived.repeat_coverage) < 1e-9,
                        where + " 判定依据与原始输出不一致");
                    const auto reason = assessment.at("reason").get<std::string>();
                    const bool structural = state == "unverified" && derived.state == "ok" &&
                        (reason == "invalid_formula_syntax" || reason == "invalid_inline_formula_syntax" ||
                         reason == "invalid_table_structure");
                    const bool exception = state == "failed" && derived.state == "failed" &&
                        !output.error.empty() && reason == "region_inference_exception:" + output.error;
                    require(structural || exception || (state == derived.state && reason == derived.reason),
                            where + " 判定与输出或停止原因不一致");
                }
                if (state != "ok" && state != "skipped")
                    require(error == assessment.at("reason"), where + " 判定原因与错误记录不一致");
                if (state == "ok") {
                    require(((version == "1.9" && type == "image") || derived.state == "ok") && error.is_null() &&
                            provenance.at("raw_output_base64").is_null() && provenance.at("text_base64").is_null(),
                            where + " 不可靠结果不可标记正常");
                    const auto text = content.at("text").get<std::string>();
                    if (type == "text")
                        require(text == output.text, where + " 正文与保存的识别文字不一致");
                    else if (type == "formula")
                        require(matches_formula_content(output.text, text, content.at("display").get<bool>()),
                                where + " 公式与保存的识别文字不一致");
                    else if (type == "table") {
                        const auto normalized = parse_table(output.text);
                        require(normalized.valid && normalized.html == text,
                                where + " 表格与保存的识别文字不一致");
                    }
                }
            }
        }
        std::set<std::string> order;
        for (const auto& value : array_field(page, "reading_order", where)) {
            require(value.is_string() && blocks.count(value.get<std::string>()) &&
                    order.insert(value.get<std::string>()).second, where + " reading_order 引用或重复无效");
        }
        require(order == blocks, where + " reading_order 未覆盖所有内容块");
        for (const auto& relation : array_field(page, "relations", where)) {
            auto type = string_field(relation, "type", where + ".relations");
            if (type == "content_owned_by") {
                require(version != "1.0" &&
                        layouts.count(string_field(relation, "source_layout_block_id", where)) &&
                        blocks.count(string_field(relation, "owner_block_id", where)), where + " 内容归属引用不存在");
                const auto& owner = *by_id.at(relation.at("owner_block_id").get<std::string>());
                bool part_of_owner = false;
                for (const auto& region_id : owner.at("source_region_ids")) {
                    const auto& source_layouts = region_data.at(region_id.get<std::string>())->at("source_layout_block_ids");
                    if (std::find(source_layouts.begin(), source_layouts.end(), relation.at("source_layout_block_id")) !=
                        source_layouts.end()) part_of_owner = true;
                }
                require(part_of_owner, where + " 内容归属不属于 owner 的源区域");
            } else {
                require((type == "caption_of" || type == "footnote_of" || type == "heading_precedes") &&
                        (version == "1.3" || version == "1.5" || (version == "1.6" || version == "1.7" || (version == "1.8" || version == "1.9")) || pdf) &&
                        blocks.count(string_field(relation, "source_block_id", where)) &&
                        blocks.count(string_field(relation, "target_block_id", where)), where + " 语义关系引用不存在");
            }
        }
        if ((version == "1.8" || version == "1.9") && !failed) validate_structure(page, by_id, width, height, where);
        if (page.contains("layout_diagnostics")) diagnostics(page.at("layout_diagnostics"), all_paths, where);
        if (pdf) {
            if (failed) {
                const auto& error = field(page, "error", where);
                result.markdown += "## 第 " + std::to_string(page.at("pdf_page_number").get<uint64_t>()) +
                    " 页\n\n> 页面失败：" + string_field(error, "code", where + ".error") + "\n\n";
                continue;
            }
            result.markdown += "## 第 " + std::to_string(page.at("pdf_page_number").get<uint64_t>()) + " 页\n\n" +
                               render_page_markdown(page, by_id) + "\n";
        } else result.markdown = render_page_markdown(page, by_id);
    }
    if (document.contains("layout_diagnostics")) diagnostics(document.at("layout_diagnostics"), all_paths, "document");
    for (const auto& resource : resources) {
        auto resource_id = string_field(resource, "id", "resources");
        require(resource_ids.insert(resource_id).second, "重复 resource ID");
        auto path = string_field(resource, "path", "resources");
        asset_path(path, "resources");
        require(std::regex_match(path, std::regex(pdf ?
                "assets/p[0-9]{4,}-b[0-9]{4,}\\.png" :
                "assets/p0001-b[0-9]{4,}\\.png")), "resource.path 不符合版本命名");
        require(resource_paths.insert(path).second, "重复 resource.path");
        require(all_blocks.count(string_field(resource, "source_block_id", "resources")),
                "resource.source_block_id 不存在");
        auto source = resource.at("source_block_id").get<std::string>();
        const auto& block = *all_block_data.at(source);
        require(block.at("content").at("resource") == path, "resource.path 与 source_block_id 不一致");
        require(string_field(resource, "media_type", "resources") == "image/png" &&
                string_field(resource, "coordinate_space", "resources") == "raster_page",
                "resource 类型或坐标系无效");
        require(field(resource, "bbox", "resources") == block.at("bbox"), "resource.bbox 与源块不一致");
        const auto& bbox = resource.at("bbox");
        require(field(resource, "width", "resources").is_number_integer() &&
                field(resource, "height", "resources").is_number_integer() &&
                resource.at("width") == bbox[2].get<int>() - bbox[0].get<int>() &&
                resource.at("height") == bbox[3].get<int>() - bbox[1].get<int>(),
                "resource 尺寸与 bbox 不一致");
        all_paths.insert(path);
    }
    for (const auto& page : pages) for (const auto& block : page.at("blocks")) {
        const auto& path = block.at("content").at("resource");
        if (!path.is_null()) require(resource_paths.count(path.get<std::string>()),
                                     "content.resource 未在 resources 中声明");
    }
    result.asset_paths.assign(all_paths.begin(), all_paths.end());
    return result;
}

RevalidatedDocument revalidate_formula_document(const std::string& json) {
    // Validate the original provenance, stop evidence, geometry and resources
    // before interpreting any previous structural failure under current rules.
    RevalidatedDocument result{json, "", validate_and_render_document(json)};
    Json document = Json::parse(json);
    Json records = Json::array();
    size_t changed = 0;
    for (auto& page : document.at("pages")) {
        bool page_changed = false;
        for (auto& block : page.at("blocks")) {
            const auto type = block.at("type").get<std::string>();
            const bool inline_formula = type == "text" && block.at("error") == "invalid_inline_formula_syntax";
            const bool formula = type == "formula" && block.at("error") == "invalid_formula_syntax";
            auto& provenance = block.at("provenance");
            if ((!inline_formula && !formula) || block.at("status") != "partial" ||
                !provenance.contains("assessment")) continue;
            auto& assessment = provenance.at("assessment");
            const Json before = {{"status", block.at("status")}, {"error", block.at("error")},
                {"assessment", assessment}, {"content", block.at("content")}};
            const auto output = saved_output(block);
            const auto completion = assess_output(output, type);
            const auto parsed = formula ? parse_formula_content(output.text) : ParsedFormula{};
            const bool valid = completion.state == "ok" &&
                (inline_formula ? valid_text_math_content(output.text) : parsed.valid);
            if (valid) {
                block["status"] = "ok";
                block["error"] = nullptr;
                assessment["state"] = completion.state;
                assessment["reason"] = completion.reason;
                auto& content = block.at("content");
                content["text"] = inline_formula ? output.text : parsed.latex;
                if (formula) {
                    content["format"] = "latex";
                    content["display"] = parsed.display;
                }
                ++changed;
                page_changed = true;
            }
            const Json after = {{"status", block.at("status")}, {"error", block.at("error")},
                {"assessment", assessment}, {"content", block.at("content")}};
            records.push_back({{"page_id", page.at("page_id")}, {"block_id", block.at("id")},
                {"changed", valid}, {"completion_state", completion.state},
                {"completion_reason", completion.reason}, {"before", before}, {"after", after}});
        }
        if (page_changed && page.at("status") == "partial" &&
            std::all_of(page.at("blocks").begin(), page.at("blocks").end(),
                [](const Json& block) { return block.at("status") == "ok"; })) page["status"] = "ok";
    }
    if (changed) {
        if (document.at("status") == "partial" &&
            std::all_of(document.at("pages").begin(), document.at("pages").end(),
                [](const Json& page) { return page.at("status") == "ok" || page.at("status") == "blank"; }))
            document["status"] = "ok";
        result.json = document.dump(2) + '\n';
        result.rendered = validate_and_render_document(result.json);
    }
    result.report = Json({{"policy", "formula-revalidation-v1"}, {"execution", "saved_output_no_ocr"},
        {"source_document_sha256", sha256(json)}, {"result_document_sha256", sha256(result.json)},
        {"changed_blocks", changed}, {"blocks", records}}).dump(2) + '\n';
    return result;
}
}
