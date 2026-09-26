#include "dococr/dococr.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
}
int main(int argc, char** argv) {
    if (argc != 7 || std::string(argv[1]) != "--backend" ||
        std::string(argv[3]) != "--input" || std::string(argv[5]) != "--out") {
        std::cerr << "用法：dococr_cli --backend none --input page.png --out 输出目录\n";
        return 2;
    }
    try {
        const std::string backend = argv[2];
        fs::path input_path = fs::u8path(argv[4]);
        fs::path output_path = fs::u8path(argv[6]);
        std::ifstream input_file(input_path, std::ios::binary);
        if (!input_file) { std::cerr << "无法打开输入图片\n"; return 2; }
        std::vector<uint8_t> image((std::istreambuf_iterator<char>(input_file)), {});
        uint32_t format = image.size() >= 8 &&
            std::string(reinterpret_cast<const char*>(image.data()), 8) == std::string("\x89PNG\r\n\x1a\n", 8)
            ? DOCOCR_IMAGE_PNG : DOCOCR_IMAGE_JPEG;
        DocOcrHandle engine = 0;
        DocOcrStatus status = dococr_create({backend.data(), backend.size()}, &engine);
        if (status != DOCOCR_OK) { std::cerr << "创建引擎失败，状态码：" << status << '\n'; return 3; }
        DocOcrJob job = 0;
        status = dococr_job_create(engine, &job);
        if (status != DOCOCR_OK) { dococr_destroy(engine); return 3; }
        DocOcrInput request{sizeof(DocOcrInput), image.data(), image.size(), format, 0, 0, 0};
        status = dococr_job_run(job, &request);
        if (status != DOCOCR_OK) {
            std::cerr << "作业运行失败，状态码：" << status << '\n';
            dococr_job_destroy(job); dococr_destroy(engine);
            return status == DOCOCR_UNSUPPORTED ? 4 : 3;
        }
        DocOcrResult result{sizeof(DocOcrResult), {}, {}};
        status = dococr_job_result(job, &result);
        if (status != DOCOCR_OK) { dococr_job_destroy(job); dococr_destroy(engine); return 3; }
        bool ok = write_file(output_path / "document.json", result.json) &&
                  write_file(output_path / "document.md", result.markdown);
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
