#pragma once
#include "dococr/inference.hpp"

namespace dococr {
// 阈值与开发对照冻结在 docs/issue-21/README.md。
struct OutputAssessment {
    std::string state = "skipped", reason = "recognition_not_executed";
    std::string finish_reason, stop_reason;
    std::string evidence_source = "raw_output";
    size_t nonempty_lines = 0, empty_number_lines = 0;
    size_t repeat_offset = 0, repeat_unit_bytes = 0, repeat_count = 0;
    double repeat_coverage = 0;
};
inline constexpr const char* assessment_policy = "region-output-v1";
OutputAssessment assess_output(const GenerationOutput& output, const std::string& type);
// 旧文档仅检查已有文字与停止证据，不补造视觉成功。
OutputAssessment assess_legacy_output(const GenerationOutput& output, const std::string& type);
std::string assessment_block_status(const std::string& state);
}
