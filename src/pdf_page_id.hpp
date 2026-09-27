#ifndef DOCOCR_PDF_PAGE_ID_HPP
#define DOCOCR_PDF_PAGE_ID_HPP
#include <cstdint>
#include <string>

namespace dococr {
inline std::string pdf_page_id(uint32_t page) {
    std::string digits = std::to_string(page);
    return "p" + std::string(digits.size() < 4 ? 4 - digits.size() : 0, '0') + digits;
}
} // namespace dococr
#endif
