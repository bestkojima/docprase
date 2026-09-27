#include "dococr/dococr.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
bool write_file(const fs::path& path, const DocOcrBytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data), static_cast<std::streamsize>(bytes.size));
    return bool(file);
}
bool write_audit(const fs::path& output_path, DocOcrHandle engine, DocOcrJob job) {
    DocOcrBytes plan{}, manifest{};
    DocOcrStatus plan_status = dococr_execution_plan(engine, &plan);
    DocOcrStatus manifest_status = dococr_job_manifest(job, &manifest);
    bool ok = manifest_status == DOCOCR_OK &&
              (plan_status == DOCOCR_NO_RESULT || plan_status == DOCOCR_OK);
    if (ok && plan_status == DOCOCR_OK)
        ok = write_file(output_path / "execution-plan.json", plan);
    if (ok) ok = write_file(output_path / "run-manifest.json", manifest);
    if (plan.data) dococr_bytes_free(&plan);
    if (manifest.data) dococr_bytes_free(&manifest);
    return ok;
}
uint64_t positive_number(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("PDF 参数须为正整数");
    uint64_t number = std::stoull(value);
    if (!number) throw std::invalid_argument("PDF 参数须为正整数");
    return number;
}
}
int main(int argc, char** argv) {
    if (argc < 7 || argc % 2 == 0 || (std::string(argv[1]) != "--backend" && std::string(argv[1]) != "--config") ||
        std::string(argv[3]) != "--input" || std::string(argv[5]) != "--out") {
        std::cerr << "用法：dococr_cli (--backend NAME | --config config.json) --input 文件 --out 输出目录 [--pages 首-末] [--dpi 72..600] [--max-page-pixels 正整数]\n";
        return 2;
    }
    try {
        std::string backend = argv[2];
        bool configured = std::string(argv[1]) == "--config";
        if (configured) {
            std::ifstream config_file(fs::u8path(backend), std::ios::binary);
            if (!config_file) { std::cerr << "无法打开配置文件\n"; return 3; }
            backend.assign(std::istreambuf_iterator<char>(config_file), {});
        }
        fs::path input_path = fs::u8path(argv[4]);
        fs::path output_path = fs::u8path(argv[6]);
        std::ifstream input_file(input_path, std::ios::binary);
        if (!input_file) { std::cerr << "无法打开输入文件\n"; return 2; }
        std::vector<uint8_t> image((std::istreambuf_iterator<char>(input_file)), {});
        uint32_t format = image.size() >= 5 &&
            std::string(reinterpret_cast<const char*>(image.data()), 5) == "%PDF-"
            ? DOCOCR_DOCUMENT_PDF : image.size() >= 8 &&
            std::string(reinterpret_cast<const char*>(image.data()), 8) == std::string("\x89PNG\r\n\x1a\n", 8)
            ? DOCOCR_IMAGE_PNG : DOCOCR_IMAGE_JPEG;
        DocOcrInput request{sizeof(DocOcrInput), image.data(), image.size(), format, 0, 0, 0};
        bool pages_set = false, dpi_set = false, pixels_set = false;
        for (int i = 7; i < argc; i += 2) {
            std::string option = argv[i], value = argv[i+1];
            if (option == "--pages" && !pages_set) {
                pages_set = true;
                auto dash = value.find('-');
                if (dash == std::string::npos || value.find('-', dash+1) != std::string::npos)
                    throw std::invalid_argument("页范围格式应为 首-末");
                uint64_t first = positive_number(value.substr(0, dash));
                uint64_t last = positive_number(value.substr(dash+1));
                if (first > UINT32_MAX || last > UINT32_MAX || first > last)
                    throw std::invalid_argument("页范围无效");
                request.first_page = static_cast<uint32_t>(first);
                request.last_page = static_cast<uint32_t>(last);
            } else if (option == "--dpi" && !dpi_set) {
                dpi_set = true;
                uint64_t dpi = positive_number(value);
                if (dpi > UINT32_MAX) throw std::invalid_argument("DPI 无效");
                request.dpi = static_cast<uint32_t>(dpi);
            } else if (option == "--max-page-pixels" && !pixels_set) {
                pixels_set = true;
                request.max_page_pixels = positive_number(value);
            } else throw std::invalid_argument("未知或重复的 PDF 参数：" + option);
        }
        if (format != DOCOCR_DOCUMENT_PDF && (pages_set || dpi_set || pixels_set))
            throw std::invalid_argument("PDF 参数仅适用于 PDF 输入");
        DocOcrHandle engine = 0;
        DocOcrStatus status = dococr_create({backend.data(), backend.size()}, &engine);
        if (status != DOCOCR_OK) {
            DocOcrBytes error{};
            if (dococr_last_error(&error) == DOCOCR_OK && error.size)
                std::cerr << std::string(reinterpret_cast<const char*>(error.data), error.size) << '\n';
            else std::cerr << "创建引擎失败，状态码：" << status << '\n';
            if (error.data) dococr_bytes_free(&error);
            return 3;
        }
        DocOcrJob job = 0;
        status = dococr_job_create(engine, &job);
        if (status != DOCOCR_OK) { dococr_destroy(engine); return 3; }
        status = dococr_job_run(job, &request);
        if (status != DOCOCR_OK) {
            DocOcrBytes event{};
            if (dococr_job_poll_events(job, &event) == DOCOCR_OK && event.size)
                std::cerr << std::string(reinterpret_cast<const char*>(event.data), event.size) << '\n';
            else std::cerr << "作业运行失败，状态码：" << status << '\n';
            if (event.data) dococr_bytes_free(&event);
            if ((configured || format == DOCOCR_DOCUMENT_PDF) &&
                !write_audit(output_path, engine, job))
                std::cerr << "运行清单导出失败\n";
            dococr_job_destroy(job); dococr_destroy(engine);
            return status == DOCOCR_UNSUPPORTED ? 4 : status == DOCOCR_BUDGET_EXCEEDED ? 5 : 3;
        }
        DocOcrResult result{sizeof(DocOcrResult), {}, {}};
        status = dococr_job_result(job, &result);
        if (status != DOCOCR_OK) { dococr_job_destroy(job); dococr_destroy(engine); return 3; }
        bool ok = write_file(output_path / "document.json", result.json) &&
                  write_file(output_path / "document.md", result.markdown);
        if (configured || format == DOCOCR_DOCUMENT_PDF)
            ok = write_audit(output_path, engine, job) && ok;
        size_t count = 0;
        if (dococr_job_asset_count(job, &count) != DOCOCR_OK) ok = false;
        for (size_t i = 0; ok && i < count; ++i) {
            DocOcrBytes name{}, data{};
            if (dococr_job_asset(job, i, &name, &data) != DOCOCR_OK) { ok = false; break; }
            std::string relative(reinterpret_cast<const char*>(name.data), name.size);
            fs::path asset_path = fs::u8path(relative);
            if (asset_path.is_absolute() || relative.find("..") != std::string::npos) ok = false;
            else ok = write_file(output_path / asset_path, data);
            dococr_bytes_free(&name); dococr_bytes_free(&data);
        }
        dococr_bytes_free(&result.json); dococr_bytes_free(&result.markdown);
        dococr_job_destroy(job); dococr_destroy(engine);
        if (!ok) { std::cerr << "导出失败\n"; return 3; }
        std::cout << "已导出：" << output_path.u8string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
