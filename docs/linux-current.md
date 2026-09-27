# 当前 Linux 版本：本机运行指南

本指南从仓库根目录运行已有的生产 `dococr_cli`，处理 JPEG、PNG 或 PDF。当前配置使用 CPU 上的 PP-DocLayoutV3 版面模型与 OvisOCR2 识别模型。命令中的相对模型路径按**启动 CLI 时的工作目录**解析，因此先进入仓库根目录：

```sh
cd /home/dr/project/docprase
```

## 准备与构建

需要 C++17 编译器、CMake 3.20 及以上版本；本机 MNN 工程位于 `../MNN`，其 `build/` 中已构建 `libMNN.so` 和 `libllm.so`。`configs/printed-page.example.json` 引用本仓库内已有的 `models/doclayout/` 和 `models/ovis/`，并固定了九个模型工件的 SHA-256；运行前须保留这些本地模型文件，不把权重提交进仓库。配置选择 `mnn:pp-doclayout-v3+ovisocr2`、CPU、1 线程。PDF 另需同版本的 `pdfinfo` 与 `pdftoppm`，默认从 `PATH` 寻找，也可用 `DOCOCR_POPPLER_BIN` 指向二者所在目录。

在上述本机目录布局中，以下构建命令已实际运行成功。两个 `REQUIRE` 选项使缺少 MNN 或 LLM 依赖时配置直接失败：

```sh
cmake -S . -B build/linux-current -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_REQUIRE_MNN=ON \
  -DDOCOCR_REQUIRE_LLM=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN
cmake --build build/linux-current -j4
ctest --test-dir build/linux-current --output-on-failure
```

入口是 `build/linux-current/dococr_cli`，无需 Python 服务。此构建在当前工作树上完成，`ctest` 为 15/15；构建和测试日志保存在 [output/linux-current/](../output/linux-current/)。可执行文件链接本机 MNN 构建目录中的动态库；移动到其他机器前须另行准备同一运行时与模型。

## 解析自己的文件

以下两条是**路径模板**，把输入与输出路径替换为自己的路径；建议每次使用新的输出目录。带空格或中文的路径用引号包住。

```sh
# JPEG 或 PNG
build/linux-current/dococr_cli --config configs/printed-page.example.json \
  --input '/绝对路径/自己的图片.jpg' --out 'output/自己的图片作业'

# PDF：页号从 1 开始，两端包含；省略 --pages 则处理全部页面
build/linux-current/dococr_cli --config configs/printed-page.example.json \
  --input '/绝对路径/自己的文档.pdf' --out 'output/自己的PDF作业' \
  --pages 1-2 --dpi 200
```

原始作业目录包含 `document.json`（DocumentIR）、`document.md`、`assets/`、`execution-plan.json`、`run-manifest.json`、`job-status.json` 和 `job-events.jsonl`。JSON 保留块状态、来源与原始模型输出；Markdown 是便于阅读的展示，不能替代 JSON 判读。查看 `document.status` 与各 `pages[].blocks[].status`：`partial` 表示仍有可用内容，但部分区域可能被截断或未成功识别。CLI 退出码 0 表示作业和导出完成，不保证每块文字完全正确。PDF 参数只适用于 PDF；`--dpi` 允许 36～600，也可用 `--max-page-pixels` 限制逐页像素数。

## 从已有 JSON 重新导出

以下命令是**路径模板**；`--out` 必须指向尚不存在、且不与资源根重叠的新目录：

```sh
build/linux-current/dococr_cli --reexport '旧作业/document.json' \
  --asset-root '旧作业' --out '新导出目录'
```

重新导出读取原 JSON 和资源，复制 JSON 原始字节与所引用的资源，并重新生成 Markdown；不会再次运行模型或 PDF 渲染。新目录包含 `document.json`、`document.md` 和 `assets/`，不生成新的作业状态或运行清单。已保存文档支持 DocumentIR 1.0～1.4。

## 本次实测结果

以下固定样本命令已在本机通过生产 CLI 实际执行，均退出 0。真实识别验证脚本分别为 `tests/printed_page_real.py` 和 `tests/pdf_real.py`；输出目录已有结果，若重跑须换新目录。本节链接指向当前工作树的本机证据；`output/` 被 Git 忽略，不随文档提交，新检出须运行命令生成自己的结果。

```sh
build/linux-current/dococr_cli --config configs/printed-page.example.json \
  --input tests/fixtures/ovis/source_page.jpg \
  --out 'output/linux-current/中文教材页/job'

build/linux-current/dococr_cli --config configs/printed-page.example.json \
  --input tests/fixtures/pdf/printed_textbook_2p.pdf \
  --out 'output/linux-current/pdf教材两页/job' --pages 1-2 --dpi 200
```

- [图片原始 Markdown](../output/linux-current/中文教材页/job/document.md)、[JSON](../output/linux-current/中文教材页/job/document.json)、[验证摘要](../output/linux-current/中文教材页/summary.json)：`partial`，19 块中 14 块 `ok`、5 块 `partial`，其中 4 个区域达到 token 上限。固定教材正文块 `b0003` 与参考正文完全匹配，裁剪框与参考范围重叠；共有 45 个资源文件。`run-manifest.json` 的解码、版面、识别、导出阶段耗时分别为 11、1390、84900、6 毫秒。其他块不能据此宣称完整识别正确。
- [PDF 原始 Markdown](../output/linux-current/pdf教材两页/job/document.md)、[JSON](../output/linux-current/pdf教材两页/job/document.json)、[验证摘要](../output/linux-current/pdf教材两页/summary.json)：`ok`，选中原 PDF 第 1～2 页，每页 5 个正文块与固定逐行真值一致，10 个 `resources[]` 引用均存在。Python 端到端墙钟时间 28.772 秒，运行清单总墙钟时间 23381 毫秒；本机 Poppler 为 24.02.0。该样本是项目自制的可选文字 PDF，但正式路径仍由渲染后的页面经双模型解析。
- 两份作业均通过生产 `--reexport` 导出到 [图片新目录](../output/linux-current/中文教材页/reexport/) 和 [PDF 新目录](../output/linux-current/pdf教材两页/reexport/)；[重导出核对结果](../output/linux-current/reexport-verification.json) 记录原 JSON 字节、Markdown 字节及所有复制文件均与原作业一致。重导出命令的 stdout/stderr 日志分别保存在各样本目录中。

当前版本的图片识别会受版面切块与每区域 512 token 上限影响；遇到 `partial` 时，应结合 JSON 状态、`run-manifest.json` 中的 `stop_reason`、原图和区域裁剪核对内容。PDF 的逐页错误与空白页信息也以 JSON 和运行清单为准。
