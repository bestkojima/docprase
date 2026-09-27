#include "pdf_renderer.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dococr {
namespace {
namespace fs = std::filesystem;

std::string read_file(const fs::path& path, size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw PdfError("pdf_tool_output_missing", path.u8string());
    const auto size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > limit)
        throw PdfError("pdf_tool_output_limit", path.u8string());
    file.seekg(0);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

fs::path tool_path(const char* name) {
#ifdef _WIN32
    const wchar_t* bin = _wgetenv(L"DOCOCR_POPPLER_BIN");
    return bin && *bin ? fs::path(bin) / fs::u8path(name) : fs::u8path(name);
#else
    const char* bin = std::getenv("DOCOCR_POPPLER_BIN");
    return bin && *bin ? fs::u8path(bin) / name : fs::path(name);
#endif
}

#ifdef _WIN32
std::wstring wide(const std::string& utf8) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) throw PdfError("pdf_path_encoding", "UTF-8 路径转换失败");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), result.data(), size);
    return result;
}
std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
#endif

int run_tool(const fs::path& executable, const std::vector<std::string>& args,
             const fs::path& output) {
#ifdef _WIN32
    std::wstring cmd = quote(wide(executable.u8string()));
    for (const auto& arg : args) cmd += L" " + quote(wide(arg));
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw PdfError("pdf_temp_error", "无法创建工具日志");
    SetHandleInformation(file, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = file; startup.hStdError = file;
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        throw PdfError("pdf_tool_launch_failed", "无法打开 NUL 输入");
    }
    SetHandleInformation(null_input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    startup.hStdInput = null_input;
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(file);
    CloseHandle(null_input);
    if (!ok) throw PdfError("pdf_tool_unavailable", executable.u8string());
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    int fd = open(output.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) throw PdfError("pdf_temp_error", output.u8string());
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fd);
    std::string name = executable.u8string();
    std::vector<char*> argv;
    argv.push_back(name.data());
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** item = environ; *item; ++item)
        if (std::string(*item).rfind("LC_ALL=", 0) != 0) environment.emplace_back(*item);
    environment.emplace_back("LC_ALL=C");
    std::vector<char*> envp;
    for (auto& item : environment) envp.push_back(item.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    int launch = name.find('/') == std::string::npos ?
        posix_spawnp(&pid, name.c_str(), &actions, nullptr, argv.data(), envp.data()) :
        posix_spawn(&pid, name.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fd);
    if (launch) throw PdfError(launch == ENOENT ? "pdf_tool_unavailable" : "pdf_tool_launch_failed", name);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw PdfError("pdf_tool_wait_failed", name);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

uint32_t parse_uint(const std::string& value) {
    unsigned long number = std::stoul(value);
    if (!number || number > std::numeric_limits<uint32_t>::max())
        throw PdfError("pdf_invalid_metadata", "无效 PDF 页数");
    return static_cast<uint32_t>(number);
}
} // namespace

PdfRenderer::PdfRenderer(const uint8_t* data, size_t size) {
    if (!data || size < 8 || size > 256 * 1024 * 1024 ||
        std::string(reinterpret_cast<const char*>(data), 5) != "%PDF-")
        throw PdfError("pdf_invalid_input", "PDF 文件头或大小无效");
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::ostringstream name;
        name << "dococr-pdf-" << std::hex << random() << random();
        directory_ = fs::temp_directory_path() / name.str();
        std::error_code ec;
        if (fs::create_directory(directory_, ec)) break;
        if (attempt == 7) throw PdfError("pdf_temp_error", "无法创建 PDF 临时目录");
    }
#ifndef _WIN32
    chmod(directory_.c_str(), 0700);
#endif
    try {
        source_ = directory_ / "source.pdf";
        std::ofstream file(source_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) throw PdfError("pdf_temp_error", "无法暂存 PDF");
        file.close();
        pdfinfo_ = tool_path("pdfinfo"
#ifdef _WIN32
                             ".exe"
#endif
                             );
        pdftoppm_ = tool_path("pdftoppm"
#ifdef _WIN32
                              ".exe"
#endif
                              );
        std::string versions = invoke(pdftoppm_, {"-v"});
        std::smatch match;
        if (!std::regex_search(versions, match, std::regex(R"(pdftoppm version ([0-9][^\s]*))")))
            throw PdfError("pdf_tool_version_unknown", versions);
        version_ = match[1];
        std::string info_version = invoke(pdfinfo_, {"-v"});
        if (!std::regex_search(info_version, match, std::regex(R"(pdfinfo version ([0-9][^\s]*))")) ||
            match[1].str() != version_)
            throw PdfError("pdf_tool_version_mismatch", "pdfinfo 与 pdftoppm 版本不同或未知");
        std::string info;
        try { info = invoke(pdfinfo_, {source_.u8string()}); }
        catch (const PdfError& error) {
            if (error.code == "pdf_tool_failed" &&
                std::string(error.what()).find("Incorrect password") != std::string::npos)
                throw PdfError("pdf_encrypted", "加密 PDF 不受支持");
            throw;
        }
        if (std::regex_search(info, std::regex(R"((?:^|\n)Encrypted:\s+yes)")))
            throw PdfError("pdf_encrypted", "加密 PDF 不受支持");
        if (!std::regex_search(info, match, std::regex(R"((?:^|\n)Pages:\s+([0-9]+))")))
            throw PdfError("pdf_invalid_metadata", "PDF 页数不可读");
        page_count_ = parse_uint(match[1]);
    } catch (...) {
        std::error_code ec; fs::remove_all(directory_, ec);
        throw;
    }
}

PdfRenderer::~PdfRenderer() {
    std::error_code ec; fs::remove_all(directory_, ec);
}

std::string PdfRenderer::invoke(const fs::path& executable,
                                const std::vector<std::string>& arguments) const {
    fs::path log = directory_ / "tool.log";
    int status = run_tool(executable, arguments, log);
    std::string output = read_file(log, 1024 * 1024);
    if (status == 127) throw PdfError("pdf_tool_unavailable", executable.u8string());
    if (status != 0) throw PdfError("pdf_tool_failed", output.substr(0, 4096));
    return output;
}

PdfGeometry PdfRenderer::geometry(uint32_t page, uint32_t dpi) const {
    std::string number = std::to_string(page);
    std::string info = invoke(pdfinfo_, {"-f", number, "-l", number, "-box", source_.u8string()});
    std::smatch match;
    std::regex crop_pattern("Page\\s+" + number + R"( CropBox:\s+(-?[0-9]+(?:\.[0-9]+)?)\s+(-?[0-9]+(?:\.[0-9]+)?)\s+(-?[0-9]+(?:\.[0-9]+)?)\s+(-?[0-9]+(?:\.[0-9]+)?))");
    if (!std::regex_search(info, match, crop_pattern))
        throw PdfError("pdf_page_geometry_failed", "页面 CropBox 不可读：" + number);
    PdfGeometry result;
    for (int i = 0; i < 4; ++i) result.crop_box_points[i] = std::stod(match[i+1]);
    result.width_points = result.crop_box_points[2] - result.crop_box_points[0];
    result.height_points = result.crop_box_points[3] - result.crop_box_points[1];
    if (!std::isfinite(result.width_points) || !std::isfinite(result.height_points) ||
        result.width_points <= 0 || result.height_points <= 0)
        throw PdfError("pdf_page_geometry_failed", "页面尺寸无效：" + number);
    std::regex rotation_pattern("Page\\s+" + number + R"( rot:\s+([0-9]+))");
    if (std::regex_search(info, match, rotation_pattern)) result.rotation = std::stoi(match[1]);
    if (result.rotation == 90 || result.rotation == 270)
        std::swap(result.width_points, result.height_points);
    double w = std::ceil(result.width_points * dpi / 72.0) + 1;
    double h = std::ceil(result.height_points * dpi / 72.0) + 1;
    if (w > std::numeric_limits<uint32_t>::max() || h > std::numeric_limits<uint32_t>::max())
        throw PdfError("pdf_page_too_large", "页面尺寸超出渲染器范围");
    result.raster_width = static_cast<uint32_t>(w);
    result.raster_height = static_cast<uint32_t>(h);
    result.estimated_pixels = uint64_t(result.raster_width) * result.raster_height;
    return result;
}

std::vector<uint8_t> PdfRenderer::render(uint32_t page, uint32_t dpi) const {
    fs::path prefix = directory_ / "page";
    fs::path png = directory_ / "page.png";
    std::string number = std::to_string(page);
    invoke(pdftoppm_, {"-f", number, "-l", number, "-singlefile", "-cropbox", "-r",
                        std::to_string(dpi), "-png", source_.u8string(), prefix.u8string()});
    std::string bytes = read_file(png, 64 * 1024 * 1024);
    std::error_code ec; fs::remove(png, ec);
    return {bytes.begin(), bytes.end()};
}
} // namespace dococr
