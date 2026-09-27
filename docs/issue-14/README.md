# Issue #14：Linux 单页离线解析

本项交付清晰印刷教材图片的 Linux CPU 单页入口。正式 CLI 与 `libdococr_c.so` 都通过 PP-DocLayoutV3、OvisOCR2 的同一组 MNN 模型运行；整页质量以 JSON 中的块状态和原始输出为准。多页与全场景回归由 #15 验收。

## 程序与外部依赖

- 本仓库源码：C++17、CMake 3.20 及以上版本；CLI 构建目标为 `dococr_cli`，公共共享库为 `libdococr_c.so`。公共头文件 [`dococr.h`](../../include/dococr/dococr.h) 只暴露 C 兼容整数、指针和显式长度结构；调用方无需 MNN 头文件或 C++ STL。
- MNN 运行时：固定上游提交 `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6`（3.6.1），需构建 CPU 图像 LLM 功能，提供 `libMNN.so`、`libllm.so`、`libMNNOpenCV.so`、`libMNNAudio.so` 与 `libMNN_Express.so`。本仓库 CMake 从 `DOCOCR_MNN_ROOT` 查找公开头文件和已构建库；不把 MNN 源码或 `.so` 捆进本仓库。
- 外部模型：`dr3334/PP-DocLayoutV3-mnn` 修订 `c67c1a858d5f6c855172d4cfdf931798dafa2edd`，`dr3334/ovrics-ocrv2_mnn` 修订 `20f12e49d846941e67829a7a7c3645693e485942`。工件放在仓库的 `models/doclayout`、`models/ovis`；[`printed-page.example.json`](../../configs/printed-page.example.json) 固定九个必需文件的 SHA-256、CPU、单线程和生成预算。模型权重不入 Git。
- 本项图片运行不需要 Poppler。若将来使用 PDF，需另外准备 `pdfinfo` 与 `pdftoppm`；PDF 的全面验收见 #15。

模型由使用者另行提供。可在仓库根目录使用 ModelScope 下载上述固定修订，验收脚本随后按配置逐一核对九个 SHA-256；`modelscope` 是下载工具，CLI 运行时不需要它：

```sh
python3 -m pip install modelscope
python3 - <<'PY'
from modelscope import snapshot_download
snapshot_download('dr3334/PP-DocLayoutV3-mnn',
                  revision='c67c1a858d5f6c855172d4cfdf931798dafa2edd',
                  local_dir='models/doclayout')
snapshot_download('dr3334/ovrics-ocrv2_mnn',
                  revision='20f12e49d846941e67829a7a7c3645693e485942',
                  local_dir='models/ovis')
PY
```

## 从源码构建与运行

先在自己的目录克隆并构建固定版 MNN；以下示例沿用仓库同级目录，用户可改 `MNN_ROOT`。随后回到本仓库根目录构建与运行。配置中的相对模型路径按启动 CLI 时的工作目录解析。

```sh
MNN_ROOT="$(pwd)/../MNN"
git clone https://github.com/alibaba/MNN.git "$MNN_ROOT"
git -C "$MNN_ROOT" checkout baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6
cmake -S "$MNN_ROOT" -B "$MNN_ROOT/build" -DCMAKE_BUILD_TYPE=Release \
  -DMNN_BUILD_SHARED_LIBS=ON -DMNN_BUILD_LLM=ON -DMNN_BUILD_LLM_OMNI=ON \
  -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_DEMO=OFF
cmake --build "$MNN_ROOT/build" -j4

cmake -S . -B build/linux-current -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_REQUIRE_MNN=ON \
  -DDOCOCR_REQUIRE_LLM=ON -DDOCOCR_MNN_ROOT="$MNN_ROOT"
cmake --build build/linux-current -j4
ctest --test-dir build/linux-current --output-on-failure

mkdir -p output/issue-14-linux/example
cp tests/fixtures/ovis/source_page.jpg 'output/issue-14-linux/example/教材原图.jpg'
build/linux-current/dococr_cli --config configs/printed-page.example.json \
  --input 'output/issue-14-linux/example/教材原图.jpg' \
  --out 'output/issue-14-linux/example/中文教材页'
```

输出目录包含 `document.md`、`document.json`、`assets/`、`execution-plan.json`、`run-manifest.json`、`job-status.json` 和 `job-events.jsonl`。`document.json` 保留块的原始模型输出、状态与资源引用；`partial` 表示仍有需要检查的区域。CLI 退出码 0 只说明作业和导出完成。

构建产物的 `RUNPATH` 会指向构建时找到的共享库目录。用 `ldd build/linux-current/dococr_cli` 和 `ldd build/linux-current/libdococr_c.so` 确认所有依赖均有路径且无 `not found`；复制二进制到另一机器后，需提供匹配的 MNN 运行时并重新配置运行时搜索路径，或在目标机器重新构建。本次本机的绝对路径及解析结果记录于 [`cli-dynamic.txt`](evidence/cli-dynamic.txt)、[`abi-dynamic.txt`](evidence/abi-dynamic.txt)、[`cli-ldd.txt`](evidence/cli-ldd.txt) 和 [`abi-ldd.txt`](evidence/abi-ldd.txt)，不作为其他机器的固定路径。

## 实际验收命令

[`issue14_linux_verify.py`](../../scripts/issue14_linux_verify.py) 将固定教材页复制为 `教材原图.jpg`，调用生产 CLI 导出到 `中文教材页/job`，检查严格 UTF-8、DocumentIR Schema、已声明资源文件与 Markdown 相对引用，并比对固定正文真值。随后它在 Python 进程中直接加载生产 `libdococr_c.so`，再以真实模型运行同页，从 C ABI 取回 JSON、Markdown、运行清单和资产字节。ABI 检查还覆盖版本、无效及忙句柄、旧结果结构拒绝、旧输入结构兼容、错误 JSON、返回字节的成对释放和重复释放拒绝。

```sh
python3 scripts/issue14_linux_verify.py \
  --cli build/linux-current/dococr_cli \
  --lib build/linux-current/libdococr_c.so \
  --out output/issue-14-linux/real
```

脚本要求一个不存在的输出目录，以免旧工件混入。运行前需有 Python 3.9 及以上版本和 `jsonschema`，但 CLI 和 `.so` 本身不依赖 Python。固定输入是 [`source_page.jpg`](../../tests/fixtures/ovis/source_page.jpg)，SHA-256 为 `c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6`。完整导出目录与 45 个资产位于本机 `output/issue-14-linux/real/`；该目录被 Git 忽略。关键原始 JSON/Markdown、运行清单、ABI 结果、构建与依赖日志已按原字节保存到已跟踪的 [`evidence/`](evidence/)，每份文件的 SHA-256 列在 [`SHA256SUMS`](evidence/SHA256SUMS)。新检出可检查这些记录，也可运行上述命令生成自己的完整工件。

## 本机结果

本机为 Ubuntu Linux，`g++` 13.3.0、CMake 4.4.3、MNN 3.6.1（上述固定提交）。[`configure.log`](evidence/configure.log)、[`build.log`](evidence/build.log) 记录核心、MNN 后端、C ABI 与 CLI 构建退出 0；[`cli-ldd.txt`](evidence/cli-ldd.txt) 和 [`abi-ldd.txt`](evidence/abi-ldd.txt) 均解析出 MNN 与 LLM 等共享库，没有 `not found`。

[`summary.json`](evidence/summary.json) 与 [`abi-result.json`](evidence/abi-result.json) 记录真实 CPU 单页命令、9 个模型工件 SHA、有效配置及 ABI 状态。CLI 退出 0；中文输入 `教材原图.jpg` 到中文输出目录 `中文教材页/job`，DocumentIR 1.3 为 `partial`，19 个块中正文 `b0003` 经首尾空白规范化后与固定参考匹配，导出 45 个资产文件；Markdown 的 5 处本地引用均在声明资源中。生产 `.so` 再次运行同页返回 0，报告 45 个资产并取回首项；ABI 返回的 JSON 和 Markdown 与 CLI 导出的字节 SHA 相同。原始正文模型输出保存在 [`raw-reference-block.txt`](evidence/raw-reference-block.txt)，其他块输出与逐区域停止原因保存在完整 [`document.json`](evidence/document.json) 和 [`run-manifest.json`](evidence/run-manifest.json)。

## 边界

固定教材页此前已观察到部分细小文字块达到 512 token 上限，公式和表格仍需对应的专门解析；不能从单个正文块的真值匹配推断整页识别正确。历史 Windows 两轮 CI 结果仍保留在提交历史与 [Issue 评论](https://github.com/bestkojima/docprase/issues/14#issuecomment-5854117287)，未完成 Windows 实际模型运行；本轮 Linux 验收不使用这些记录。
