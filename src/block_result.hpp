#pragma once
#include "output_assessment.hpp"
#include "region_recognition.hpp"
#include "table_parser.hpp"

namespace dococr {
// 区域内容结果与页面几何、资源身份和执行状态分开保存。
struct BlockResult {
    std::string status, text, raw, raw_base64, text_base64, error, error_base64, format_override;
    bool display_formula = true;
    ParsedTable table;
    GenerationOutput visual;
    OutputAssessment assessment;
};
// 选用最后一次尝试，一次完成判定、规范化和字节保存；不执行推理或重试。
// 仅接收 text / formula / table。输入及其原始输出保持不变。
BlockResult finalize_region_recognition(const std::string& type, const RegionRecognition& recognition);
// 通用后端的完成/截断与 UTF-8 契约；资源回退和作业状态由执行阶段处理。
BlockResult finalize_generic_recognition(const GenericRegionRecognition& recognition);

}
