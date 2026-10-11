#include "candidate_review.hpp"
#include "json.hpp"
#include <algorithm>

namespace dococr {
namespace {
std::string box_json(Box b) {
    return "[" + std::to_string(b.x0) + "," + std::to_string(b.y0) + "," +
           std::to_string(b.x1) + "," + std::to_string(b.y1) + "]";
}
} // namespace

CandidateReviewResult review_layout_candidates(const std::vector<LayoutCandidateReview>& reviews,
    const Image& image, const image_resources::LayoutRasterSource& source,
    const PageIdentity& identity, std::vector<LayoutCandidateRecord>& records) {
    CandidateReviewResult result;
    const auto& output = source.output;
    using Json = nlohmann::json;
    std::string pixels = std::to_string(image.width) + "x" + std::to_string(image.height) + ":";
    pixels.append(reinterpret_cast<const char*>(image.rgb.data()), image.rgb.size());
    const auto page_hash = sha256(pixels);
    const auto source_asset = identity.page_asset("review-source.png");
    const bool relevant = reviews.empty() ||
        std::any_of(reviews.begin(), reviews.end(),
                    [&](const LayoutCandidateReview& review) { return review.page_rgb_sha256 == page_hash; });
    if (relevant) result.assets.push_back({source_asset, image_resources::crop_png(image, {0, 0, image.width, image.height})});
    Json evidence = {{"policy", "explicit-candidate-review-v1"}, {"page_rgb_sha256", page_hash},
                     {"source_asset", relevant ? Json(source_asset) : Json(nullptr)}, {"decisions", Json::array()}};
    for (const auto& review : reviews) {
        Json decision = {{"candidate_id", review.candidate_id}, {"page_rgb_sha256", review.page_rgb_sha256},
                         {"candidate_sha256", review.candidate_sha256}, {"decision", review.decision},
                         {"reason", review.reason}, {"outcome", "page_mismatch"}, {"crop_asset", nullptr}};
        if (review.page_rgb_sha256 == page_hash) {
            decision["outcome"] = "candidate_mismatch";
            if (size_t(review.candidate_id) < records.size()) {
                auto& candidate = records[size_t(review.candidate_id)];
                std::string bytes = box_json(candidate.crop) + ":";
                bytes.append(output.candidate_row_bytes(size_t(candidate.id)));
                bytes.append(output.mask_row(size_t(candidate.id)).bytes());
                if (sha256(bytes) == review.candidate_sha256) {
                    decision["outcome"] = "already_filtered";
                    if (candidate.selected) {
                        const auto crop_asset = identity.page_asset("review-c") + std::to_string(candidate.id) + ".png";
                        result.assets.push_back({crop_asset, image_resources::crop_png(image, candidate.crop)});
                        decision["crop_asset"] = crop_asset;
                        decision["outcome"] = "retained";
                        const bool confirmed = review.decision == "confirmed_watermark" || review.decision == "confirmed_decoration";
                        // Only original image (class 14) may be skipped; headers and all other labels are protected.
                        if (confirmed && candidate.class_id == 14) {
                            candidate.mask_asset = identity.page_asset("mask-c") + std::to_string(candidate.id) + ".png";
                            result.assets.push_back({candidate.mask_asset,
                                image_resources::layout_mask_png(candidate, source,
                                    image_resources::MaskPurpose::CandidateReview)});
                            candidate.selected = false;
                            candidate.reason = "review_" + review.decision;
                            decision["outcome"] = "skipped";
                        } else if (confirmed) decision["outcome"] = "protected_content";
                    }
                }
            }
        }
        evidence["decisions"].push_back(std::move(decision));
    }
    result.evidence = evidence.dump();
    return result;
}
} // namespace dococr
