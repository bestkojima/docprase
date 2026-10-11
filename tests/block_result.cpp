#include "block_result.hpp"
#include "content_validation.hpp"
#include <iostream>
#include <stdexcept>

using namespace dococr;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
GenerationOutput complete(const std::string& text) {
    GenerationOutput output{text, text, "complete", "", "normal"};
    output.visual_evidence = "explicit_success";
    return output;
}
RegionRecognition attempt(GenerationOutput output) {
    RegionRecognition recognition;
    recognition.attempts.push_back({});
    recognition.attempts.back().output = std::move(output);
    return recognition;
}
BlockResult finish(const std::string& type, GenerationOutput output) {
    return finalize_region_recognition(type, attempt(std::move(output)));
}
void normalized_content() {
    auto text = finish("text", complete("价格 $5，设$x^2+1$。"));
    check(text.status == "ok" && text.error.empty() && text.raw == text.text,
          "正文与货币符号被误判或改写");
    auto formula = finish("formula", complete("  $$\n\\frac{a}{b}=c\n$$  "));
    check(formula.status == "ok" && formula.text == "\\frac{a}{b}=c" &&
          formula.format_override == "latex" && formula.display_formula &&
          formula.raw == "  $$\n\\frac{a}{b}=c\n$$  ", "公式规范化破坏来源");
    auto inline_formula = finish("formula", complete("$x=1$"));
    check(inline_formula.status == "ok" && !inline_formula.display_formula, "公式显示模式改变");
    auto mixed = finish("formula", complete("$a+b$，则"));
    check(mixed.status == "ok" && mixed.format_override == "markdown" &&
          mixed.text == "$a+b$，则" && !mixed.display_formula, "混合公式顺序丢失");
    auto table = finish("table", complete("<table><tr><td>合法$x=1$</td></tr></table>"));
    check(table.status == "ok" && table.table.valid && table.table.rows == 1 &&
          table.table.columns == 1 && table.text == table.table.html &&
          table.table.cells.front().text == "合法$x=1$", "表格结构规范化错误");
}
void syntax_and_table_isolation() {
    auto text = finish("text", complete("设$\\frac{a}$，求解。"));
    check(text.status == "partial" && text.assessment.state == "unverified" &&
          text.error == "invalid_inline_formula_syntax" && text.text == text.raw,
          "正文公式错误丢失原始文字");
    auto formula = finish("formula", complete("$$\\unknown{a}$$"));
    check(formula.status == "partial" && formula.error == "invalid_formula_syntax" &&
          formula.assessment.reason == formula.error && formula.format_override == "markdown" &&
          formula.text == formula.raw, "公式语法判定不一致");
    for (const auto& value : {"<table><tr><td>不完整", "<table><tr><td>$\\frac{a}$</td></tr></table>"}) {
        auto table = finish("table", complete(value));
        check(table.status == "partial" && table.assessment.state == "unverified" &&
              table.text.empty() && !table.table.valid && table.table.cells.empty() &&
              table.raw == value && table.format_override == "markdown", "失败表格暴露结构或丢失来源");
    }
}
void output_evidence() {
    auto output = complete("完整但无视觉证据的文字");
    output.visual_evidence.clear();
    auto no_vision = finish("text", output);
    check(no_vision.status == "failed" && no_vision.error == "visual_evidence_missing" &&
          no_vision.text.empty() && no_vision.raw == output.raw_output, "无视觉证据被误标正常");
    output = complete("截断的正文");
    output.finish_reason = "truncated"; output.stop_reason = "token_limit";
    auto truncated = finish("text", output);
    check(truncated.status == "partial" && truncated.assessment.state == "incomplete" &&
          truncated.error == "ovis_token_limit" && truncated.text == output.text, "截断判定改变");
    std::string repeated;
    for (int i = 0; i < 8; ++i) repeated += "Repeated phrase sentinel!\n";
    output = complete(repeated);
    auto suspect = finish("text", output);
    check(suspect.status == "partial" && suspect.assessment.state == "unverified" &&
          suspect.assessment.repeat_count >= 3 && suspect.error == "repeated_fragment", "重复证据丢失");
    output.finish_reason = "truncated"; output.stop_reason = "token_limit";
    auto anomalous = finish("text", output);
    check(anomalous.assessment.state == "anomalous" && anomalous.raw == repeated, "确认异常被截断或改写");
    output = complete("结果文字"); output.stop_reason.clear();
    check(finish("text", output).error == "unconfirmed_completion", "未知停止原因被标正常");
}
void encoding_and_error_precedence() {
    const std::string invalid(1, char(0xff));
    auto output = complete("正文"); output.raw_output = invalid;
    auto raw = finish("text", output);
    check(raw.status == "failed" && raw.error == "invalid_backend_utf8" &&
          raw.text.empty() && raw.raw.empty() && raw.raw_base64 == "/w==", "无效来源字节未保存");
    output = complete("正文"); output.text = invalid;
    auto text = finish("text", output);
    check(text.error == "invalid_backend_utf8" && text.text_base64 == "/w==" &&
          text.raw == "正文", "无效生成文字未保存");
    output.error = invalid;
    auto error = finish("text", output);
    check(error.error == "invalid_backend_error_utf8" && error.error_base64 == "/w==" &&
          error.text_base64 == "/w==" && error.assessment.reason == error.error,
          "错误字段未覆盖文字编码错误");
    output = complete("正文"); output.error = invalid;
    auto invalid_error = finish("text", output);
    check(invalid_error.status == "failed" && invalid_error.text == "正文" &&
          invalid_error.error == "invalid_backend_error_utf8", "错误字节处理改变正常文字记录");
    auto recognition = attempt(output); recognition.backend_unavailable = true;
    auto exception = finalize_region_recognition("text", recognition);
    check(exception.error == "region_inference_exception:invalid_utf8" &&
          exception.error_base64 == "/w==", "后端异常错误优先级改变");
    output = {"", "原始错误", "failed", "controlled_failure", "error"};
    recognition = attempt(output); recognition.backend_unavailable = true;
    auto backend = finalize_region_recognition("text", recognition);
    check(backend.error == "region_inference_exception:controlled_failure" &&
          backend.assessment.reason == backend.error && backend.raw == "原始错误", "异常上下文丢失");
    recognition.attempts.back().output.error = "region_reset_failed";
    recognition.reset_failed = true;
    check(finalize_region_recognition("text", recognition).error == "region_reset_failed", "reset 错误被异常前缀覆盖");
    recognition.reset_failed = false;
    recognition.attempts.back().output.error = "generation_response_type_mismatch";
    check(finalize_region_recognition("text", recognition).error == "generation_response_type_mismatch",
          "返回类型错误被异常前缀覆盖");
}
void selection_and_independence() {
    auto recognition = attempt(complete("第一次正文"));
    recognition.attempts.push_back({});
    recognition.attempts.back().output = complete("$x=1$");
    auto result = finalize_region_recognition("formula", recognition);
    check(result.status == "ok" && result.text == "x=1" && result.raw == "$x=1$" &&
          result.visual.raw_output.empty() && result.visual.text == "$x=1$", "选中的尝试或视觉记录错误");
    check(recognition.attempts.size() == 2 && recognition.attempts[0].output.text == "第一次正文" &&
          recognition.attempts.back().output.raw_output == "$x=1$", "结果判定修改了识别历史");
    auto missing = finalize_region_recognition("table", {});
    check(missing.status == "failed" && missing.error == "generation_response_type_mismatch" &&
          missing.assessment.stop_reason == "error" && missing.format_override == "markdown" &&
          missing.text.empty() && !missing.table.valid, "缺少识别尝试未形成可追溯失败");
    bool rejected = false;
    try { (void)finalize_region_recognition("image", recognition); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "图片被当成识别结果");
}
}
int main() {
    try {
        normalized_content(); syntax_and_table_isolation(); output_evidence();
        encoding_and_error_precedence(); selection_and_independence();
        std::cout << "区域结果：内容规范化、失败隔离、完成证据、字节保存与错误优先级检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
