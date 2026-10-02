#include "pdf_job.hpp"
#include "config.hpp"
#include "pdf_renderer.hpp"
#include "pdf_page_id.hpp"
#include "dococr/dococr.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>

namespace dococr {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
uint64_t milliseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
Json rss_bytes() {
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) != 0) continue;
        std::istringstream value(line.substr(6));
        uint64_t kib = 0;
        std::string unit;
        if (value >> kib >> unit && unit == "kB") return kib * 1024;
    }
#endif
    return nullptr;
}
Json failed_page(uint32_t page, const Json& geometry, const Json& raster_size, const std::string& code,
                 const std::string& message) {
    Json result = {{"page_id", pdf_page_id(page)}, {"page_index", page - 1},
                   {"pdf_page_number", page}, {"status", "failed"},
                   {"raster_size", raster_size}, {"coordinate_space", "raster_page"},
                   {"reading_order", Json::array()}, {"layout_blocks", Json::array()},
                   {"regions", Json::array()}, {"blocks", Json::array()},
                   {"relations", Json::array()},
                   {"error", {{"code", code}, {"message", message}}}};
    result.update(geometry);
    return result;
}
Json png_raster_size(const std::vector<uint8_t>& png) {
    if (png.size() < 24 ||
        std::string(reinterpret_cast<const char*>(png.data()), 8) != "\x89PNG\r\n\x1a\n")
        throw PdfError("pdf_invalid_raster", "Poppler 未生成 PNG 页面");
    auto be32 = [&](size_t offset) {
        return uint32_t(png[offset]) << 24 | uint32_t(png[offset+1]) << 16 |
               uint32_t(png[offset+2]) << 8 | uint32_t(png[offset+3]);
    };
    uint32_t width = be32(16), height = be32(20);
    if (!width || !height) throw PdfError("pdf_invalid_raster", "PNG 页面尺寸无效");
    return Json::array({width, height});
}
Json geometry_json(uint32_t page, uint32_t dpi, const PdfGeometry& geometry) {
    double x0 = geometry.crop_box_points[0], y0 = geometry.crop_box_points[1];
    double x1 = geometry.crop_box_points[2], y1 = geometry.crop_box_points[3];
    double scale = double(dpi) / 72.0;
    Json affine = geometry.rotation == 90 ? Json::array({0, scale, scale, 0, -scale*y0, -scale*x0}) :
                  geometry.rotation == 180 ? Json::array({-scale, 0, 0, scale, scale*x1, -scale*y0}) :
                  geometry.rotation == 270 ? Json::array({0, -scale, -scale, 0, scale*y1, scale*x1}) :
                  Json::array({scale, 0, 0, -scale, -scale*x0, scale*y1});
    return {{"pdf_page_number", page}, {"pdf_page_size_points",
             {geometry.width_points, geometry.height_points}},
            {"pdf_crop_box_points", {geometry.crop_box_points[0], geometry.crop_box_points[1],
                                      geometry.crop_box_points[2], geometry.crop_box_points[3]}},
            {"pdf_rotation_degrees", geometry.rotation}, {"raster_dpi", dpi},
            {"pdf_points_per_raster_pixel", 72.0 / dpi},
            {"pdf_points_to_raster_affine", affine},
            {"estimated_raster_pixels", geometry.estimated_pixels}};
}
} // namespace

PdfJobResult run_pdf(IInferenceEngine* backend, InputView input,
                     uint32_t first_page, uint32_t last_page, uint32_t dpi,
                     uint64_t request_max_pixels, std::atomic_bool& cancelled,
                     const ExecutionPlan* plan, const ProgressCallback& progress) {
    PdfJobResult result;
    result.run.code = RunCode::InputError;
    auto document_start = Clock::now();
    if (input.width || input.height || input.row_stride || dpi < 36 || dpi > 600 ||
        (first_page && last_page && first_page > last_page)) {
        result.run.error_code = "pdf_invalid_options";
        result.run.error_message = "PDF 页范围、DPI 或输入字段无效";
        return result;
    }
    uint64_t max_pixels = plan ? plan->max_page_pixels : 16000000;
    if (request_max_pixels) max_pixels = std::min(max_pixels, request_max_pixels);
    try {
        PdfRenderer renderer(input.data, input.size);
        uint32_t first = first_page ? first_page : 1;
        uint32_t last = last_page ? last_page : renderer.page_count();
        if (first > last || last > renderer.page_count()) {
            result.run.error_code = "pdf_invalid_page_range";
            result.run.error_message = "PDF 页范围超出原文页数";
            return result;
        }
        std::string source_hash = sha256(std::string(reinterpret_cast<const char*>(input.data), input.size));
        std::string identity = sha256(source_hash + ":" + std::to_string(first) + ":" +
                                      std::to_string(last) + ":" + std::to_string(dpi));
        Json document = {{"schema_version", "1.4"}, {"document_id", "doc-" + identity.substr(0, 16)},
                         {"status", "ok"},
                         {"source", {{"type", "pdf"}, {"sha256", source_hash},
                                     {"page_count", renderer.page_count()},
                                     {"selected_pages", {first, last}}, {"dpi", dpi},
                                     {"renderer", "poppler"}, {"renderer_version", renderer.version()}}},
                         {"pages", Json::array()}, {"resources", Json::array()}};
        Json page_runs = Json::array();
        std::string markdown;
        size_t success_count = 0, blank_count = 0, failure_count = 0, partial_count = 0;
        uint64_t output_bytes = 0;
        bool budget_failed = false;
        std::string first_failure_code, first_failure_message;
        std::string first_budget_code, first_budget_message;
        auto cancel_with_evidence = [&](const Json* in_flight = nullptr) {
            if (in_flight) {
                Json interrupted = *in_flight;
                if (!interrupted.contains("status")) interrupted["status"] = "cancelled";
                page_runs.push_back(std::move(interrupted));
            }
            result.run.code = RunCode::Cancelled;
            result.manifest_pdf = Json{{"source_sha256", source_hash},
                {"page_count", renderer.page_count()}, {"selected_pages", {first, last}},
                {"dpi", dpi}, {"renderer", "poppler"},
                {"renderer_version", renderer.version()}, {"pages", page_runs},
                {"completed_pages", in_flight ? page_runs.size() - 1 : page_runs.size()},
                {"total_wall_ms", milliseconds(document_start)}}.dump();
            return result;
        };
        auto failure_with_evidence = [&](Json& in_flight, const std::string& code,
                                         const std::string& message) {
            in_flight["status"] = "failed";
            in_flight["error"] = {{"code", code}, {"message", message}};
            auto failed = cancel_with_evidence(&in_flight);
            failed.run.code = code.find("budget") != std::string::npos ?
                              RunCode::BudgetExceeded : RunCode::Failed;
            failed.run.error_code = code;
            failed.run.error_message = message;
            if (failed.run.code == RunCode::BudgetExceeded) failed.run.budget_stage = code;
            return failed;
        };
        const uint32_t selected_count = last - first + 1;
        for (uint32_t page = first; page <= last; ++page) {
            if (cancelled) return cancel_with_evidence();
            if (progress) progress("page_started", page, "", page - first, selected_count);
            auto page_start = Clock::now();
            Json record = {{"page_id", pdf_page_id(page)}, {"pdf_page_number", page},
                           {"geometry_ms", 0}, {"render_ms", 0}, {"pipeline_ms", 0},
                           {"rss_before_bytes", rss_bytes()}};
            Json geometry = Json::object();
            Json raster_size = nullptr;
            try {
                PdfGeometry size = renderer.geometry(page, dpi);
                record["geometry_ms"] = milliseconds(page_start);
                if (cancelled) {
                    record["total_ms"] = milliseconds(page_start);
                    return cancel_with_evidence(&record);
                }
                geometry = geometry_json(page, dpi, size);
                record["estimated_raster_pixels"] = size.estimated_pixels;
                if (size.estimated_pixels > max_pixels) {
                    budget_failed = true;
                    throw PdfError("pdf_page_pixel_budget", "页面渲染前像素预估超出有效上限");
                }
                auto render_start = Clock::now();
                auto png = renderer.render(page, dpi);
                record["render_ms"] = milliseconds(render_start);
                if (cancelled) {
                    record["total_ms"] = milliseconds(page_start);
                    return cancel_with_evidence(&record);
                }
                raster_size = png_raster_size(png);
                record["raster_size"] = raster_size;
                auto pipeline_start = Clock::now();
                InputView page_input{png.data(), png.size(), DOCOCR_IMAGE_PNG, 0, 0, 0};
                RunResult run = run_page(backend, page_input, cancelled, plan, page, progress);
                record["pipeline_ms"] = milliseconds(pipeline_start);
                if (run.code == RunCode::Cancelled) {
                    record["pipeline_status"] = "cancelled";
                    record["decode_ms"] = run.decode_ms;
                    record["layout_ms"] = run.layout_ms;
                    record["recognition_ms"] = run.recognition_ms;
                    record["regions"] = Json::array();
                    for (const auto& region : run.regions)
                        record["regions"].push_back({{"request_id", region.request_id},
                            {"status", region.status}, {"stop_reason", region.stop_reason},
                            {"recognition", region.recognition_json.empty() ? Json(nullptr) : Json::parse(region.recognition_json)}});
                    record["total_ms"] = milliseconds(page_start);
                    return cancel_with_evidence(&record);
                }
                record["pipeline_status"] = run.code == RunCode::Ok ? "ok" :
                    run.code == RunCode::Partial ? "partial" : run.code == RunCode::Blank ? "blank" :
                    run.code == RunCode::BudgetExceeded ? "budget_exceeded" : "failed";
                record["decode_ms"] = run.decode_ms;
                record["layout_ms"] = run.layout_ms;
                record["recognition_ms"] = run.recognition_ms;
                record["export_ms"] = run.export_ms;
                record["raster_pixels"] = run.page_pixels;
                record["pipeline_audit"] = {{"did_decode", run.did_decode},
                    {"did_layout", run.did_layout}, {"layout_attempted", run.layout_attempted},
                    {"did_normalize", run.did_normalize}, {"did_crop", run.did_crop},
                    {"did_reset", run.did_reset}, {"reset_failed", run.reset_failed},
                    {"did_recognition", run.did_recognition}, {"did_export", run.did_export}};
                record["regions"] = Json::array();
                for (const auto& region : run.regions)
                    record["regions"].push_back({{"request_id", region.request_id},
                        {"status", region.status}, {"stop_reason", region.stop_reason},
                        {"elapsed_ms", region.elapsed_ms},
                        {"recognition", region.recognition_json.empty() ? Json(nullptr) : Json::parse(region.recognition_json)}});
                result.run.regions.insert(result.run.regions.end(), run.regions.begin(), run.regions.end());
                result.run.page_pixels += run.page_pixels;
                result.run.decode_ms += run.decode_ms;
                result.run.layout_ms += run.layout_ms;
                result.run.recognition_ms += run.recognition_ms;
                result.run.export_ms += run.export_ms;
                result.run.did_decode |= run.did_decode;
                result.run.did_layout |= run.did_layout;
                result.run.did_normalize |= run.did_normalize;
                result.run.did_crop |= run.did_crop;
                result.run.did_reset |= run.did_reset;
                result.run.reset_failed |= run.reset_failed;
                result.run.layout_attempted |= run.layout_attempted;
                result.run.did_recognition |= run.did_recognition;
                result.run.did_export |= run.did_export;
                if (run.code == RunCode::Ok || run.code == RunCode::Partial || run.code == RunCode::Blank) {
                    uint64_t page_bytes = run.output.json.size() + run.output.markdown.size();
                    for (const auto& asset : run.output.assets) page_bytes += asset.png.size();
                    if (plan && (page_bytes > plan->max_output_bytes ||
                                 output_bytes > plan->max_output_bytes - page_bytes)) {
                        budget_failed = true;
                        throw PdfError("pdf_document_output_budget", "PDF 作业累计导出字节超出上限");
                    }
                    output_bytes += page_bytes;
                    Json fragment = Json::parse(run.output.json);
                    if (fragment.at("schema_version") == "1.10" || fragment.at("schema_version") == "1.11") {
                        if (document["schema_version"] != "1.11")
                            document["schema_version"] = fragment.at("schema_version");
                        document["export_policy"] = fragment.at("export_policy");
                    }
                    else if (fragment.at("schema_version") == "1.9" && document["schema_version"] != "1.10" && document["schema_version"] != "1.11") document["schema_version"] = "1.9";
                    else if (fragment.at("schema_version") == "1.8" && document["schema_version"] != "1.9" && document["schema_version"] != "1.10" && document["schema_version"] != "1.11")
                        document["schema_version"] = "1.8";
                    else if (fragment.at("schema_version") == "1.7" && document["schema_version"] != "1.8" && document["schema_version"] != "1.9" && document["schema_version"] != "1.10" && document["schema_version"] != "1.11")
                        document["schema_version"] = "1.7";
                    else if (fragment.at("schema_version") == "1.5" && document["schema_version"] != "1.7" && document["schema_version"] != "1.8" && document["schema_version"] != "1.9" && document["schema_version"] != "1.10" && document["schema_version"] != "1.11")
                        document["schema_version"] = "1.5";
                    Json page_data = fragment.at("pages").at(0);
                    page_data.update(geometry);
                    page_data["error"] = nullptr;
                    if (fragment.contains("layout_diagnostics"))
                        page_data["layout_diagnostics"] = fragment["layout_diagnostics"];
                    document["pages"].push_back(std::move(page_data));
                    for (const auto& resource : fragment.at("resources"))
                        document["resources"].push_back(resource);
                    for (auto& asset : run.output.assets)
                        result.run.output.assets.push_back(std::move(asset));
                    markdown += "## 第 " + std::to_string(page) + " 页\n\n" + run.output.markdown + "\n";
                    ++success_count;
                    if (run.code == RunCode::Blank) ++blank_count;
                    if (run.code == RunCode::Partial) ++partial_count;
                    record["status"] = run.code == RunCode::Blank ? "blank" :
                                       run.code == RunCode::Partial ? "partial" : "ok";
                } else {
                    std::string code = run.code == RunCode::BudgetExceeded ? "pdf_page_output_budget" :
                                       run.error_code.empty() ? "pdf_page_pipeline_failed" : run.error_code;
                    if (run.code == RunCode::BudgetExceeded) budget_failed = true;
                    throw PdfError(code, run.error_message.empty() ? run.budget_stage : run.error_message);
                }
            } catch (const PdfError& error) {
                if (cancelled) {
                    record["total_ms"] = milliseconds(page_start);
                    return failure_with_evidence(record, error.code, error.what());
                }
                ++failure_count;
                if (first_failure_code.empty()) {
                    first_failure_code = error.code;
                    first_failure_message = error.what();
                }
                if (first_budget_code.empty() && error.code.find("budget") != std::string::npos) {
                    first_budget_code = error.code;
                    first_budget_message = error.what();
                }
                document["pages"].push_back(failed_page(page, geometry, raster_size, error.code, error.what()));
                markdown += "## 第 " + std::to_string(page) + " 页\n\n> 页面失败：" + error.code + "\n\n";
                record["status"] = "failed";
                record["error"] = {{"code", error.code}, {"message", error.what()}};
            } catch (const std::bad_alloc&) { throw; }
              catch (const std::exception& error) {
                if (cancelled) {
                    record["total_ms"] = milliseconds(page_start);
                    return failure_with_evidence(record, "pdf_page_failed", error.what());
                }
                ++failure_count;
                if (first_failure_code.empty()) {
                    first_failure_code = "pdf_page_failed";
                    first_failure_message = error.what();
                }
                document["pages"].push_back(failed_page(page, geometry, raster_size, "pdf_page_failed", error.what()));
                markdown += "## 第 " + std::to_string(page) + " 页\n\n> 页面失败：pdf_page_failed\n\n";
                record["status"] = "failed";
                record["error"] = {{"code", "pdf_page_failed"}, {"message", error.what()}};
            }
            record["total_ms"] = milliseconds(page_start);
            record["rss_after_bytes"] = rss_bytes();
            page_runs.push_back(std::move(record));
            if (progress) progress("page_completed", page, "", page - first + 1, selected_count);
            if (cancelled) return cancel_with_evidence();
            if (page == std::numeric_limits<uint32_t>::max()) break;
        }
        document["status"] = failure_count || partial_count ? "partial" :
                             blank_count == success_count ? "blank" : "ok";
        result.run.output.json = document.dump();
        result.run.output.markdown = std::move(markdown);
        uint64_t final_bytes = result.run.output.json.size() + result.run.output.markdown.size();
        for (const auto& asset : result.run.output.assets) final_bytes += asset.png.size();
        if (plan && final_bytes > plan->max_output_bytes) {
            result.run.code = RunCode::BudgetExceeded;
            result.run.budget_stage = "pdf_document_output_bytes";
            result.run.error_code = "pdf_document_output_budget";
            result.run.error_message = "PDF 作业总导出字节超出上限";
        } else
        result.run.code = success_count ? (failure_count || partial_count ? RunCode::Partial :
                                           blank_count == success_count ? RunCode::Blank : RunCode::Ok) :
                                           budget_failed ? RunCode::BudgetExceeded : RunCode::Failed;
        if (!success_count && result.run.error_code.empty()) {
            result.run.error_code = budget_failed ? first_budget_code : first_failure_code;
            if (result.run.error_code.empty()) result.run.error_code = "pdf_all_pages_failed";
            result.run.error_message = budget_failed ? first_budget_message : first_failure_message;
            if (result.run.error_message.empty()) result.run.error_message = "所选 PDF 页面均未成功解析";
            if (budget_failed) result.run.budget_stage = result.run.error_code;
        }
        Json pdf_manifest = {{"source_sha256", source_hash}, {"page_count", renderer.page_count()},
                             {"selected_pages", {first, last}}, {"dpi", dpi},
                             {"renderer", "poppler"}, {"renderer_version", renderer.version()},
                             {"effective_max_page_pixels", max_pixels}, {"pages", page_runs},
                             {"total_wall_ms", milliseconds(document_start)},
                             {"memory", {{"method", "linux_proc_self_status_VmRSS_snapshots"},
                                         {"sampled_process", "dococr_parent_only"},
                                         {"renderer_child_peak_bytes", nullptr},
                                         {"note", "页面前后 RSS 快照不是进程峰值；Poppler 子进程内存未计入"}}}};
        result.manifest_pdf = pdf_manifest.dump();
        return result;
    } catch (const PdfError& error) {
        result.run.error_code = error.code;
        result.run.error_message = error.what();
        result.run.code = error.code == "pdf_tool_unavailable" ? RunCode::Unsupported : RunCode::InputError;
        return result;
    }
}
} // namespace dococr
