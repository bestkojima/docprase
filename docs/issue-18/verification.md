# Issue #18：小裁图视觉适配与无视觉拦截

## 冻结策略

固定 OvisOCR2 五输入 `idx_tensor` 路径的 smartresize 参数：factor 32、最小 65,536 像素、最大 16,777,216 像素、最大宽高比 200。先按运行时相同的取整和像素预算规则确定画布，再用**单一比例**缩放原裁图，整数尺寸四舍五入一次，剩余部分居中补白。画布已经是 32 的倍数且在预算内，因此运行时 smartresize 再次计算时目标尺寸不变；运行时仍负责颜色转换和归一化。原始裁图资源不被适配图替换。

开发比较记录来自 `output/resolution-diagnosis/REPORT.md`：323×31 图注的原图生成无效编号；323×32 单行补白、2 倍及 4 倍等比放大均正确得到 `Figure 1. Recorded readings.`。82×9 页眉原图产生无效编号，4 倍放大正确。此次补齐页眉 82×32 补白对照，结果为 `JEE (Advanced) 2023`；生产画布适配也得到相同文字。选择统一的等比缩放加补白策略，使两类短边不足 32 的裁图都满足运行时输入及预算条件，同时避免分别拉伸宽和高。极端宽高比超过 200 或无法满足预算时明确失败。

## 可复核样本与结果

原裁图固定在 `tests/fixtures/issue18/`：图注 SHA-256 `b5302505e069f237ee16564eb56dbb09abb016af0a872b813286340c5da44a05`，页眉 SHA-256 `04d2e094e83b8ae242008c50cebb146b606b59da9a142c2eb75a42961900ecbe`。使用同一份 `output/resolution-diagnosis/effective-config.json` 的 CPU1、greedy、512-token 配置和 `tests/fixtures/ovis/prompt.txt`；模型工件的固定哈希见 `configs/printed-page.example.json`。结果的耗时属于环境相关指标，不用作视觉成功证据。

| 输入 | 画布；内容；偏移 | 有效 `image_pad` token | 结果 |
| --- | --- | ---: | --- |
| 图注原图 323×31 | 无 | 0，虽然 `vision_us=10`、`pixels_mp=0.010013` | 生成前拦截 |
| 图注生产适配 | 832×96；832×80；(0,8) | 78 | 正常结束，`Figure 1. Recorded readings.` |
| 页眉仅补白到 82×32 | runtime 416×160 | 已有真实生成对照 | 正常结束，`JEE (Advanced) 2023` |
| 页眉生产适配 | 416×160；416×46；(0,57) | 65 | 正常结束，`JEE (Advanced) 2023` |

运行时的 `pixels_mp` 分别为 0.079872 和 0.06656，恰好等于适配画布的像素数；没有改变第二次输入几何。`dococr_visual_adaptation_probe` 还覆盖短边 1、31、32、正常 640×480、4,000×4,000 和宽高比超过 200 的失败情况，校验比例误差不超过整数取整界限。

重现命令（先配置并构建 CMake；有效配置由本机模型目录生成）：

```sh
build/dococr_visual_adaptation_probe tests/fixtures/issue18/caption-323x31.png /tmp/caption-adapted.png
build/dococr_visual_adaptation_probe tests/fixtures/issue18/header-82x9.png /tmp/header-adapted.png
build/dococr_issue18_token_probe output/resolution-diagnosis/effective-config.json /tmp/caption-adapted.png tests/fixtures/ovis/prompt.txt /tmp/caption-raw.txt
build/dococr_issue18_token_probe output/resolution-diagnosis/effective-config.json /tmp/header-adapted.png tests/fixtures/ovis/prompt.txt /tmp/header-raw.txt
```

## 作业与保存契约

生产后端先编码图像并预先取得视觉 token；`image_pad` 为 0 时不调用文本生成。DocumentIR 1.5 的图片区分于 PDF 结构，分别使用 `document-ir-1.5-image.schema.json` 与 `document-ir-1.5-pdf.schema.json`。区域 provenance 保存有效 token 数、证据类型、原裁图资源与页面 bbox、画布、内容尺寸、补白偏移和停止原因；资源始终是原裁图。失败和待核验块的 Markdown 展示原图及提示，原始输出只留在 JSON。JSON 重新导出验证视觉证据与画布，并复用相同展示规则。历史 1.1、1.2、1.3、1.4 保存文档仍按原 Schema 重新导出，缺失的视觉证据不会被补造。

公共作业入口的受控视觉空结果覆盖“处理计数非零、视觉 token 为零”时的局部失败、正常区域继续输出及保存后重新导出；真实原裁图探针复现计数非零而 token 为零。连续作业、取消和其他现有回归由完整 CTest 套件覆盖。

## 真实整页回归

`dococr_cli --config configs/printed-page.example.json --input tests/fixtures/issue15/printed_science_2col.png --out /tmp/issue18-real-page` 通过公共作业入口完成。DocumentIR 1.5 含 12 块，其中 11 块 `ok`、1 块图片 `skipped`；图注 `b0008` 的原定位为 `[714,659,1037,690]`，原始识别输出为 `Figure 1. Recorded readings.`，有效视觉 token 78。没有无效编号正文。使用 `--reexport` 对该 JSON 重新导出，JSON 与 Markdown 均逐字节一致。此结果是工程回归，不代表整页质量评分已经达标。

`python3 tests/job_control_real.py /tmp/docprase-issue18-build/libdococr_c.so /tmp/issue18-real-continuous` 在同一个公共引擎实例上连续完成两次教材页作业；每次 19 个区域，固定正文块 `b0003` 均匹配参考，原始输出 SHA-256 同为 `e1cf9ab07816f67591c568dacd5addd6789fa23e2770c8f4bc67544056c00546`，两次 DocumentIR SHA-256 也相同。两次作业终态均为 `partial`，引擎均保持可用。`partial` 包含原有非正文图片/局部状态，不在此处改称全页质量通过。
