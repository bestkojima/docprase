#include "page_identity.hpp"
#include "pdf_page_id.hpp"
#include <iomanip>
#include <sstream>

namespace dococr {
std::string PageIdentity::page_id() const {
    return pdf_page_id(source_page_ ? source_page_ : 1);
}

std::string PageIdentity::page_asset(const std::string& suffix) const {
    return "assets/" + page_id() + "-" + suffix;
}
std::string PageIdentity::block_asset(const std::string& block_id) const {
    return source_page_ ? "assets/" + block_id + ".png" :
                              page_asset(block_id + ".png");
}

std::string PageIdentity::id(const char prefix, size_t index) const {
    std::ostringstream out;
    if (source_page_) out << page_id() << '-';
    out << prefix << std::setw(4) << std::setfill('0') << index;
    return out.str();
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
} // namespace dococr
