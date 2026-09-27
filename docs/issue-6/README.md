# Issue #6：真实 Layout 公共作业

本项只接入 PP-DocLayoutV3 的单页图片版面检测。区域转写由后续任务接入，当前所有导出区域的内容状态均为 `skipped`，页面为 `partial`，区域图片和空文本绝不表示 OCR 成功。空候选数页面为 `blank`；模型给出候选但全部被筛掉时为 `partial`，便于审核遗漏。Linux CPU 已实测；Windows、PDF、多页、业务检测准确率和转写质量均未由本项验证。

## 构建与运行

CMake 使用 `DOCOCR_MNN_ROOT` 定位 MNN **公共头文件**与已构建的 `libMNN.so`。本地默认路径为 `../MNN`，MNN 版本 3.6.1。缺少 MNN 时仍可构建无模型核心与测试库；真实配置创建会显式失败。模型文件始终是外部工件，示例配置只接受锁定 SHA-256 `5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67`。生产必须使用 JSON 配置入口，不能用裸 `--backend mnn:pp-doclayout-v3` 绕过工件绑定。

```sh
cmake -S . -B /tmp/dococr-issue6 -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN
cmake --build /tmp/dococr-issue6 -j2
ctest --test-dir /tmp/dococr-issue6 --output-on-failure
/tmp/dococr-issue6/dococr_cli --config configs/layout-plan.example.json \
  --input tests/fixtures/layout/exam-jee-346-pil-rgb.png --out /tmp/dococr-layout
/home/dr/project/google_edge/litert-env/bin/python tests/layout_preprocess_reference.py \
  /tmp/dococr-issue6/dococr_layout_preprocess_probe
/home/dr/project/google_edge/litert-env/bin/python tests/layout_real_integration.py \
  /tmp/dococr-issue6/dococr_cli /tmp/dococr-issue6/dococr_layout_jpeg_decode_probe \
  /tmp/dococr-issue6-evidence --direct-probe /tmp/dococr-issue6/dococr_layout_mnn_probe
```

输入 `image` 为 RGB float32 NCHW `[1,3,800,800]`，`im_shape=[800,800]`，`scale_factor=[800/原高,800/原宽]`；输出保留 `fetch_name_0` float32 `[300,7]`、`fetch_name_1` int32 `[1]`、`fetch_name_2` int32 `[300,200,200]`。输入 bicubic 的 uint8 定点权重、逐轴舍入和 `antialias=false` 路径按 [PyTorch 固定版本的公共实现](https://github.com/pytorch/pytorch/blob/v2.13.0/aten/src/ATen/native/cpu/UpSampleKernel.cpp)实现，并独立对照[锁定的 HF 处理器](https://github.com/huggingface/transformers/blob/27166ea03f12c940f23176a904ab1d2ff1a3dcbb/src/transformers/models/pp_doclayout_v3/image_processing_pp_doclayout_v3.py)。OpenCV 或 Pillow resize 的手写近似不能作为通过依据。

`tests/fixtures/layout/exam-jee-346-pil-rgb.png` 是从原始 JEE JPEG 通过 Pillow `convert('RGB')` 保存的**无损输入像素固定件**，SHA-256 为 `3f092c959987cc81e83a68d554361d07fce95b08a003cb23ed0ae323e9cd4817`；原 JPEG SHA-256 为 `e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade`。保留原文件。PNG 只用于隔离 JPEG 解码差异并同 #2 输入张量对照。

## 导出契约

`document.json` 维持 DocumentIR 1.0，并可选加入 `layout_diagnostics`。候选列表按原始行号保存 `candidate_id`、`mask_row`、类别 ID/名称、分数、**原始浮点框**、rank、mask 非零数、筛选原因、裁剪框及是否钳制。25 个类别 ID 按固定参考表映射；重复名称保留原始 ID。可输出内容类型映射为 `text/formula/table/image/unknown`，未知类区域保留并标 `unknown_layout_class`。默认分数阈值 0.5，不叠加 NMS。坐标已经在图内恢复，外层不再除以 `scale_factor`。区域裁剪用原图 RGB，浮点框向外取整后钳制到页面；mask 页图按参考处理器的整型框、200×200 网格裁剪及最近邻映射。原始框不改写。极端大框在整数转换前过滤为 `box_out_of_supported_range`。

块 ID 与候选行号分离；重复 rank 保持唯一 ID。当前 DocumentIR 1.0 的阅读顺序沿用稳定几何排序，`reading_order_source=geometry`；rank 仅保存为模型元数据，并非复杂版面阅读顺序质量证明。所有选中区域均导出 `assets/p0001-bNNNN.png`；独立的页 mask 图 `assets/p0001-mask-cN.png` 和红框/绿 mask 叠加图 `assets/p0001-layout-overlay.png` 由诊断字段引用。原图不因 mask 着色而修改。正常区域的 `error=recognition_not_executed`，`status=skipped`，`confidence=null`。

公共 `dococr_job_asset` 与 CLI 同时导出六个原始张量工件：`image.f32`、`im_shape.f32`、`scale_factor.f32`、`fetch_name_0.f32`、`fetch_name_1.i32`、`fetch_name_2.rle`；实际名称前带 `p0001-`。前五项为小端原始字节。mask RLE 是**无损**编码，格式为 ASCII `DOCOCR_MASK_RLE_V1\n`，随后三个小端 uint32（300、200、200），每行依次为初始 bit、run 数及各 run 长度的小端 uint32。每行长度和必须为 40000，bit 在 0/1 间交替；按行反解可恢复完整的 300×200×200 int32 原始 mask，包括低分和被过滤行。`layout_diagnostics.raw_tensor_assets` 给出全部路径，不靠 mask 非零计数充当原始证据。

## 本机验收结果

完整机器可读摘要及各命令的退出码/标准输出/错误输出见 [evidence/real-report.json](evidence/real-report.json) 与本目录 `evidence/*.log`。验证命令均退出 0；期望失败的 CLI 命令分别退出 3。执行计划与运行清单仍由公共 CLI 写入所选输出目录；运行清单记录配置哈希、模型工件哈希、MNN 版本、CPU、阶段状态与耗时。

| 核对项 | 实测结果 |
| --- | --- |
| 固定 HF 前处理 | 锁定页、2×2、1×1、135×73、800×640、640×800、800×800 共 7 例，C++ 与真实处理器 `max_abs=0`，严于冻结容差 `1e-7`。 |
| 真实公共作业 | 无损 PNG：300 原始候选、16 选中区域、39 个工件，作业 `partial`，16 块全 `skipped`，转写阶段 `not_run`，DocumentIR schema 验证通过。 |
| 输入与 #2 | 公共作业导出 `image.f32` 与 #2 已存参考张量逐元素相同；`im_shape=[800,800]`、`scale_factor=[1.25,1.25]`。 |
| 同参数原始输出 | 独立 MNN 探针以公共作业输入张量、1 线程运行；`fetch_name_0` 的 8400 字节、`fetch_name_1` 的 4 字节完全相等，300×200×200 mask 反解后零差。探针可选 `THREADS` 参数默认仍为原 #2 的 4。 |
| 与旧 #2 四线程基线 | 16 个选中 ID/类别/rank 一致，选中框最大坐标差 0.000244140625，选中原始 mask 行零差，16 张页 mask 与 #2 图片逐像素零差。7 个低分 mask 行共 1676 像素不同；低分候选也有 TopK 顺序差异，不声称所有 300 行跨线程字节相等。 |
| JPEG 解码隔离 | 同 JPEG 的 stb 与 PIL 解码最大通道差 1，1491 个通道分量/497 像素有差；直接 JPEG 作业选中 17，PIL 像素 PNG 为 16，故不把两种解码输入混同。 |
| 受控公共作业 | 部分越界钳制、极端大框过滤、空候选、未知类、重复 rank、合法包含关系、全部 RLE 行反解和受控推理失败均通过。 |
| 失败路径 | 缺工件 `artifact_missing`、哈希错误 `artifact_hash_mismatch`、伪模型 `layout_artifact_contract_mismatch` 均 CLI 退出 3；受控 Layout 推理抛错事件 stage=`layout`、清单 `failure_code=layout_inference_failed`、CLI 退出 3，不回退其他后端。 |

本次未对有效模型强制制造 MNN `createSession` 或 `runSession` 内部故障；这些分支按阶段返回明确代码，但没有宣称实测。JPEG 直接运行可用，不过解码细微差异足以改变本样本临界候选数。HiLEx 人工标注的业务检测准确率仍为 `not_verified`；区域文本、公式和表格转写由 #7 接入。
