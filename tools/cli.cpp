#include "dococr/dococr.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupt_requested = 0;
void on_interrupt(int) { interrupt_requested = 1; }
struct OwnedBytes {
    DocOcrBytes value{};
    ~OwnedBytes() { if (value.data) dococr_bytes_free(&value); }
};
struct OwnedSession {
    DocOcrHandle engine = 0;
    DocOcrJob job = 0;
    ~OwnedSession() {
        if (job) dococr_job_destroy(job);
        if (engine) dococr_destroy(engine);
    }
};
struct OwnedResult {
    DocOcrResult value{sizeof(DocOcrResult), {}, {}};
    ~OwnedResult() {
        if (value.json.data) dococr_bytes_free(&value.json);
        if (value.markdown.data) dococr_bytes_free(&value.markdown);
    }
};
bool write_file(const fs::path& path, const DocOcrBytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data), static_cast<std::streamsize>(bytes.size));
    return bool(file);
}
bool write_audit(const fs::path& output_path, DocOcrHandle engine, DocOcrJob job) {
    OwnedBytes plan, manifest;
    DocOcrStatus plan_status = dococr_execution_plan(engine, &plan.value);
    DocOcrStatus manifest_status = dococr_job_manifest(job, &manifest.value);
    bool ok = manifest_status == DOCOCR_OK &&
              (plan_status == DOCOCR_NO_RESULT || plan_status == DOCOCR_OK);
    if (ok && plan_status == DOCOCR_OK)
        ok = write_file(output_path / "execution-plan.json", plan.value);
    if (ok) ok = write_file(output_path / "run-manifest.json", manifest.value);
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
        std::cerr << "用法：dococr_cli (--backend NAME | --config config.json) --input 文件 --out 输出目录 [--pages 首-末] [--dpi 72..600] [--max-page-pixels 正整数] [--timeout-ms 正整数]\n";
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
        bool pages_set = false, dpi_set = false, pixels_set = false, timeout_set = false;
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
            } else if (option == "--timeout-ms" && !timeout_set) {
                timeout_set = true;
                uint64_t timeout = positive_number(value);
                if (timeout > UINT32_MAX) throw std::invalid_argument("超时时间无效");
                request.timeout_ms = static_cast<uint32_t>(timeout);
            } else throw std::invalid_argument("未知或重复的 PDF 参数：" + option);
        }
        if (format != DOCOCR_DOCUMENT_PDF && (pages_set || dpi_set || pixels_set))
            throw std::invalid_argument("PDF 参数仅适用于 PDF 输入");
        OwnedSession session;
        DocOcrHandle& engine = session.engine;
        DocOcrJob& job = session.job;
        DocOcrStatus status = dococr_create({backend.data(), backend.size()}, &engine);
        if (status != DOCOCR_OK) {
            OwnedBytes error;
            if (dococr_last_error(&error.value) == DOCOCR_OK && error.value.size)
                std::cerr << std::string(reinterpret_cast<const char*>(error.value.data), error.value.size) << '\n';
            else std::cerr << "创建引擎失败，状态码：" << status << '\n';
            return 3;
        }
        status = dococr_job_create(engine, &job);
        if (status != DOCOCR_OK) return 3;
        std::signal(SIGINT, on_interrupt);
        std::atomic_bool finished{false};
        std::thread monitor([&] {
            bool cancel_sent = false;
            try {
                fs::create_directories(output_path);
                std::ofstream stream(output_path / "job-events.jsonl", std::ios::binary);
                auto drain = [&] {
                    while (true) {
                        OwnedBytes event;
                        if (dococr_job_next_event(job, &event.value) != DOCOCR_OK) break;
                        std::string line(reinterpret_cast<const char*>(event.value.data), event.value.size);
                        std::cout << line << '\n' << std::flush;
                        if (stream) stream << line << '\n' << std::flush;
                    }
                };
                while (!finished) {
                    if (interrupt_requested && !cancel_sent) {
                        dococr_job_cancel(job);
                        cancel_sent = true;
                    }
                    drain();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                drain();
            } catch (...) {
                /* 事件导出失败后仍继续处理 SIGINT，直到同步运行返回。 */
                while (!finished) {
                    if (interrupt_requested && !cancel_sent) {
                        dococr_job_cancel(job);
                        cancel_sent = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });
        status = dococr_job_run(job, &request);
        finished = true;
        monitor.join();
        OwnedBytes final_status;
        if (dococr_job_status(job, &final_status.value) == DOCOCR_OK) {
            write_file(output_path / "job-status.json", final_status.value);
        }
        if (status != DOCOCR_OK) {
            OwnedBytes event;
            if (dococr_job_poll_events(job, &event.value) == DOCOCR_OK && event.value.size)
                std::cerr << std::string(reinterpret_cast<const char*>(event.value.data), event.value.size) << '\n';
            else std::cerr << "作业运行失败，状态码：" << status << '\n';
            if ((configured || format == DOCOCR_DOCUMENT_PDF) &&
                !write_audit(output_path, engine, job))
                std::cerr << "运行清单导出失败\n";
            return status == DOCOCR_UNSUPPORTED ? 4 : status == DOCOCR_BUDGET_EXCEEDED ? 5 :
                   status == DOCOCR_TIMEOUT ? 6 : status == DOCOCR_CANCELLED ? 130 : 3;
        }
        OwnedResult result;
        status = dococr_job_result(job, &result.value);
        if (status != DOCOCR_OK) return 3;
        bool ok = write_file(output_path / "document.json", result.value.json) &&
                  write_file(output_path / "document.md", result.value.markdown);
        if (configured || format == DOCOCR_DOCUMENT_PDF)
            ok = write_audit(output_path, engine, job) && ok;
        size_t count = 0;
        if (dococr_job_asset_count(job, &count) != DOCOCR_OK) ok = false;
        for (size_t i = 0; ok && i < count; ++i) {
            OwnedBytes name, data;
            if (dococr_job_asset(job, i, &name.value, &data.value) != DOCOCR_OK) { ok = false; break; }
            std::string relative(reinterpret_cast<const char*>(name.value.data), name.value.size);
            fs::path asset_path = fs::u8path(relative);
            if (asset_path.is_absolute() || relative.find("..") != std::string::npos) ok = false;
            else ok = write_file(output_path / asset_path, data.value);
        }
        if (!ok) { std::cerr << "导出失败\n"; return 3; }
        std::cout << "已导出：" << output_path.u8string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
