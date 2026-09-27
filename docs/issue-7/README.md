# Issue #7：真实单页正文转写

本项在 #6 的 PP-DocLayoutV3 版面结果后，使用 OvisOCR2 的 MNN 高层图像接口逐区域转写。生产配置为 [`configs/printed-page.example.json`](../../configs/printed-page.example.json)，通过公共 C ABI 作业及 `dococr_cli` 执行；不使用测试 fixture 或云端后备。当前在 Linux CPU 上验证图片单页，PDF 多页和 Windows 实测属于后续任务。

## 运行契约

- `mnn:pp-doclayout-v3+ovisocr2` 同时绑定已核验的 Layout 与 Ovis 工件。Ovis 的八个运行工件必须各出现一次且哈希匹配。启动时核对高层运行时实际解析的 CPU、两个线程数、greedy、KV/提示缓存和 120 秒超时配置；模型常驻一次，单实例最多一个请求，区域前调用公开 `Llm::reset()`。
- Layout 继续沿用 #6 的前处理、候选筛选、几何排序、mask 和原始张量证据。Ovis 区域输入为未再归一化的 RGB 裁剪 PNG，由高层运行时完成视觉 resize、patch、模板、tokenizer 与生成。提示词与 [`tests/fixtures/ovis/prompt.txt`](../../tests/fixtures/ovis/prompt.txt) 逐字节一致（仅去掉文件末尾换行，保留开头换行）；`prompt_sha256=de9617f877f6110d22adf1a6ba2a96221189dc246fb1fef161e408d37bff5267`。
- 正常文字在 DocumentIR 保留模型原文及其换行，`provenance.raw_output` 也保存原始输出。导出 Markdown 时，仅对本项未验证的生成内容转义 HTML 主动标签和模型编造的 Markdown 图片语法；旧版已验证的结构化公式/表格渲染不变，LaTeX 反斜杠也不转义。公式与表格当前仅保留原始 Markdown/HTML 片段和裁剪图，标 `partial`/`specialized_parser_pending`，不宣称 LaTeX 或表结构已经校验。图片、未知类别保留系统生成的图片资源。所有转写置信度为 `null`。
- 运行清单的每个区域有 `request_id`、`status`、`stop_reason` 和 `elapsed_ms`；总清单记录两个模型哈希、MNN 版本、CPU、有效计划、全部显式采样参数及 seed 未配置状态。正常结束、token 上限、空输出、视觉未处理和运行时错误分别处理。可恢复的区域结果错误保留占位，并对下一识别区域重新 reset；reset 失败、异常或错误响应类型使状态未知时，后续识别块标 `skipped`，其图片仍保留。最后一个区域推理期间取消时，等待该调用返回后报告 `cancelled`。
- DocumentIR 仍为 1.0，`reading_order_source=geometry`，没有给字符或单元格制造坐标。新停止原因放在独立运行清单，不改变 DocumentIR 1.0 的字段语义。

## 复现

源码构建要求本机 `/home/dr/project/MNN` 的公开头文件、`build/libMNN.so` 和 `build/libllm.so`。模型包仍位于 `models/`，不会被构建改写。完整教材源页为 [`tests/fixtures/ovis/source_page.jpg`](../../tests/fixtures/ovis/source_page.jpg)，来自 OmniDocBench 修订 `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec` 的中文 `book` 页；SHA-256 为 `c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6`。正文裁剪和真值见 [`manifest.json`](../../tests/fixtures/ovis/manifest.json)。固定图片入库，复现不依赖旧 `output/` 目录。

Python 公共接口脚本和现有 #6 `ctest` 的 JSON Schema 检查需要 `jsonschema`；本机系统 `python3` 已安装。原生库及 CLI 本身不依赖 Python。

```sh
cmake -S . -B /tmp/dococr-issue7 -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN
cmake --build /tmp/dococr-issue7 -j2
ctest --test-dir /tmp/dococr-issue7 --output-on-failure
python3 tests/printed_page_integration.py /tmp/dococr-issue7/dococr_cli_fixture
python3 tests/printed_page_cancel.py /tmp/dococr-issue7/libdococr_c_test.so
python3 tests/printed_page_real.py /tmp/dococr-issue7/dococr_cli /tmp/dococr-issue7-real
python3 tests/ovis_same_instance_real.py /tmp/dococr-issue7/dococr_ovis_same_instance_probe /tmp/dococr-issue7-isolation
```

受控测试从公共 CLI 观察正常文本、中文与英文标点换行、公式/表格待解析、图片/未知资源、原图框、Markdown 占位、正常/截断/空输出/错误以及后续区域状态，并从公共 C ABI 核对最后区域推理期间取消。真实脚本从同一 CLI 验证 DocumentIR Schema、教材正文与标注、原图定位、资源、运行清单；没有使用内部函数调用次数作为断言。同实例探针运行 B（完整表格）→ A（中文正文）→ B → 坏图 → B，保存全部原始输出和公开高层状态，并读取 `dump_config()` 核对有效参数。

## Linux CPU 实测

2026-09-27 对固定教材 JPG 运行真实公共 CLI，退出码 `0`；机器摘要见 [`evidence/real-summary.json`](evidence/real-summary.json)。19 个选中区域含 17 个文字、1 个表格、1 个公式；13 个文字块正常，4 个细小文字块反复生成直到 512-token 上限，表格和公式均待专门解析，因此整页为 `partial`。45 个输出工件含每块裁剪 PNG。正文块 `b0003` 的原始文字与 OmniDocBench 标注逐字相同，其原图框 `[93,176,1241,256]` 与标注裁剪 `[93,169,1244,260]` 相交。此单个正文真值和正常结束不能推断整页转写准确：4 个文字块实际质量失败，公式尚非 LaTeX 产品结果，表格未做结构验证。

最终 `ctest` 七项全通过（3.33 秒），受控公共 CLI 测试退出 `0`。正式同实例探针结果见 [`evidence/same-instance-report.json`](evidence/same-instance-report.json)；坏图即使进程能生成文字也必须因视觉指标为零判为失败。同实例证据只针对固定模型、配置和样本，不能代替其他输入的质量验收。

正式同实例探针退出 `0`，五次状态依次为 `NORMAL_FINISHED`、`NORMAL_FINISHED`、`NORMAL_FINISHED`、`MAX_TOKENS_FINISHED`、`NORMAL_FINISHED`。B 是完整表格，三次原始 SHA-256 均为 `f5f90f8fa95ff4fc823311196df525ad975463c5daab867945b539f98e4c33ee`；坏图视觉耗时和像素数均为零。其 `loaded-config.json` 是高层公开 `dump_config()` 实际读到的配置，核对 CPU、主模型/视觉模型各 1 线程、greedy、关闭缓存及 120 秒限时。先前探索使用中文正文作 B，所得 `e1cf9ab0…` 是另一张图的原始输出，不是跨线程或文本清理差异。

`/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v` 的 12 项原有探针回归通过（34.475 秒）。完整教材页仍需人工检查其余文字块及后续结构解析；本项只提供首条可追溯的真实离线路径。

生产配置拒绝边界也从 CLI 验证：`max_new_tokens=4097` 返回退出码 `3`/`unsupported_parameter`；将 `visual.mnn` 条目替换成重复的 `config.json` 返回退出码 `3`/`ovis_artifact_duplicate`，不让未声明视觉工件参与运行。命令与原始错误见 [`evidence/token_over_limit.result.json`](evidence/token_over_limit.result.json)、[`evidence/duplicate_artifact.result.json`](evidence/duplicate_artifact.result.json)。
