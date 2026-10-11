#include "layout_output.hpp"
#include "dococr/dococr.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace dococr;
namespace {
using Row = std::array<float, 7>;
const Row normal{22, .9f, 1, 2, 80, 40, -1};
const LayoutDecodeOptions page{100, 50};
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> void put(std::vector<uint8_t>& bytes, size_t index, T value) {
    std::memcpy(bytes.data() + index * sizeof(T), &value, sizeof(value));
}
Tensor& tensor(TensorOutput& output, const std::string& name) {
    return *std::find_if(output.outputs.begin(), output.outputs.end(),
                        [&](const Tensor& t) { return t.name == name; });
}
TensorOutput fixture(const std::vector<Row>& rows = {normal}) {
    TensorOutput output{{
        {"fetch_name_0", DataType::Float32, TensorLayout::Matrix, {300, 7}, std::vector<uint8_t>(300 * 7 * 4)},
        {"fetch_name_1", DataType::Int32, TensorLayout::Matrix, {1}, std::vector<uint8_t>(4)},
        {"fetch_name_2", DataType::Int32, TensorLayout::Matrix, {300, 200, 200}, std::vector<uint8_t>(300 * 40000 * 4)}}};
    put(output.outputs[1].data, 0, int32_t(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i)
        std::memcpy(output.outputs[0].data.data() + i * 7 * 4, rows[i].data(), 7 * 4);
    return output;
}
TensorOutput generic_fixture(const std::vector<Row>& rows = {{0, .9f, 1, 2, 80, 40, 7}}) {
    TensorOutput output{{{"layout_candidates", DataType::Float32, TensorLayout::Matrix,
        {int64_t(rows.size()), 7}, std::vector<uint8_t>(rows.size() * 7 * sizeof(float))}}};
    for (size_t i = 0; i < rows.size(); ++i)
        std::memcpy(output.outputs[0].data.data() + i * 7 * sizeof(float), rows[i].data(), 7 * sizeof(float));
    return output;
}
void generic_tensor_contract() {
    for (int defect = 0; defect < 14; ++defect) {
        auto input = generic_fixture(); auto& t = input.outputs[0];
        switch (defect) {
        case 0: input.outputs.clear(); break;
        case 1: input.outputs.push_back(t); break;
        case 2: t.name = "fetch_name_0"; break;
        case 3: t.dtype = DataType::Int32; break;
        case 4: t.layout = TensorLayout::HWC; break;
        case 5: t.shape.clear(); break;
        case 6: t.shape = {1}; break;
        case 7: t.shape = {1, 7, 1}; break;
        case 8: t.shape[0] = -1; break;
        case 9: t.shape[0] = 10001; break;
        case 10: t.shape[1] = 6; break;
        case 11: t.data.pop_back(); break;
        case 12: t.data.push_back(0); break;
        case 13: t.shape[0] = 0; break;
        }
        check(!decode_generic_layout_output(input, 100, 50), "通用输出的非法张量契约被接受");
    }
    auto empty = decode_generic_layout_output(generic_fixture({}), 100, 50);
    check(empty && empty->empty(), "通用空页被当作解码失败");
    auto full = decode_generic_layout_output(generic_fixture(std::vector<Row>(10000,
        Row{0, 1, 0, 0, 100, 50, 0})), 100, 50);
    check(full && full->size() == 10000, "通用候选数量上限改变");
}
void generic_values_and_geometry() {
    const Row row{0, .9f, 1, 2, 80, 40, 7};
    for (size_t column = 0; column < 7; ++column)
        for (float value : {std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
            auto invalid = row; invalid[column] = value;
            check(!decode_generic_layout_output(generic_fixture({row, invalid}), 100, 50),
                  "通用数值非有限或部分结果被接受");
        }
    for (const auto& change : std::vector<std::pair<size_t, float>>{
             {0, -1}, {0, .5f}, {0, 1000001}, {1, -.1f}, {1, 1.1f},
             {6, -1}, {6, .5f}, {6, 1000001},
             {2, -1000001}, {3, -1000001}, {4, 1000001}, {5, 1000001},
             {2, 1.5f}, {3, 2.5f}, {4, 80.5f}, {5, 40.5f}}) {
        auto invalid = row; invalid[change.first] = change.second;
        check(!decode_generic_layout_output(generic_fixture({row, invalid}), 100, 50),
              "通用整数、范围或 rank=-1 契约改变");
    }
    for (const Box invalid : std::vector<Box>{{-1, 0, 10, 10}, {0, -1, 10, 10},
             {0, 0, 101, 10}, {0, 0, 10, 51}, {10, 0, 10, 10}, {0, 10, 10, 10},
             {11, 0, 10, 10}, {0, 11, 10, 10}}) {
        Row bad{0, .9f, float(invalid.x0), float(invalid.y0), float(invalid.x1), float(invalid.y1), 0};
        check(!decode_generic_layout_output(generic_fixture({row, bad}), 100, 50),
              "通用非法框被 clamp、跳过或返回部分候选");
    }
    auto input = generic_fixture({{0, 0, 0, 0, 1000000, 1000000, 0},
                                  {1000000, 1, 0, 0, 1000000, 1000000, 1000000}});
    const auto original = input.outputs[0].data;
    auto output = decode_generic_layout_output(input, 1000000, 1000000);
    check(output && output->size() == 2 && output->at(0).label == "text" &&
          output->at(0).detection_score == 0 && output->at(0).rank == 0 &&
          output->at(0).box.x1 == 1000000 && output->at(1).label == "unknown" &&
          output->at(1).class_id == 1000000 && output->at(1).rank == 1000000 &&
          input.outputs[0].data == original, "通用边界数值、零分候选或来源像素规则改变");
    input.outputs[0].data.assign(original.size(), 0);
    check(output->at(1).class_id == 1000000 && output->at(0).box.x1 == 1000000,
          "通用成功结果依赖调用方张量寿命");
}
void generic_order_and_labels() {
    // 分别固定每一级前置条件，模型 rank/score 与几何顺序相反。
    const std::vector<std::pair<Row, Row>> pairs{
        {{0, .1f, 20, 1, 80, 40, 100}, {0, 1, 0, 2, 10, 10, 0}},
        {{0, .1f, 1, 2, 80, 40, 100}, {0, 1, 2, 2, 10, 10, 0}},
        {{0, .1f, 1, 2, 80, 10, 100}, {0, 1, 1, 2, 10, 20, 0}},
        {{0, .1f, 1, 2, 10, 40, 100}, {0, 1, 1, 2, 80, 40, 0}}};
    for (const auto& pair : pairs) {
        auto output = decode_generic_layout_output(generic_fixture({pair.second, pair.first}), 100, 50);
        check(output && output->size() == 2 && output->at(0).rank == 100 &&
              output->at(0).detection_score == .1f && output->at(1).rank == 0,
              "通用几何排序层级或 rank/score 使用规则改变");
    }
    auto output = decode_generic_layout_output(generic_fixture({
        {99, .5f, 1, 2, 80, 40, 0}, {0, 0, 1, 2, 80, 40, 1},
        {2, .1f, 1, 2, 80, 40, 2}, {3, .2f, 1, 2, 80, 40, 3},
        {1, .3f, 1, 2, 80, 40, 4}}), 100, 50);
    check(output && output->size() == 5, "通用类别或低分候选被过滤");
    const std::vector<std::string> labels{"formula", "image", "table", "text", "unknown"};
    const std::vector<int> classes{1, 3, 2, 0, 99};
    for (size_t i = 0; i < labels.size(); ++i)
        check(output->at(i).label == labels[i] && output->at(i).class_id == classes[i],
              "通用标签映射或最后排序层级改变");
}
class GenericBackend final : public IInferenceEngine {
public:
    TensorOutput output;
    bool wrong_payload = false;
    int generation_calls = 0;
    explicit GenericBackend(TensorOutput tensors) : output(std::move(tensors)) {}
    bool load(const BackendLoadSpec&) override { return true; }
    std::vector<ArtifactInfo> loaded_artifacts() const override { return {}; }
    const EngineCapabilities& capabilities() const override { return capabilities_; }
    std::string profile() const override { return "fixture:generic-layout"; }
    bool reset() override { return true; }
    void unload() override {}
    InferenceResponse execute(const InferenceRequest& request, ExecutionContext&) override {
        if (std::holds_alternative<TensorRequest>(request.payload)) {
            if (wrong_payload) return {GenerationOutput{}};
            return {std::move(output)};
        }
        ++generation_calls;
        return {GenerationOutput{"正文", "正文", "complete", "", "normal"}};
    }
private:
    const EngineCapabilities capabilities_{true, true, true, 1};
};
void generic_page_handoff() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        auto tensors = scenario == 1 || scenario == 5 ? generic_fixture({}) : generic_fixture();
        if (scenario == 2) tensors.outputs[0].shape[1] = 6;
        if (scenario == 3) put(tensors.outputs[0].data, 4, 101.f);
        GenericBackend backend(std::move(tensors));
        backend.wrong_payload = scenario == 4;
        std::atomic_bool cancelled{false};
        std::vector<uint8_t> pixels(100 * 50 * 3, 127);
        std::vector<std::string> events;
        const auto result = run_page(&backend,
            {pixels.data(), pixels.size(), DOCOCR_IMAGE_RGB8, 100, 50, 100 * 3},
            cancelled, nullptr, 0,
            [&](const char* stage, uint32_t, const std::string&, uint32_t, uint32_t) {
                events.emplace_back(stage);
                if (scenario != 5 && events.back() == "layout_completed") cancelled = true;
            });
        const std::vector<std::string> decoded_events{
            "decode_completed", "layout_started", "layout_completed"};
        if (scenario <= 1) {
            check(result.code == RunCode::Cancelled && result.did_decode && result.did_layout &&
                  result.did_recognition == (scenario == 0) && !result.did_export &&
                  result.output.json.empty() && events == decoded_events,
                  "layout_completed 回调取消的空/非空页交接改变");
        } else if (scenario <= 4) {
            const std::vector<std::string> expected = scenario == 4 ?
                std::vector<std::string>{"decode_completed", "layout_started"} : decoded_events;
            check(result.code == RunCode::Failed && !result.did_decode && !result.did_layout &&
                  !result.did_recognition && result.page_pixels == 0 && result.output.json.empty() &&
                  result.error_code.empty() && events == expected,
                  "非法输出的完成事件、fresh Failed 审计或取消优先级改变");
        } else {
            check(result.code == RunCode::Blank && result.did_decode && result.did_layout &&
                  !result.did_recognition && result.did_export && !result.output.json.empty() &&
                  events == std::vector<std::string>{"decode_completed", "layout_started", "layout_completed",
                                                     "export_started", "export_completed"},
                  "通用空页成功导出或识别标志改变");
        }
        check(backend.generation_calls == 0, "取消、坏输出或空页启动了区域识别");
    }
}
void mask(TensorOutput& output, size_t row, size_t pixel, int32_t value) {
    put(tensor(output, "fetch_name_2").data, row * 40000 + pixel, value);
}
void tensor_contract() {
    for (size_t i = 0; i < 3; ++i) {
        for (int defect = 0; defect < 4; ++defect) {
            auto input = fixture(); auto& t = input.outputs[i];
            if (defect == 0) t.dtype = DataType::UInt8;
            if (defect == 1) ++t.shape[0];
            if (defect == 2) t.layout = TensorLayout::HWC;
            if (defect == 3) t.data.pop_back();
            check(!decode_layout_output(std::move(input), page), "错误张量契约被接受");
        }
    }
    for (int defect = 0; defect < 4; ++defect) {
        auto input = fixture();
        if (defect == 0) input.outputs.pop_back();
        if (defect == 1) input.outputs.push_back({"extra", DataType::Int32, TensorLayout::Matrix, {0}, {}});
        if (defect == 2) input.outputs[1].name = "fetch_name_0";
        if (defect == 3) input.outputs[1].name = "unknown";
        check(!decode_layout_output(std::move(input), page), "缺失、重复或额外输出被接受");
    }
    for (int32_t count : {-1, 301}) {
        auto input = fixture(); put(input.outputs[1].data, 0, count);
        check(!decode_layout_output(std::move(input), page), "非法候选数量被接受");
    }
    auto full = decode_layout_output(fixture(std::vector<Row>(300, normal)), page);
    check(full && full->candidates.size() == 300 && full->candidates.back().id == 299,
          "最大候选数量未保留原行身份");
    std::array<int, 3> order{0, 1, 2};
    do {
        auto input = fixture(); auto source = std::move(input.outputs);
        for (int index : order) input.outputs.push_back(std::move(source[size_t(index)]));
        auto output = decode_layout_output(std::move(input), page);
        check(output && output->candidates.size() == 1 && output->mask_row(0).pixel(0) == 0,
              "输出张量顺序影响解码");
    } while (std::next_permutation(order.begin(), order.end()));
}
void numeric_contract_and_priority() {
    for (const auto& change : std::vector<std::pair<size_t, float>>{
             {0, -.1f}, {0, .5f}, {0, 1000001.f}, {1, -.1f}, {1, 1.1f},
             {6, -2.f}, {6, -.5f}, {6, 1000001.f},
             {2, std::numeric_limits<float>::infinity()},
             {4, std::numeric_limits<float>::quiet_NaN()}}) {
        auto row = normal; row[change.first] = change.second;
        check(!decode_layout_output(fixture({row}), page), "非法候选数值被接受");
    }
    auto output = decode_layout_output(fixture({
        {22, .5f, -.3f, 1.2f, 100.2f, 49.1f, -1},
        {22, .49f, 20, 20, 10, 10, 0},
        {22, 1, 20, 20, 10, 10, 0},
        {22, 1, 100000000, 20, 100000008, 30, 0},
        {22, 1, -10000001, 20, 10, 30, 0},
        {1000000, 1, 1, 2, 3, 4, 1000000},
        {0, 1, -10000000, 20, 10, 30, 0}}), page);
    check(output && output->candidates.size() == 7, "合法数值或未知类别被拒绝");
    const auto& c = output->candidates;
    check(c[0].selected && c[0].clamped && c[0].rank == -1 &&
          c[0].crop.x0 == 0 && c[0].crop.y0 == 1 && c[0].crop.x1 == 100 && c[0].crop.y1 == 50,
          "阈值等号、浮点裁框或 rank=-1 行为改变");
    check(c[1].reason == "below_score_threshold" && c[2].reason == "degenerate_box" &&
          c[3].reason == "outside_page" && c[4].reason == "box_out_of_supported_range" &&
          c[5].label == "unknown" && c[5].selected && c[6].selected,
          "筛除原因优先级或坐标范围边界改变");
    auto options = page; options.score_threshold = 0;
    auto zero = normal; zero[1] = 0;
    check(decode_layout_output(fixture({zero}), options)->candidates[0].selected,
          "合法零分阈值被拒绝");
    options.transform = {true, 200, 100, 0, 0, 0, 2};
    check(!decode_layout_output(fixture(), options), "回映后的非有限坐标被接受");
}
void all_masks_and_unused_rows() {
    for (const auto& position : {std::pair<size_t, size_t>{0, 0}, {1, 123}, {299, 39999}}) {
        auto input = fixture(); mask(input, position.first, position.second, 2);
        check(!decode_layout_output(std::move(input), page), "非法 mask 位或未使用行被忽略");
    }
    auto low = normal; low[1] = .1f;
    auto input = fixture({low}); mask(input, 0, 123, -1);
    check(!decode_layout_output(std::move(input), page), "低分候选的非法 mask 被忽略");
    input = fixture({}); mask(input, 299, 39999, -1);
    check(!decode_layout_output(std::move(input), page), "空页未校验全部 mask");
    input = fixture({}); mask(input, 299, 39999, 1);
    put(input.outputs[0].data, 0, std::numeric_limits<float>::quiet_NaN());
    auto output = decode_layout_output(std::move(input), page);
    check(output && output->candidates.empty() && output->mask_row(299).pixel(39999) == 1,
          "空页错误限制尾部浮点值或非零 mask");
    input = fixture(); mask(input, 0, 123, 1); mask(input, 0, 39999, 1);
    put(input.outputs[0].data, 299 * 7, std::numeric_limits<float>::quiet_NaN());
    output = decode_layout_output(std::move(input), page);
    check(output && output->candidates[0].mask_nonzero == 2, "mask 计数或未用浮点行规则改变");
}
void ownership_and_original_evidence() {
    const Row model{22, .9f, 18, 30, 48, 70, 7};
    auto input = fixture({model}); mask(input, 0, 123, 1);
    const auto* original_buffer = input.outputs[2].data.data();
    auto options = page; options.transform = {true, 200, 100, 10, 20, 2, 2};
    auto output = decode_layout_output(std::move(input), options);
    check(output && output->candidates[0].box[0] == 4 && output->candidates[0].box[1] == 5 &&
          output->candidates[0].crop.x1 == 19 && output->candidates[0].crop.y1 == 25 &&
          output->candidates[0].model_box[0] == 18 &&
          output->candidate_row_bytes(0) == std::string_view(reinterpret_cast<const char*>(model.data()), 28),
          "回映坐标与原始模型证据混淆");
    auto moved = std::move(*output);
    auto candidates = std::move(moved.candidates);
    check(reinterpret_cast<const uint8_t*>(moved.mask_row(0).bytes().data()) == original_buffer &&
          moved.mask_row(0).pixel(123) == 1 && moved.candidate_row_bytes(0).size() == 28 &&
          candidates[0].mask_nonzero == 1, "移动交接复制了 mask 或使来源失效");
    auto target = decode_layout_output(fixture(), page);
    *target = std::move(moved);
    check(target->mask_row(0).pixel(123) == 1 && target->candidate_row_bytes(0).size() == 28,
          "移动赋值后来源失效");
    for (int access = 0; access < 3; ++access) {
        bool rejected = false;
        try {
            if (access == 0) (void)target->mask_row(300);
            if (access == 1) (void)target->mask_row(0).pixel(40000);
            if (access == 2) (void)target->candidate_row_bytes(1);
        } catch (const std::out_of_range&) { rejected = true; }
        check(rejected, "来源访问越界未被拒绝");
    }
}
void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
void rle_contract() {
    auto input = fixture(); mask(input, 0, 0, 1); mask(input, 0, 39999, 1);
    for (size_t pixel = 0; pixel < 40000; ++pixel) {
        mask(input, 1, pixel, 1); mask(input, 2, pixel, int32_t(pixel % 2));
    }
    mask(input, 299, 0, 1);
    auto output = decode_layout_output(std::move(input), page);
    check(bool(output), "合法填充 mask 被拒绝");
    const std::string magic = "DOCOCR_MASK_RLE_V1\n";
    std::vector<uint8_t> expected(magic.begin(), magic.end());
    for (uint32_t value : {300, 200, 200, 1, 3, 1, 39998, 1, 1, 1, 40000, 0, 40000})
        append32(expected, value);
    for (int i = 0; i < 40000; ++i) append32(expected, 1);
    for (int row = 3; row < 299; ++row)
        for (uint32_t value : {0, 1, 40000}) append32(expected, value);
    for (uint32_t value : {1, 2, 1, 39999}) append32(expected, value);
    check(output->masks_rle() == expected, "RLE 原始格式、首尾位或未使用行改变");
}
}
int main() {
    static_assert(!std::is_copy_constructible_v<DecodedLayoutOutput>);
    try {
        generic_tensor_contract(); generic_values_and_geometry(); generic_order_and_labels();
        generic_page_handoff();
        tensor_contract(); numeric_contract_and_priority(); all_masks_and_unused_rows();
        ownership_and_original_evidence(); rle_contract();
        std::cout << "版面输出：通用契约、几何顺序及 PP 回映、完整 mask、原始证据、移动与 RLE 检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
