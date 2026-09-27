# Issue #11：多页 PDF 逐页文档作业

## 使用

```bash
dococr_cli --config configs/printed-page.example.json \
  --input tests/fixtures/pdf/printed_textbook_2p.pdf --out output/pdf-job \
  --pages 1-2 --dpi 200 --max-page-pixels 16000000
```

`--pages` 使用原 PDF 中从 1 开始、两端均包含的页号；省略时处理全部页面。`--dpi` 范围为 36～600，默认 150。`--max-page-pixels` 是可选的逐页上限。公共 C ABI 使用 `DOCOCR_DOCUMENT_PDF` 和追加于 `DocOcrInput` 尾部的 `first_page`、`last_page`、`dpi`、`max_page_pixels`；0 表示默认。旧版 48 字节输入结构仍可调用，新字段仅在 `struct_size` 覆盖它们时读取。

输出为 `document.json`（DocumentIR 1.4）、`document.md`、`assets/`、`execution-plan.json` 与 `run-manifest.json`。未提供配置的 PDF 作业仍导出 PDF 运行清单。图片作业继续采用原有 1.0～1.3 契约。1.4 的 JSON 约束见 [document-ir-1.4.schema.json](document-ir-1.4.schema.json)。

## 渲染器、构建和许可

使用 Poppler 24.02.0 的独立 `pdfinfo`、`pdftoppm` 可执行程序。Linux 验证环境中两者 `-v` 均报告 24.02.0；作业启动时也核对二者版本一致，清单记录实际版本。运行时从 `DOCOCR_POPPLER_BIN` 指定的目录寻找工具；未设置时从系统可执行文件搜索路径寻找。Linux 以 `posix_spawn[p]` 的参数数组启动，Windows 代码以 `CreateProcessW` 宽字符参数启动；不经过 shell。输入 PDF 暂存于独立的私有临时目录，逐页使用 `pdfinfo -f/-l/-box` 读取几何，再以 `pdftoppm -f/-l/-singlefile/-cropbox/-r/-png` 渲染；临时目录随作业释放。

Windows 集成需准备同一版本的 `pdfinfo.exe`、`pdftoppm.exe`、配套 Poppler DLL 和编码数据，设置 `DOCOCR_POPPLER_BIN` 指向其目录，再运行本仓库的构建及 PDF 样例。Poppler 24.02.0 源码的 CMake `ENABLE_UTILS=ON` 会构建命令行工具，PNG 支持须在配置结果中启用；工具依赖的运行库及字体环境须随部署核对。Poppler 项目列有 Windows 构建 CI，但本 Issue 只在 Linux 实测，Windows 构建/运行由 #14 验收。参考：[Poppler 项目与下载](https://poppler.freedesktop.org/)、[24.02.0 CMake 选项](https://gitlab.freedesktop.org/poppler/poppler/-/blob/poppler-24.02.0/CMakeLists.txt)、[Poppler README 许可说明](https://gitlab.freedesktop.org/poppler/poppler/-/blob/poppler-24.02.0/README.md)。

Poppler 受 GPL 许可约束。本仓库没有将 Poppler 源码或库链接进 `dococr_core`，运行时通过独立进程调用工具；这项进程边界本身**不能**免除 Poppler 二进制及其依赖在复制、分发时的许可义务，也不能单凭进程边界断言所有组合分发方式的许可结论。部署方若随产品分发 Poppler，应核对对应 GPL 版本、源码提供及通知义务。JSON 结构化聚合使用仓库内的 nlohmann/json 3.12.0 单头文件，许可为 [MIT](../../third_party/nlohmann/LICENSE.MIT)，源码 SHA-256 为 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`。

## 页几何、顺序与失败

每页 `page_id` 与块、区域、版面块 ID 均使用原页号命名空间，例如 `p0002-b0001`；图片资源、mask、原始张量、overlay、请求 ID 也按该页命名。只取第二页与处理全本时，第二页的页对象和资源名一致。聚合只按原页号追加页面，不合并跨页段落或表格，不替换 OCR 原文中的 ID 字符串。Markdown 在每页正文前加入原页号标题。

PDF 使用 CropBox 渲染。`pdf_crop_box_points` 保留 PDF 原始坐标中的 `[x0,y0,x1,y1]`，`pdf_rotation_degrees` 保留页面旋转；`pdf_page_size_points` 是旋转后的显示宽高。`pdf_points_to_raster_affine` 为 `[a,b,c,d,e,f]`，约定 `raster_x=a*pdf_x+c*pdf_y+e`、`raster_y=b*pdf_x+d*pdf_y+f`，栅格原点在左上。坐标采用名义 `dpi/72` 比例；整数像素边界仍以 `raster_size` 与块框为准。非零 CropBox `[20,30,80,150]` 加 90° 的 72 DPI 固定例实际得到 `120×60` 栅格，矩阵为 `[0,1,1,0,-30,-20]`。

先从 CropBox 尺寸与 DPI 向上取整并加一像素安全余量，取请求预算、执行计划预算及核心 16,000,000 像素限制的最小值，在渲染前拒绝超额页；实际解码仍执行原有尺寸校验。`max_output_bytes` 对多页作业累计计算，超额页显示失败占位并继续记录其已执行阶段。有效页的图像和推理结果逐页释放或转入最终资源，不同时解码全部页面。空白页标为 `blank`，可继续的单页异常保留原页索引、`failed` 状态、错误和清单审计；所有选中页都失败时公共作业返回错误并保留运行清单。损坏、加密、非法页范围在推理前给出明确错误。

`run-manifest.json` 的 `pdf.pages[]` 记录逐页几何、渲染、流水线、区域耗时与执行状态，并有文档总墙钟时间。Linux 内存字段是从 `/proc/self/status` 读取的作业父进程 `VmRSS` 页前/页后快照，**不是峰值**，也不包含 Poppler 子进程；`renderer_child_peak_bytes` 为 `null`。外部 `/usr/bin/time -v` 可补充进程级最大 RSS，口径与页快照不同。原生 PDF 文字抽取只用于验证样本真值，不参与正式 OCR 路径。

## 验证样本与边界

[printed_textbook_2p.pdf](../../tests/fixtures/pdf/printed_textbook_2p.pdf) 是项目自写的两页中文印刷教材内容，使用 ReportLab 5.0.1 排版并嵌入 DroidSansFallback TrueType 子集；`pdffonts` 可核对 `emb/sub/uni=yes`，`pdftotext` 可核对可选文本。生成脚本为 [make_textbook.py](../../tests/fixtures/pdf/make_textbook.py)，原文真值在 [pdf_real.py](../../tests/pdf_real.py)。字体源文件 `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf` 在本机 SHA-256 为 `acb6440a713d880a13a21b468ba7cd43f5a2b2934972e51be791c880730777b8`；已生成的 PDF 自带所用字形子集，运行不依赖该字体源文件。字体版权及 Apache 2.0 许可通知见 [DROID_FONT_NOTICE.md](../../tests/fixtures/pdf/DROID_FONT_NOTICE.md)。样本是可控排版，不代表真实出版教材的字体、版式和质量分布。

固定推理例 [pdf_integration.py](../../tests/pdf_integration.py) 覆盖复杂版面关系、原页范围、资源和深层引用、空白/局部失败、加密/损坏输入、预算与非零 CropBox/旋转；[pdf_abi.py](../../tests/pdf_abi.py) 核对旧/新 C ABI 输入。真实双模型例对两页逐句比对 2 个标题加 8 行正文，并验证源页号、资源与清单；公式、表格专门质量和高级页内排序仍由各自任务承担。完整实测数据与命令记录在 `output/issue-11/real-pdf-final/`。
