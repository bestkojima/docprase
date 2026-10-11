#pragma once
#include "block_result.hpp"
#include "layout_candidate.hpp"
#include "layout_preprocess.hpp"
#include "label_export_policy.hpp"

namespace dococr {
struct DocumentBlock {
    std::string id, layout_id, region_id, type, resource;
    BlockResult result;
    Box box, layout_box;
    bool crop_expanded = false;
    float detection_score = 0;
    int candidate_rank = 0, original_class_id = 0;
    int candidate_id = -1, mask_row = -1, mask_nonzero = 0;
    std::string model_label;
    float raw_box[4]{};
    bool clamped = false;
    std::vector<std::string> owned_layout_ids;
    RegionRecognition recognition;
};
struct PageMetadata {
    int width = 0, height = 0;
    uint32_t source_page = 0; // 0 为图片，PDF 页号从 1 开始。
    std::string document_id; // 调用方按原始像素计算；导出不读取像素。
};
struct PageLayoutExport {
    const std::vector<LayoutCandidateRecord>& candidates;
    const PreRecognitionPagePlan& plan;
    const LayoutPageTransform& transform;
    const LabelExportPolicy& label_policy;
    std::string overlay_asset;
    double score_threshold = .5;
    std::string candidate_reviews;
};
struct PageDocumentInput {
    PageMetadata page;
    const std::vector<DocumentBlock>& blocks;
    std::string state, model_profile;
    const PageLayoutExport* layout = nullptr; // 无版面证据时保持通用作业的导出格式。
};
struct PageDocumentOutput {
    std::string json, markdown;
};
// 同时生成 DocumentIR 与 Markdown；有计划时按冻结 block_order 读取块。
// 不改写输入，不生成资源字节，不推理，也不重算页面计划。
PageDocumentOutput export_document_page(const PageDocumentInput& input);
// DocumentIR 与作业运行记录共用同一份尝试来源格式。
std::string recognition_json(const RegionRecognition& recognition);
}
