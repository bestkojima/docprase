#pragma once
#include "dococr/inference.hpp"

namespace dococr {
// 每次 pipeline 调用持有独立身份；0 为图片，PDF 页号从 1 开始。
class PageIdentity {
public:
    explicit PageIdentity(uint32_t source_page) : source_page_(source_page) {}
    uint32_t source_page() const { return source_page_; }
    uint32_t progress_page() const { return source_page_ ? source_page_ : 1; }
    std::string page_id() const;
    std::string page_asset(const std::string& suffix) const;
    std::string block_asset(const std::string& block_id) const;
    std::string id(char prefix, size_t index) const;
private:
    uint32_t source_page_;
};
// 维持原始 RGB 像素和尺寸的 FNV 身份；与候选复核的 SHA256 绑定分别处理。
std::string document_id(const Image& image);
}
