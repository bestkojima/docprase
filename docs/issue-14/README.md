# Issue #14：Windows 单页离线解析

## 当前验收状态

Windows 实测以 [专用工作流](../../.github/workflows/issue14-windows.yml) 的 `windows-2022` 运行记录为准。提交代码或 Linux 结果本身不构成 Windows 验收；在附上成功的 Actions run、日志和原始输出前，Issue 保持未验证。

本项仅检查清晰印刷教材图片的单页 CPU 路径。多页 PDF 和整套试卷教材回归由 #15 验收。

## 依赖与工件

- 程序源码：本仓库；C++17、CMake ≥ 3.20、Visual Studio 2022 x64、Python 3.12（仅下载和验收脚本需要）。
- 程序运行时：固定提交 `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6` 的 [MNN](https://github.com/alibaba/MNN)，启用 `MNN_BUILD_LLM=ON` 和 `MNN_BUILD_LLM_OMNI=ON`。Windows 版本的 LLM 对象合入 `MNN.dll`，`dococr_c.dll` 和 `dococr_cli.exe` 应与 `MNN.dll` 同目录；使用 Visual Studio 的 `/MD` 运行时。
- 外部模型：`dr3334/PP-DocLayoutV3-mnn` 修订 `c67c1a858d5f6c855172d4cfdf931798dafa2edd`、`dr3334/ovrics-ocrv2_mnn` 修订 `20f12e49d846941e67829a7a7c3645693e485942`。模型不包含在程序源码或构建输出中；[`configs/printed-page.example.json`](../../configs/printed-page.example.json) 固定九个必需文件的 SHA-256。下载脚本逐一复核哈希。
- 固定输入：[`source_page.jpg`](../../tests/fixtures/ovis/source_page.jpg)，SHA-256 `c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6`。模型识别质量会是 `partial`；不得把成功导出解释为全页文字正确。

## Windows PowerShell 复现

从仓库根目录运行以下命令。模型可由脚本下载，或自行放在 `models/doclayout`、`models/ovis` 后执行 `verify`；配置中的相对路径以仓库根目录为工作目录解析。

```powershell
git clone https://github.com/alibaba/MNN.git mnn
git -C mnn checkout baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6
cmake -S mnn -B mnn/build -G "Visual Studio 17 2022" -A x64 -DMNN_BUILD_SHARED_LIBS=ON -DMNN_WIN_RUNTIME_MT=OFF -DMNN_BUILD_LLM=ON -DMNN_BUILD_LLM_OMNI=ON -DMNN_BUILD_TEST=OFF -DMNN_BUILD_DEMO=OFF -DMNN_BUILD_CONVERTER=OFF
cmake --build mnn/build --config Release --parallel 4
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DDOCOCR_MNN_ROOT="$PWD/mnn" -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON -DDOCOCR_BUILD_TESTS=ON
cmake --build build-win --config Release --parallel 4
Copy-Item mnn/build/Release/MNN.dll build-win/Release/
python -m pip install jsonschema markdown-it-py "modelscope>=1.20,<2"
ctest --test-dir build-win -C Release --output-on-failure -R '^(contract|cli_integration|config_integration|config_abi|layout_integration|printed_page_integration)$'
python scripts/issue14_windows.py download --models models
python scripts/issue14_windows.py verify --cli build-win/Release/dococr_cli.exe --dll build-win/Release/dococr_c.dll --models models --out issue14-evidence
```

常规运行只需要 CLI、两个 DLL、外部模型和配置文件。以下命令读取一个中文文件名，向中文目录写出 `document.md`、`document.json`、`run-manifest.json`、`execution-plan.json`、`job-events.jsonl` 和 `assets/`。在仓库根目录执行，或将配置中的模型 `root` 调整为实际路径。

```powershell
& .\build-win\Release\dococr_cli.exe --config configs\printed-page.example.json --input "教材样例.jpg" --out "中文解析结果"
```

`issue14_windows.py verify` 复制固定输入为 `教材样例.jpg`，检查 UTF-8 JSON/Markdown、Schema、资源相对引用和真实 CPU 运行清单；随后从独立 Python 进程仅通过 C ABI 装载 `dococr_c.dll`，验证 ABI 版本、无效句柄、旧结果结构大小拒绝、旧输入结构兼容、错误 JSON、字节成对释放及句柄销毁。脚本把 CLI stdout/stderr、全部导出文件、原始块输出、模型哈希和有效计划留在指定输出目录。Windows CTest 选择本项相关的六项无模型公共路径；含 Poppler 或 POSIX 进程/符号链接假设的跨场景测试不作为本项 Windows 单页门禁。

## CI 证据规则

工作流只在独立 `codex/issue-14-windows` 分支的 push 或人工 dispatch 运行。成功运行的 Actions URL、runner/Visual Studio/CMake/Python 版本、MNN 提交与 DLL SHA、CTest 日志、真实解析目录和 `abi-result.json` 应写入本目录的验收记录。上传包不含模型权重。若 Windows 运行未完成或失败，保留具体失败阶段与日志，Issue 不可关闭。
