#include "image_resources.hpp"
#include "dococr/dococr.h"
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace dococr;
namespace resources = dococr::image_resources;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::array<uint8_t, 3> pixel(const Image& image, int x, int y) {
    const auto offset = (size_t(y) * image.width + x) * 3;
    return {image.rgb[offset], image.rgb[offset + 1], image.rgb[offset + 2]};
}
Image solid(int width, int height, std::array<uint8_t, 3> color) {
    Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 3)};
    for (size_t i = 0; i < image.rgb.size(); ++i) image.rgb[i] = color[i % 3];
    return image;
}
Image png_image(const std::vector<uint8_t>& png) {
    auto decoded = resources::decode({png.data(), png.size(), DOCOCR_IMAGE_PNG, 0, 0, 0}, 64000000);
    check(bool(decoded), "模块生成的 PNG 无法解码");
    return std::move(*decoded);
}
void input_and_crop_contract(const std::string& fixtures) {
    const std::vector<uint8_t> padded{1, 2, 3, 4, 5, 6, 99, 99, 7, 8, 9, 10, 11, 12};
    const InputView input{padded.data(), padded.size(), DOCOCR_IMAGE_RGB8, 2, 2, 8};
    auto decoded = resources::decode(input, 4);
    check(decoded && decoded->rgb == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}),
          "RGB 步长、末行长度或 padding 行为改变");
    auto invalid = input; invalid.size = 13;
    check(!resources::decode(invalid, 4), "不完整末行被接受");
    invalid = input; invalid.row_stride = 5;
    check(!resources::decode(invalid, 4) && !resources::decode(input, 3), "步长或像素预算未检查");
    invalid = input; invalid.data = nullptr;
    check(!resources::decode(invalid, 4), "空输入指针被接受");
    const std::vector<uint8_t> gray{10, 20, 99, 99, 30, 40};
    auto monochrome = resources::decode({gray.data(), gray.size(), DOCOCR_IMAGE_GRAY8, 2, 2, 4}, 4);
    check(monochrome && monochrome->rgb == std::vector<uint8_t>({10,10,10,20,20,20,30,30,30,40,40,40}),
          "灰度输入未正确展开为 RGB");
    const auto before = decoded->rgb;
    const auto cropped = resources::crop(*decoded, {1, 0, 2, 2});
    check(cropped.width == 1 && cropped.height == 2 &&
          cropped.rgb == std::vector<uint8_t>({4, 5, 6, 10, 11, 12}) && decoded->rgb == before,
          "裁图内容错误或修改原图");
    const auto png = resources::crop_png(*decoded, {1, 0, 2, 2});
    check(png_image(png).rgb == cropped.rgb, "RGB 裁图与 PNG 像素不一致");
    for (const auto& box : {Box{-1,0,1,1}, {0,0,3,2}, {1,1,1,2}, {1,1,0,2}}) {
        bool rejected = false;
        try { (void)resources::crop(*decoded, box); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "非法裁框未拒绝");
    }
    auto truncated = *decoded; truncated.rgb.pop_back();
    bool rejected = false;
    try { (void)resources::crop(truncated, {0,0,1,1}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "不完整 RGB 缓冲区未拒绝");
    for (const auto& entry : {std::pair<const char*, uint32_t>{"rgb2x2.png", DOCOCR_IMAGE_PNG},
                             {"rgb2x2.jpg", DOCOCR_IMAGE_JPEG}}) {
        std::ifstream file(fixtures + "/" + entry.first, std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        auto image = resources::decode({bytes.data(), bytes.size(), entry.second, 0, 0, 0}, 4);
        check(image && image->width == 2 && image->height == 2, "有效 PNG/JPEG 被拒绝");
        check(!resources::decode({bytes.data(), 20, entry.second, 0, 0, 0}, 4), "截断编码图片被接受");
        check(!resources::decode({bytes.data(), bytes.size(), entry.second, 2, 0, 0}, 4),
              "编码图片的原始尺寸字段未拒绝");
        const auto other = entry.second == DOCOCR_IMAGE_PNG ? DOCOCR_IMAGE_JPEG : DOCOCR_IMAGE_PNG;
        check(!resources::decode({bytes.data(), bytes.size(), uint32_t(other), 0, 0, 0}, 4),
              "编码格式和文件头不匹配未拒绝");
    }
    for (int field : {24, 25}) {
        auto unsupported = png;
        unsupported[size_t(field)] = field == 24 ? 16 : 6;
        check(!resources::decode({unsupported.data(), unsupported.size(), DOCOCR_IMAGE_PNG, 0, 0, 0}, 4),
              "16-bit 或 alpha 图片未拒绝");
    }
}
using Row = std::array<float, 7>;
DecodedLayoutOutput layout(const std::vector<Row>& rows, const LayoutDecodeOptions& options,
                           const std::vector<std::pair<size_t, size_t>>& points, bool full = false) {
    TensorOutput input{{
        {"fetch_name_0", DataType::Float32, TensorLayout::Matrix, {300,7}, std::vector<uint8_t>(300*7*4)},
        {"fetch_name_1", DataType::Int32, TensorLayout::Matrix, {1}, std::vector<uint8_t>(4)},
        {"fetch_name_2", DataType::Int32, TensorLayout::Matrix, {300,200,200}, std::vector<uint8_t>(300*40000*4)}}};
    const int32_t count = int32_t(rows.size()), one = 1;
    std::memcpy(input.outputs[1].data.data(), &count, 4);
    for (size_t i = 0; i < rows.size(); ++i) {
        std::memcpy(input.outputs[0].data.data() + i*28, rows[i].data(), 28);
        if (full) for (size_t j = 0; j < 40000; ++j)
            std::memcpy(input.outputs[2].data.data() + (i*40000+j)*4, &one, 4);
    }
    for (const auto& point : points)
        std::memcpy(input.outputs[2].data.data() + (point.first*40000+point.second)*4, &one, 4);
    auto decoded = decode_layout_output(std::move(input), options);
    check(bool(decoded), "测试版面输出契约不符");
    return std::move(*decoded);
}
void reference_mask_and_overlay() {
    LayoutDecodeOptions options{8, 8};
    auto decoded = layout({{22,.9f,-.8f,.5f,5.9f,6.7f,0}}, options, {{0,75*200+50}});
    const resources::LayoutRasterSource source{8,8,decoded,options.transform};
    const auto png = resources::layout_mask_png(decoded.candidates[0], source);
    const auto mask = png_image(png);
    check(mask.width == 8 && mask.height == 8, "mask PNG 未保留原页尺寸");
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        check(pixel(mask,x,y) == (x == 2 && y == 3 ? std::array<uint8_t,3>{255,255,255} :
                                                   std::array<uint8_t,3>{0,0,0}),
              "reference 浮点截断、nearbyint 或整数回映改变");
    check(png == resources::layout_mask_png(decoded.candidates[0], source,
                   resources::MaskPurpose::CandidateReview), "复核与诊断 mask 字节不一致");
    options = {4,4};
    auto two = layout({{22,.9f,0,0,4,4,0},{23,.8f,0,0,4,4,1}}, options, {}, true);
    const resources::LayoutRasterSource repeated{4,4,two,options.transform};
    const auto original = solid(4,4,{200,100,50});
    const auto overlay = png_image(resources::layout_overlay_png(original,two.candidates,repeated,{}));
    check(pixel(overlay,1,1) == std::array<uint8_t,3>{50,215,12} &&
          pixel(overlay,0,0) == std::array<uint8_t,3>{255,0,0} &&
          pixel(original,1,1) == std::array<uint8_t,3>{200,100,50},
          "叠加顺序、绿色混合、红边框或输入不变性改变");
    for (auto& c : two.candidates) c.selected = false;
    check(png_image(resources::layout_overlay_png(original,two.candidates,repeated,{})).rgb == original.rgb,
          "被筛除的候选仍绘入叠加图");
}
void smart_mask_and_preview() {
    LayoutDecodeOptions options{8,8,{true,16,16,4,8,2,2}};
    auto decoded = layout({{22,.9f,6,10,18,22,0}}, options, {{0,4*200+2}});
    const resources::LayoutRasterSource source{8,8,decoded,options.transform};
    const auto mask = png_image(resources::layout_mask_png(decoded.candidates[0],source));
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        check(pixel(mask,x,y)[0] == (x >= 2 && x <= 3 && y >= 4 && y <= 5 ? 255 : 0),
              "smart mask 的像素中心回映或裁框改变");
    const auto preview = solid(800,800,{0,0,254});
    const auto original = solid(8,8,{255,255,255});
    const auto overlay = png_image(resources::layout_overlay_png(original,decoded.candidates,source,preview));
    check(overlay.width == 800 && overlay.height == 800 &&
          pixel(overlay,9,17) == std::array<uint8_t,3>{0,127,127} &&
          pixel(overlay,6,10) == std::array<uint8_t,3>{255,0,0} &&
          pixel(overlay,1,1) == std::array<uint8_t,3>{0,0,254} &&
          pixel(preview,9,17) == std::array<uint8_t,3>{0,0,254},
          "smart overlay 未按 preview、模型框或直接 mask 采样绘制");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        input_and_crop_contract(argv[1]); reference_mask_and_overlay(); smart_mask_and_preview();
        std::cout << "图像资源：输入、裁图、PNG、两类 mask 回映与叠加图检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
