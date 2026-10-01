#include "output_assessment.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace dococr {
namespace {
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
bool empty_number(std::string line) {
    // Heading/list syntax followed only by a number; a question with text is not empty.
    const auto first = line.find_first_not_of('#');
    if (first == std::string::npos) return false;
    line = trim(line.substr(first));
    if (line.empty()) return false;
    size_t i = line[0] == '(' ? 1 : 0, begin = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == begin) return false;
    while (i < line.size() && (line[i] == '.' || line[i] == ')' || line[i] == ':' ||
                              line[i] == ' ' || line[i] == '\t')) ++i;
    return i == line.size();
}
bool substantive(const std::string& unit) {
    return std::any_of(unit.begin(), unit.end(), [](unsigned char ch) {
        return ch >= 128 || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    });
}
void repetition(const std::string& text, OutputAssessment& result) {
    // Bounded by the generation budget; byte offsets point back to the untouched output.
    for (size_t size = 8; size <= 256 && size * 3 <= text.size(); ++size) {
        for (size_t offset = 0; offset + size * 3 <= text.size(); ++offset) {
            if ((static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80 ||
                (static_cast<unsigned char>(text[offset + size]) & 0xc0) == 0x80)
                continue;
            if (text.compare(offset, size, text, offset + size, size) != 0) continue;
            size_t count = 2;
            while (offset + (count + 1) * size <= text.size() &&
                   text.compare(offset, size, text, offset + count * size, size) == 0) ++count;
            if (count >= 3 && count * size >= 48 && substantive(text.substr(offset, size))) {
                const double coverage = double(count * size) / text.size();
                if (coverage > result.repeat_coverage) {
                    result.repeat_offset = offset;
                    result.repeat_unit_bytes = size;
                    result.repeat_count = count;
                    result.repeat_coverage = coverage;
                }
            }
            offset += (count - 1) * size - 1;
        }
    }
}
}
namespace {
OutputAssessment assess(const GenerationOutput& output, const std::string& type, bool require_visual) {
    OutputAssessment result;
    result.finish_reason = output.finish_reason;
    result.stop_reason = output.stop_reason;
    const auto& text = output.raw_output;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty()) continue;
        ++result.nonempty_lines;
        if (empty_number(line)) ++result.empty_number_lines;
    }
    // Identical cells/rows in a table are legitimate; structure validation owns tables.
    if (type != "table") repetition(text, result);
    const bool limit = output.finish_reason == "truncated" || output.stop_reason == "token_limit";
    const bool visual = (output.visual_evidence == "image_pad_tokens" && output.visual_tokens > 0) ||
                        output.visual_evidence == "explicit_success";
    if (output.finish_reason == "failed" || output.stop_reason == "error" ||
        output.stop_reason == "timeout" || output.stop_reason == "cancelled") {
        result.state = "failed";
        result.reason = output.error.empty() ? "runtime_failure" : output.error;
    } else if (require_visual && !visual) {
        result.state = "failed"; result.reason = "visual_evidence_missing";
    } else if (text.empty() || output.text.empty()) {
        result.state = "failed"; result.reason = "ovis_empty_output";
    } else if (result.empty_number_lines >= 8 &&
               result.empty_number_lines * 5 >= result.nonempty_lines * 4) {
        result.state = limit ? "anomalous" : "unverified";
        result.reason = "empty_numbering_structure";
    } else if (result.repeat_coverage >= .5) {
        const bool confirmed = limit || output.stop_reason == "repetition";
        result.state = confirmed ? "anomalous" : "unverified";
        result.reason = "repeated_fragment";
    } else if (limit) {
        result.state = "incomplete"; result.reason = "ovis_token_limit";
    } else if (output.finish_reason != "complete" || output.stop_reason != "normal" ||
               !output.error.empty()) {
        result.state = "unverified"; result.reason = "unconfirmed_completion";
    } else {
        result.state = "ok"; result.reason = "normal_completion";
    }
    return result;
}
OutputAssessment assess_both(const GenerationOutput& output, const std::string& type, bool require_visual) {
    auto result = assess(output, type, require_visual);
    if (output.text != output.raw_output && type != "table" && result.state != "failed") {
        auto edited = output;
        edited.raw_output = edited.text;
        auto text_result = assess(edited, type, require_visual);
        if ((text_result.state == "anomalous" && result.state != "anomalous") ||
            (text_result.state == "unverified" && result.state == "ok")) {
            text_result.evidence_source = "text";
            result = std::move(text_result);
        }
    }
    return result;
}
}
OutputAssessment assess_output(const GenerationOutput& output, const std::string& type) {
    return assess_both(output, type, true);
}
OutputAssessment assess_legacy_output(const GenerationOutput& output, const std::string& type) {
    return assess_both(output, type, false);
}
std::string assessment_block_status(const std::string& state) {
    return state == "ok" || state == "failed" || state == "skipped" ? state : "partial";
}
}
