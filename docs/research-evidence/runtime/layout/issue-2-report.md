# Issue #2：PP-DocLayoutV3-MNN 版面契约诊断

日期：2026-09-26。仅覆盖 Linux CPU 单页 Layout 诊断。`diagnostic_passed` 表示本入口已通过静态、真实运行、参考前处理及已测几何契约；原模型权重输出一致性、业务质量和 Windows 仍为 `not_verified`，不借此宣布父规格完成。

## 运行与依赖

在仓库根目录使用本机明确的 Python 环境：

```bash
/home/dr/project/google_edge/litert-env/bin/python scripts/model_probe_layout.py \
  --model models/doclayout/PP-DocLayoutV3.mnn \
  --image tests/fixtures/layout/exam-jee-346.jpg \
  --mnn /home/dr/project/MNN \
  --out output/layout-issue-2
/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests
```

本轮环境：Python 3.11.16、NumPy 2.4.6、Pillow 12.3.0、OpenCV 4.11.0、PyTorch 2.13.0+cu130、torchvision 0.28.0+cu130、Transformers 5.16.1。为直接执行固定参考处理器，使用 `uv pip install --python /home/dr/project/google_edge/litert-env/bin/python --no-deps 'torchvision==0.28.0'` 增加了 torchvision；未升级 PyTorch。入口验证已导入参考处理器源码的 SHA-256，不匹配会失败。链接已有 `../MNN/build/libMNN.so`，用现有 `GetMNNInfo`；小 C++ 探针只按公共头文件构建到输出目录，不重建 MNN。缺模型、坏哈希、契约/数值不符返回非零，并尽可能留下报告、差异和命令日志。未知 `--image` 不套用 JEE 人工标注；显式 `--reference` 必须含匹配图片的 `image_sha256`。

## 锁定来源

- MNN 模型 `models/doclayout/PP-DocLayoutV3.mnn`，SHA-256 `5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67`。MNN 源码 commit `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6`，实际链接库 `MNN::getVersion()` 为 3.6.1。
- [Transformers 参考处理器固定提交](https://github.com/huggingface/transformers/blob/27166ea03f12c940f23176a904ab1d2ff1a3dcbb/src/transformers/models/pp_doclayout_v3/image_processing_pp_doclayout_v3.py)：`27166ea03f12c940f23176a904ab1d2ff1a3dcbb`，源码 SHA-256 `5b064fa7383dda12b3550448eae77d4f627a102c25e8b4db25d99e85fba4abc6`；本机安装的同名文件哈希相同。参考 [PaddlePaddle 模型修订](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3_safetensors/tree/97d101e6db2642e162a1d05392d1b0231c91033e) `97d101e6db2642e162a1d05392d1b0231c91033e` 的 [25 类配置](../../../../tests/fixtures/layout/reference-model-config.json) 与 [前处理配置](../../../../tests/fixtures/layout/reference-preprocessor-config.json)；文件 SHA-256 分别为 `3cf834b91d23a756b1519bce4db42c09e852f3e35c35092dd5a3e253a50c071a`、`519fe0187a43a1ca429e3ad8317bab8700f0d5e8fb3a6e3a0a413ffac078ba42`。这些配置与 MNN 图的 25 类和已观察语义相符，但没有 MNN 转换链记录，不能证明两套权重输出逐元素等价。
- 人工业务参考：[HiLEx 试卷数据集固定提交](https://github.com/HiLEx-DLA/HiLEx/tree/3bb962636c14b2630d308a77df2b3f5b768bb434) `3bb962636c14b2630d308a77df2b3f5b768bb434`。项目声明 CC BY 4.0 和专家人工标注。取 `HiLEx_Coco_Format/test/2288acd7-JEE_346_jpg.rf.dfdce22db409f96a0773f4b07b058e7b.jpg`（SHA-256 `e8d587b83baade2dbdb3ad3333cfe8bc9a7d9cbf489de4db961058b23343dade`），并从同目录 `_annotations.coco.json` 抽取 image id 169 的五个分层框到 [本地参考](../../../../tests/fixtures/layout/exam-jee-346-reference.json)。这些框由数据集提供，不是代理自行标注。样本 640×640 版本可能把原页面纵横比压缩，结论只适用于此图版本。

## 静态、真实运行及参考数值

`GetMNNInfo` 静态列出 `image` float32 NCHW `[1,3,800,800]`、`im_shape` float32 `[1,2]`、`scale_factor` float32 `[1,2]`；真实 Session 再核对这三项及输出 `fetch_name_0` float32 `[300,7]`、`fetch_name_1` int32 `[1]`、`fetch_name_2` int32 `[300,200,200]`。试卷原图 `[640,640]`，输入 `im_shape=[800,800]`、`scale_factor=[1.25,1.25]`。第二次重用同一 `image.f32`，只改为 `[0.625,0.625]`。`report.json` 分别标记静态、运行、参考前处理、参考模型输出和业务质量状态。

固定处理器直接以 PIL RGB 图像调用 `PPDocLayoutV3ImageProcessor(images=..., return_tensors='pt')`。实际喂 MNN 的候选张量独立调用 torchvision v2 `resize(BICUBIC, antialias=False)`，按 RGB、NCHW、float32、1/255、mean=0、std=1 构造。与**实际参考处理器**逐像素比较，最大/平均绝对差都是 0；冻结最大容差 `1e-7`。另记录 OpenCV `INTER_CUBIC` 与参考最大差 `0.10588235`（约 27/255）、平均差 `0.00013360`，Pillow BICUBIC 最大差 `0.07450981`（约 19/255）、平均差 `0.00068288`。它们均不能替代本轮选定路径，也未放宽容差。用 `--preprocess opencv` 可重现前处理失败并保留差异报告。这个通过只涉及输入数值；没有同一权重的 Paddle/ONNX 原始输出用于比较。

`fetch_name_0` 七列为类别 ID、分数、四角框、rank；`fetch_name_1=300` 是 TopK 原始候选数。试卷页 300 行全部有限，mask 是 int32 且仅含 0/1。依参考处理器默认分数阈值 `0.5`，本例安全筛选保留 16 行，284 行记录 `below_score_threshold`，没有越界或退化框；**16 不是通用断言**。原始 `candidates.json` 为所有行保留 `candidate_id`、`mask_row`、框、分数、rank 和过滤原因。每行 ID 和 mask 行号相同，rank 仅为排序线索；本页 rank 只有 118 个不同值，182 次重复且不连续。[按参考 rank 排列的视图](issue-2/selected-reference-order.json) 保留原候选 ID。

同一图像张量的第二轮类别/分数/rank 差 0，四坐标均精确变为 2 倍，完整 mask 数组逐像素相同，证明此 MNN 图已按 `im_shape/scale_factor` 恢复页面框，外层不应再次除以缩放比例。两轮原始候选、候选数、**完整** int32 mask 及输入张量均存为可解压 gzip 工件；`report.json` 的 `raw_masks` 指向随仓库保存的 `.gz` 路径，`runtime_mask_files` 指向单次运行目录中的原始 `.bin`。

参考处理器会把二值 200×200 mask 按 `800/width/4`、`800/height/4` 换算后在框内裁剪，最近邻放大，并提取轮廓/多边形。本诊断调用**实际固定处理器**的 `_extract_polygon_points_by_masks`，截取它内部产生的 resized mask patch，将 patch 放回原页，与独立产生的页面 mask 逐像素比较；16 个保留候选最大差异像素 0，参考多边形和逐行差异见 [mask-reference-comparison.json](issue-2/mask-reference-comparison.json)，页面 mask PNG 见 [page-masks](issue-2/page-masks/)。非方形旧技术样本 `[1419,2000]` 单独运行也得到 0 差异，并记录 46 个越界原始候选；见 [nonsquare-report.json](issue-2/nonsquare-report.json)。后者没有人工参考，不能作为业务质量证据。

类别配置按 0–24 共 25 个 ID，部分名称重复（`footer`、`header`、`formula`、`text`），ID 不可由名称唯一反推。参考处理器执行 sigmoid、类别×query TopK、分数阈值和 rank 排序，没有 NMS；本入口不额外加 NMS。本页选中框同类两两 IoU>0.5 为 0，不能据一页确定所有业务图都无需去重。对低分、未知类、退化框、越界框的诊断安全筛选另记录原因，不改写原始数组；它比参考处理器纯分数过滤更严格。

## 人工参考与结论边界

HiLEx 框是试卷区域和两道题的分层父块，MNN 类别是文字、公式、图表等较细视觉元素，不能按类直接算准确率。[对照图](issue-2/overlay.jpg) 用红框表示模型候选、蓝框表示人工父块，[几何报告](issue-2/business-reference-comparison.json) 列出每个父框与细粒度检测框并集的覆盖率。两题父框覆盖约 40.2% 与 16.8%；第二题的化学反应图未被框出。结果可复核，业务质量保持 `not_verified`。未执行同权重原模型输出对齐、Windows 或文档核心。

## 失败证据与验收矩阵

真实命令、有效参数、哈希、版本、退出码和张量清单见 [report.json](issue-2/report.json)，阶段日志在同目录。缺模型、错误模型哈希、OpenCV 前处理参考失败分别退出 1，保留 [missing-model-report.json](issue-2/missing-model-report.json)、[wrong-model-report.json](issue-2/wrong-model-report.json)、[reference-fail-report.json](issue-2/reference-fail-report.json)。截断真实 `image.f32` 到 4 字节后直接运行同一 C++ 探针，输出 `input_tensor_size_mismatch`，退出 8，见 [input-mismatch.log](issue-2/input-mismatch.log)；这验证输入张量长度分支，没有冒称另一个 MNN 图签名的实测。完整测试日志在 [tests.log](issue-2/tests.log)。

| #2 条件 | 状态与证据 |
| --- | --- |
| 1. 可复跑入口、模型/运行清单 | **通过**：上述命令；`report.json`、阶段日志、哈希及实际 MNN 库版本。 |
| 2. 三输入约定及分层状态 | **通过**：`static-info.log`、C++ 真实张量检查，五种检查状态分别列出。 |
| 3. 固定参考颜色/归一化/resize | **通过输入前处理**：实际固定 HF 处理器调用、源码哈希检查、0 差异，OpenCV/Pillow 差异及 `1e-7` 容差留证。 |
| 4. 25 类、筛选、框-mask、原页、NMS、人工业务图 | **通过本项可核查契约**：固定 25 ID 配置和图类数对应、逐行原始/选中数据、实际 HF mask patch 零差异、参考阈值/NMS 语义、HiLEx 人工父框对照。MNN 转换链、业务质量不在已验证项。 |
| 5. scale_factor、越界与 rank | **通过**：两轮框精确 2 倍；非方形旧技术图 46 个越界候选；本页 rank 重复/不连续，稳定 ID 是行号。 |
| 6. 原始输出、筛选原因、类型/值域 | **通过**：两轮完整 `.gz` 原始数据、300 行 `candidates.json`、有限性及二值 mask 检查；无通用“26 框”断言。 |
| 7. 失败留证 | **通过已测分支**：缺模型/哈希/参考失败退出 1 并留 JSON；输入长度错误退出 8 并留日志；签名不符有代码拒绝但无另图实测。 |
| 8. 范围 | **通过**：只交付 Layout 诊断和报告，没有实现文档核心/Ovis。 |
