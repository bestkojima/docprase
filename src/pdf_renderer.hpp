#ifndef DOCOCR_PDF_RENDERER_HPP
#define DOCOCR_PDF_RENDERER_HPP
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dococr {
struct PdfError : std::runtime_error {
    std::string code;
    PdfError(std::string c, std::string message) : std::runtime_error(message), code(std::move(c)) {}
};
struct PdfGeometry {
    double width_points = 0, height_points = 0;
    double crop_box_points[4]{};
    int rotation = 0;
    uint32_t raster_width = 0, raster_height = 0;
    uint64_t estimated_pixels = 0;
};
class PdfRenderer {
public:
    PdfRenderer(const uint8_t* data, size_t size);
    ~PdfRenderer();
    PdfRenderer(const PdfRenderer&) = delete;
    PdfRenderer& operator=(const PdfRenderer&) = delete;
    uint32_t page_count() const { return page_count_; }
    const std::string& version() const { return version_; }
    PdfGeometry geometry(uint32_t page, uint32_t dpi) const;
    std::vector<uint8_t> render(uint32_t page, uint32_t dpi) const;
private:
    std::filesystem::path directory_, source_, pdfinfo_, pdftoppm_;
    uint32_t page_count_ = 0;
    std::string version_;
    std::string invoke(const std::filesystem::path& executable,
                       const std::vector<std::string>& arguments) const;
};
} // namespace dococr
#endif
