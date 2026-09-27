# Issue #11 验证记录（Linux）

## 版本与命令

- 基线：`29efbaecc9fcc2e77eee6b19fcb420502ddd05ea`。构建：`cmake -S . -B /tmp/dococr-issue11 -DCMAKE_BUILD_TYPE=Release -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN`，随后 `cmake --build /tmp/dococr-issue11 -j4`；均退出 0。
- `ctest --test-dir /tmp/dococr-issue11 --output-on-failure`：12/12 通过，退出 0。包括新增 `pdf_integration`、`pdf_abi`，也覆盖原有图片作业回归。初次公共 PDF 用例在实现前因 CLI 不支持 `--pages` 失败；实现后通过。审查新增的重复 JSON 键与全页输出预算断言在修复前分别失败、修复后通过。
- `python3 tests/pdf_real.py /tmp/dococr-issue11/dococr_cli output/issue-11/real-pdf-final`：退出 0。脚本通过公共 CLI 以 `configs/printed-page.example.json` 的真实 Layout/Ovis MNN CPU 双模型处理第 1～2 页、200 DPI，并校验 DocumentIR 1.4、页号、全部导出资源、请求状态、Markdown 页序及逐行原文。原始运行清单、导出与机器可读摘要在 `output/issue-11/real-pdf-final/`。
- `pdfinfo -v` 与 `pdftoppm -v`：均为 Poppler 24.02.0；CMake、工具部署与 GPL 分发边界见 [README.md](README.md)。Windows 路径已有 `CreateProcessW` 实现，但本项仅在 Linux 实测；Windows 构建、依赖打包和运行仍待 #14。

## 真实 PDF 与处理结果

样本 `tests/fixtures/pdf/printed_textbook_2p.pdf` 是项目自行排版的两页中文教材正文，有可选择的 PDF 文本层和内嵌的 DroidSansFallback 字体子集，SHA-256 为 `13c682d9d850a23be468cfaec94b136ff67493f8792dac807425561a02cff1d7`。`pdftotext` 只用于检查已知真值，正式路径始终渲染后 OCR。样本可复核完整路径，不能代表出版教材总体质量。独立 `pdftoppm -f 1 -l 2 -r 200 -png` 参考渲染的两页 PNG SHA-256 分别为 `911868b7755bbef8dfa9fa7c76357c90792da06793a9e1b586f4ff4c9db502a2`、`7ee5933f34e92aeb783b4d2705c9e5875e1ade61f9c20877f2351a59185ae269`。

| 原页 | 页面状态 | 栅格像素 | 标题与正文逐行匹配 | 渲染 / 流水线 / 本页总耗时 |
| --- | --- | --- | --- | --- |
| 1 | `ok` | 1653×2339 | 5/5 | 136 / 12895 / 13043 ms |
| 2 | `ok` | 1653×2339 | 5/5 | 125 / 11633 / 11771 ms |

10 个文字块包括 2 个标题和 8 行正文；未检查印刷页码识别。10 个资源全部存在且名称唯一。`document.json` SHA-256 为 `a4345cda6a0c55fecea057868e68a6df9c76104aad860ba19fa11abe9114415f`；`document.md` 为 `f895e7f41a60146137079ee0bcbf8ab219526748ef3b27b390d8b0a3482c3bff`。清单的文档墙钟耗时为 24858 ms，Python 进程外层计时为 30.991 s，两者起止边界不同。

内存字段来自 Linux `/proc/self/status` 的 **父进程 VmRSS 快照**，并非峰值，不含 Poppler 子进程：第 1 页前/后为 1537798144 / 2351046656 字节，第 2 页前/后为 2351046656 / 2371117056 字节。`renderer_child_peak_bytes` 明确为 `null`；逐页预估像素均为 3870360。像素预算在渲染前按 CropBox、DPI、执行计划和核心上限检查；多页输出字节累计计数，超额页保留原页号、阶段审计与错误。

## 七项验收定位

| Issue 验收 | 实现与证据 |
| --- | --- |
| 公共作业/CLI、页范围、DPI、像素预算和逐页栅格化 | `DocOcrInput` 尾部扩展、`tools/cli.cpp`、`src/pdf_job.cpp`；公共测试用原页 2～3 范围及单页预算，旧 48 字节 ABI 测试。 |
| 可移植渲染器版本、构建和许可 | `src/pdf_renderer.cpp` 用参数数组启动 Poppler 工具；版本、Linux/Windows 部署及 GPL 说明见 README。 |
| 真实 Layout/Ovis、IR、导出及几何 | 上述真实双模型结果；DocumentIR 1.4 schema；非零 CropBox 加 90° 固定例核对 120×60 实际栅格和 PDF 点坐标仿射。 |
| 跨页稳定 ID/资源、页序与无跨页合并 | 固定测试比较只取第 2～3 页与全本对应页对象及资源完全一致，且 OCR 原文中出现 ID 字面量时保持原样。 |
| 有效/非法范围、空白、损坏/加密、超大、局部失败 | `tests/pdf_integration.py` 覆盖全部；10000×10000 PDF 点的固定超大页在渲染前因有效像素上限拒绝，未产生栅格；其他失败页保留 `page_id`、可用的栅格尺寸、错误与已执行阶段。 |
| 时间、内存与预算 | 逐页 geometry/render/pipeline/region/total 和文档总时、上述 RSS 口径、累计输出预算失败测试及 `run-manifest.json`。 |
| 真实文字多页 PDF 与复杂固定推理 | 上述 10/10 真实推理；固定推理覆盖版面关系、逐页失败、资源深层引用及 OCR 原文保真。公式/表格质量和高级页内排序仍由原任务验收。 |
