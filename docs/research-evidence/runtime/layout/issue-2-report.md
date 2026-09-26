# Issue #2：PP-DocLayoutV3-MNN 版面契约诊断

日期：2026-09-26。该报告只覆盖 Linux CPU 上的单页 Layout 诊断，不是文档解析核心或 Windows 验收。`diagnostic_passed` 仅表示本入口的静态检查、真实运行、参考前处理数值检查和几何不变量通过；转换前模型输出一致性与业务质量仍为 `not_verified`。

## 可独立复跑

在仓库根目录执行，使用已经包含 NumPy 2.4.6、Pillow 12.3.0、PyTorch 2.13.0、OpenCV 4.11.0 的明确环境：

```bash
/home/dr/project/google_edge/litert-env/bin/python scripts/model_probe_layout.py \
  --model models/doclayout/PP-DocLayoutV3.mnn \
  --image tests/fixtures/layout/exam-jee-346.jpg \
  --mnn /home/dr/project/MNN \
  --out output/layout-issue-2
/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -p 'test_model_probe_layout.py'
```

入口只链接现有 `../MNN/build/libMNN.so`，使用现有 `GetMNNInfo`；C++ 小探针按公共头文件编译到输出目录，不重建 MNN。若缺少模型、运行库或工具、模型哈希不符、张量契约不符、输出数值非法或参考前处理差异超过容差，退出非零并尽可能保留 `report.json`、已产生的输入、差异及命令日志。`--image` 可替换；未知图片不会自动套用本例人工标注。显式 `--reference` 必须含匹配图片的 `image_sha256`，不匹配时拒绝。

## 来源与锁定版本

- 指定 MNN 模型：`models/doclayout/PP-DocLayoutV3.mnn`，SHA-256 `5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67`。MNN 源码 commit `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6`；运行库报告 3.6.1。未取得 MNN 工件的转换链记录，不能据外观相似断言它与下述 Hugging Face 权重逐层等价。
- 参考处理器：[Transformers 固定提交的 `image_processing_pp_doclayout_v3.py`](https://github.com/huggingface/transformers/blob/27166ea03f12c940f23176a904ab1d2ff1a3dcbb/src/transformers/models/pp_doclayout_v3/image_processing_pp_doclayout_v3.py)，源码 SHA-256 `5b064fa7383dda12b3550448eae77d4f627a102c25e8b4db25d99e85fba4abc6`，本机安装的 Transformers 5.16.1 文件哈希相同。处理器规定 800×800、BICUBIC、`antialias=False`、零 mean、单位 std、重缩放；本任务未执行完整 Transformers 模型。其参考配置来自 [PaddlePaddle 固定修订](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3_safetensors/tree/97d101e6db2642e162a1d05392d1b0231c91033e)，保存在 [reference-preprocessor-config.json](../../../../tests/fixtures/layout/reference-preprocessor-config.json) 和 [reference-model-config.json](../../../../tests/fixtures/layout/reference-model-config.json)。文件 SHA-256 分别是 `519fe0187a43a1ca429e3ad8317bab8700f0d5e8fb3a6e3a0a413ffac078ba42`、`3cf834b91d23a756b1519bce4db42c09e852f3e35c35092dd5a3e253a50c071a`。
- 人工参考：[HiLEx 试卷数据集](https://github.com/HiLEx-DLA/HiLEx/tree/3bb962636c14b2630d308a77df2b3f5b768bb434)，固定 commit `3bb962636c14b2630d308a77df2b3f5b768bb434`，项目声明 CC BY 4.0、专家人工标注。抽取其 `HiLEx_Coco_Format/test/2288acd7-JEE_346_jpg.rf.dfdce22db409f96a0773f4b07b058e7b.jpg`（SHA-256 `e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade`）及同目录 `_annotations.coco.json` 中 image id 169 的五个框到 [exam-jee-346-reference.json](../../../../tests/fixtures/layout/exam-jee-346-reference.json)。没有把模型推断或本代理目视判断冒充人工真值。样本是 640×640 正方形版本，原页面纵横比可能已被压缩；仅用于此版本的定位核对。

## 契约和数值结果

静态 `GetMNNInfo` 与真实 Session 共同核对三个输入：`image` float32 NCHW `[1,3,800,800]`、`im_shape` float32 `[1,2]`、`scale_factor` float32 `[1,2]`。真实 Session 还核对 `fetch_name_0` float32 `[300,7]`、`fetch_name_1` int32 `[1]`、`fetch_name_2` int32 `[300,200,200]`。完整清单和日志见 [本次工件](issue-2/)。本例 `im_shape=[800,800]`，原页 `[640,640]`，`scale_factor=[1.25,1.25]`。第二轮复用同一 `image.f32`，只把 `scale_factor` 变为 `[0.625,0.625]`。

输入按 RGB 解码、OpenCV `INTER_CUBIC` 到 800×800、转 NCHW float32、除以 255；没有额外 mean/std 变换。参考数值由独立的 PyTorch bicubic `align_corners=False`、无抗锯齿路径计算，与固定处理器所述的 torchvision 路径相对应。容差冻结为输入 `[0,1]` 空间最大绝对差 `1/255 + 1e-7`。本页 OpenCV 对参考最大差 `0.0039215684`、平均差 `1.10e-7`、不同像素占比 `0.0028125%`，通过。Pillow BICUBIC 对参考最大差 `0.09803921`、平均差 `0.00071733`、不同像素占比 `7.046875%`；因此不能沿用旧 Pillow 冒烟作为数值对齐。颜色约定来自参考处理器输入 RGB 和本次明确转换；转换前权重来源仍未证实。数值只证明前处理这一步，不能推导模型检测结果与参考权重一致。

`fetch_name_0` 的列为类别 ID、分数、四角框、rank；`fetch_name_1=300` 是 TopK 原始候选数。本页 300 行全部有限、mask 每值均为 0/1。以参考处理器默认分数阈值 `0.5`，本次安全筛选保留 16 行，284 行记录 `below_score_threshold`，没有越界/退化框；**16 是此页结果，不是通用断言**。每行用 `candidate_id`/`mask_row` 指向原始数组同一行，rank 只作为模型顺序线索。全体候选 rank 仅 122 个不同值，有 178 次重复且不连续。旧技术样本中 50 个越界候选的记录可见 [先前证据](README.md)；本任务的合成边界测试也验证越界原因和重复 rank，不把旧样本当业务验收。

同图张量、同 `im_shape` 的第二轮中，类别/分数/rank 差为 0，所有框坐标恰为 2 倍，两个完整 mask 数组逐像素相同。这支持图内已按 `im_shape/scale_factor` 恢复原页坐标，外层不得再次除比例。原始三个输出及两轮完整 mask 已保存在工件中，完整原始数组以 gzip 包装，解压后分别为 float32/int32 小端；不是只存非零计数。

参考处理器按原页宽高把 200×200 mask 坐标缩为 `800/width/4`、`800/height/4`，在候选框内裁剪，`INTER_NEAREST` 放大到原页框，再提取轮廓/多边形。本诊断另将同一 patch 放进原页大小二值图，以便核查。16 个真实保留候选与单独按固定参考公式算出的页面图逐像素比较，最大差异像素数为 0；单元测试覆盖非方形页面和越界裁剪。页面 mask PNG 存在 [page-masks](issue-2/page-masks/)。该验证不等于多边形轮廓质量或人工 mask 真值已验证。

类别配置有 25 个独立 ID，名称允许重复（例如 `footer`、`header`、`formula`、`text`），不能按名称反推唯一 ID。参考处理器执行 sigmoid、类别×query TopK、分数阈值和 rank 排序，没有 NMS；本次不额外加 NMS。保留框中同类两两 IoU 超过 0.5 的对数为 0。上层业务是否需要去重仍需跨样本验证，不能从本页推出通用结论。诊断安全筛选另外标记未知类、低分、退化及越界，不改写原始候选；若未来接入选择不同策略，原始证据仍在。

## 人工参考对照及局限

HiLEx 人工框是试卷区域和两道题的分层父块；模型 25 类是文字、公式、图表等视觉元素，两者不具一对一类别对应。原图和红色检测/蓝色人工框对照见 [overlay.jpg](issue-2/overlay.jpg)，数字结果见 [business-reference-comparison.json](issue-2/business-reference-comparison.json)。两个题目父块的检测框并集覆盖约 40.2% 和 16.8%；第二题的化学反应图未被框出。这个结果可供人工复核，不能称作 25 类准确率或质量验收通过。`reference_model_output` 与 `business_quality` 均保持 `not_verified`。参考版本只核对前处理和后处理语义，尚无同一权重的 Paddle/ONNX 原始输出可比。

## 失败路径与文件

真实运行命令及退出码记录于 [report.json](issue-2/report.json) 与 `static-info.log`、`build.log`、`normal.log`、`scale_factor_half.log`。此外执行三个负例：缺模型、错误模型哈希、将参考容差强制为 0，均退出 1；证据为 [missing-model-report.json](issue-2/missing-model-report.json)、[wrong-model-report.json](issue-2/wrong-model-report.json)、[reference-fail-report.json](issue-2/reference-fail-report.json)。输入/输出名称、形状和类型不符会由 C++ 探针拒绝；本轮没有另一份改签名的真实 MNN 模型来演练这一分支。重跑会覆盖指定 `--out` 的同名诊断工件，请为不同输入使用不同输出目录。

补充输入负例：用 `head -c 4 output/layout-issue-2/image.f32 > output/layout-issue-2/truncated-image.f32` 制造长度错误的 float32 图像输入，再直接调用已编译探针：

```bash
output/layout-issue-2/model_probe_layout_runner \
  models/doclayout/PP-DocLayoutV3.mnn \
  output/layout-issue-2/truncated-image.f32 \
  output/layout-issue-2/input-mismatch 640 640 1
```

实际退出码 8，输出 `input_tensor_size_mismatch`，见 [input-mismatch.log](issue-2/input-mismatch.log)。它证明输入字节长度错误会被拒绝；没有把它冒称为另一份 MNN 图签名错误的真实模型试验。

## Issue #2 验收矩阵

| 条件 | 本轮证据与状态 |
| --- | --- |
| 1. 可复跑入口与运行清单 | **通过**：上方命令；`report.json` 记录命令、模型/样本哈希、MNN 3.6.1、CPU/4 线程、阈值、状态；命令日志逐项记录退出码。 |
| 2. 三输入类型与验证层级 | **通过**：静态 `GetMNNInfo`、真实 Session 的输入/输出张量检查；`checks` 分开列静态、运行、前处理对齐、原模型参考输出及业务质量。 |
| 3. 参考颜色、归一化、resize | **通过前处理数值**：固定源码与配置、RGB/1÷255、OpenCV 对独立 Torch 最大 1/255；Pillow 最大约 25/255。原权重输出对齐仍未验证。 |
| 4. 25 类、筛选、框-mask、原页、NMS、业务样本 | **部分通过**：25 ID 配置、按候选行关联完整 mask、参考映射像素差 0、默认阈值与无 NMS、HiLEx 人工父框及可视化均有证据。MNN 转换来源未证实，业务质量与多边形轮廓未验收。 |
| 5. scale_factor 与边界/rank | **通过**：同一张量两次坐标 2× 残差 0；本页 rank 重复且不连续；越界通过回归测试和旧原始样本覆盖，候选 ID 为行号。 |
| 6. 原始与筛选原因、值域 | **通过**：两轮原始三个输出、完整 mask、300 条 `candidates.json` 和原因；float 有限、int32 mask 仅 0/1；无固定“26 框”断言。 |
| 7. 失败留痕 | **通过已测分支**：缺模型、错误哈希、参考容差不通过均退出 1 且保存 JSON；截断输入探针退出 8 并保存日志。另一 MNN 图签名错误由代码检查，但未取得变签名模型实测。 |
| 8. 范围 | **通过**：仅 Layout 诊断、样本和报告，无文档核心/Ovis。 |
