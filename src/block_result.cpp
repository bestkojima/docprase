#include "block_result.hpp"
#include "content_validation.hpp"
#include "layout_region_policy.hpp"
#include "output_encoding.hpp"
#include <stdexcept>

namespace dococr {
using output_encoding::valid_utf8;
using output_encoding::base64;

BlockResult finalize_region_recognition(const std::string& type, const RegionRecognition& recognition) {
    if (!requires_ovis(type)) throw std::invalid_argument("unsupported_region_result_type");
    BlockResult result;
    if (recognition.attempts.empty()) {
        result.status = result.assessment.state = "failed";
        result.error = result.assessment.reason = "generation_response_type_mismatch";
        result.assessment.stop_reason = "error";
    } else {
        const auto& output = recognition.attempts.back().output;
        result.visual = output;
        result.visual.raw_output.clear();
        if (valid_utf8(output.raw_output)) result.raw = output.raw_output;
        else result.raw_base64 = base64(output.raw_output);
        if (valid_utf8(output.text)) result.text = output.text;
        else result.text_base64 = base64(output.text);
        if (!valid_utf8(output.error)) result.error_base64 = base64(output.error);
        result.assessment = assess_output(output, type);
        if (result.assessment.state != "ok") {
            result.status = assessment_block_status(result.assessment.state);
            result.error = result.assessment.reason;
            result.format_override = "markdown";
            if (result.error == "visual_evidence_missing") result.text.clear();
        } else if (output.finish_reason == "complete" &&
            !output.raw_output.empty() && !output.text.empty() &&
            valid_utf8(output.raw_output) && valid_utf8(output.text)) {
            if (type == "text") {
                if (valid_text_math_content(result.text)) {
                    result.status = "ok"; result.error.clear();
                } else {
                    result.status = "partial";
                    result.error = "invalid_inline_formula_syntax";
                }
            } else if (type == "formula") {
                const auto formula = parse_formula_region(result.text);
                if (formula.valid) {
                    result.status = "ok"; result.error.clear();
                    result.text = formula.text;
                    result.format_override = formula.format;
                    result.display_formula = formula.display;
                } else {
                    result.status = "partial";
                    result.error = "invalid_formula_syntax";
                    result.format_override = "markdown";
                }
            } else {
                result.table = parse_table(result.text);
                if (valid_table_math_content(result.table)) {
                    result.status = "ok"; result.error.clear();
                    result.text = result.table.html;
                } else {
                    result.status = "partial";
                    result.error = result.table.valid ? "invalid_table_formula_syntax" : "invalid_table_structure";
                    result.format_override = "markdown";
                }
            }
        } else if (output.finish_reason == "truncated") {
            result.status = "partial"; result.error = "ovis_token_limit";
            result.format_override = "markdown";
        } else {
            result.status = "failed";
            result.error = !valid_utf8(output.error) ? "invalid_backend_error_utf8" :
                output.error.empty() ?
                (output.text.empty() ? "ovis_empty_output" : "ovis_inference_failed") :
                output.error;
        }
        if (!valid_utf8(output.raw_output) || !valid_utf8(output.text)) {
            result.status = "failed"; result.error = "invalid_backend_utf8";
            result.text.clear();
        }
        if (!valid_utf8(output.error)) {
            result.status = "failed"; result.error = "invalid_backend_error_utf8";
        }
        if (recognition.backend_unavailable && !recognition.reset_failed &&
            output.error != "generation_response_type_mismatch")
            result.error = "region_inference_exception:" + (valid_utf8(output.error) ?
                output.error : "invalid_utf8");
        if (result.status == "failed" ||
            (result.status != "ok" && result.assessment.state == "ok"))
            result.assessment.state = result.status == "failed" ? "failed" : "unverified";
        if (!result.error.empty()) result.assessment.reason = result.error;
    }
    if (type == "table" && result.status != "ok") {
        result.text.clear();
        result.table = {};
        result.format_override = "markdown";
    }
    return result;
}
BlockResult finalize_generic_recognition(const GenericRegionRecognition& recognition) {
    BlockResult result;
    result.status = "ok";
    const auto& generation = recognition.output;
    const auto& local_error = recognition.error;
    bool text_valid = valid_utf8(generation.text);
    bool raw_valid = valid_utf8(generation.raw_output);
    bool error_valid = valid_utf8(generation.error);
    if (raw_valid) result.raw = generation.raw_output;
    else result.raw_base64 = base64(generation.raw_output);
    if (!text_valid) result.text_base64 = base64(generation.text);
    if (!error_valid) result.error_base64 = base64(generation.error);
    if (!local_error.empty() || generation.finish_reason != "complete" ||
        !text_valid || !raw_valid || !error_valid) {
        result.status = recognition.skipped_after_backend_failure ? "skipped" :
                       generation.finish_reason == "truncated" ? "partial" : "failed";
        result.error = !local_error.empty() ? local_error :
                      !error_valid ? "invalid backend error encoding" :
                      (!text_valid || !raw_valid) ? "invalid backend UTF-8" :
                      generation.error.empty() ? "recognition incomplete" : generation.error;
    } else result.text = generation.text;
    return result;
}

}
