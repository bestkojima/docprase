#pragma once
#include "config.hpp"
#include "image_resources.hpp"
#include "page_identity.hpp"

namespace dococr {
struct CandidateReviewResult {
    std::string evidence;
    std::vector<Asset> assets;
};
// 显式复核按页面像素、原模型行及完整 mask 行绑定；只修改已匹配候选的选择和原因。
// 调用方决定是否启用与何时提交资源。未确认或受保护内容保留，不读取作业审计。
CandidateReviewResult review_layout_candidates(const std::vector<LayoutCandidateReview>& reviews,
    const Image& image, const image_resources::LayoutRasterSource& source,
    const PageIdentity& identity, std::vector<LayoutCandidateRecord>& records);
}
