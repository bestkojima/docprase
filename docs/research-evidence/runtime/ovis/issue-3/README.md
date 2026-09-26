# Issue #3：Ovis 印刷区域诊断

日期：2026-09-26。诊断对象是清晰教材页的三个**识别区域**，不是完整页面解析。结论分为诊断完成度与内容质量：三类区域均获得完整原始生成和正常结束证据；中文正文与数据集标注逐字相同，五行表格结构完整；公式的数学内容完整，但 MNN 与原框架都输出 Markdown 标题形式而非所要求的 LaTeX。此格式缺口已保留，不改写原始模型输出，也不把它称为公式产品质量通过。

## 样本及来源

- 样本、数据集标注、源页哈希、裁剪坐标及文件哈希见 [`tests/fixtures/ovis/manifest.json`](../../../../../tests/fixtures/ovis/manifest.json)。三个区域均来自 OmniDocBench 标注为 `book`、`simplified_chinese` 的 `docstructbench_dianzishu_zhongwenzaixian-o.O-61520788.pdf_391.jpg`；数据集修订为 `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec`，原始标注 JSON 的 SHA-256 为 `a45cd84b04ad8b793e775089640e6b681209abea33ead54c1828ddca35fae496`。源页下载地址：<https://huggingface.co/datasets/opendatalab/OmniDocBench/blob/aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec/images/docstructbench_dianzishu_zhongwenzaixian-o.O-61520788.pdf_391.jpg>。
- `chinese_text` 为教材正文；`printed_formula` 为独立印刷公式；`complete_table` 为五行三列完整表格。标注的 `text`、`latex`、`html` 字段原样保存于 `.reference.txt`。这些是数据集真值，**不是** Ovis 作者输出。
- 固定提示词为作者原始模型 README 推理示例的长提示词，见 [`prompt.txt`](../../../../../tests/fixtures/ovis/prompt.txt)；三类区域共享，不对单类调提示词。裁剪仅加 4 像素边距，无缩放、颜色归一化或锐化。

## 模型、接口与处理责任

- MNN 源码及运行库：`/home/dr/project/MNN`，commit `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6`。指定模型包为本仓库 `models/ovis/`；逐文件哈希、独立有效配置、探针二进制哈希和生成参数见 [`mnn-suite/artifact-manifest.json`](mnn-suite/artifact-manifest.json)。模型包自身未修改。独立配置采用 CPU/4 线程、greedy、禁用 KV 重用和 prompt 缓存，最大生成 512 token、单请求 120 秒。
- [`scripts/model_probe_ovis.cpp`](../../../../../scripts/model_probe_ovis.cpp) 调用 MNN 高层 `Llm::createLLM`、`load`、带 `<img>绝对路径</img>` 的 `response`，直接读取 `LlmContext::status`、生成 token 数、视觉耗时和像素数。语言图 embedding 输入不是外层填整数 token 的接口。旧 64-token 冒烟没有进入本次完整质量结论。
- 高层 Omni 从文件解码图像。`MNN/transformers/llm/engine/src/omni.cpp` 的 `processImageContent` 和 `qwen2VisionProcess` 负责图像分支；本视觉图的五输入及 `idx_tensor` 选择 Qwen3VL 分支。运行时依据配置的像素上下限按 32 对齐做 smart resize，使用 cubic 与 BGR→RGB，再按 `image_mean=127.5`、`image_norm=1/127.5` 处理并构造 patches。`Llm`/Omni 负责模板、tokenizer、视觉 embedding 及文本生成。探测入口只交付未预处理的区域 PNG，不重复 resize 或 normalize。图接口输入名及静态形状另见已有 [`RESULT.md`](../RESULT.md)。
- 有效三类运行均有 `vision_us>0`、`pixels_mp>0`：正文 0.110592 MP，公式 0.089088 MP，表格 0.258048 MP；分别见各自 `report.json`。坏图虽然进程返回 0 并生成文字，视觉指标为 0，因而探测入口判为错误。

## 完整输出、停止及参考差异

| 区域 | MNN 结束 / token | 数据集标注差异 | 原框架对照 |
| --- | --- | --- | --- |
| 中文正文 | `NORMAL_FINISHED` / 39 | 原始输出与标注字节相同。 | 原框架 39 token 正常结束；文字相同，仅原始文件末尾换行不同。 |
| 独立公式 | `NORMAL_FINISHED` / 13 | 标注为 `$$` 包围的 LaTeX `\times`；MNN 原始输出为 `## 运输保险费=材料原价×材料运输保险费率`，无 LaTeX 标记。 | 原框架 15 token 正常结束，同为 Markdown 标题，`=` 与 `×` 两侧有空格；差异见 `comparison/printed_formula.original-vs-mnn.diff`。 |
| 完整表格 | `NORMAL_FINISHED` / 139 | 五行、每行三格均保留；标注 `border="1"`、`序 号`，MNN 为 `border=1`、`序号`。 | 原框架 139 token 正常结束；HTML 主体与 MNN 相同，仅原始文件末尾换行不同。 |

原始输出在 `mnn-suite/<区域>/raw.txt` 与 `original-<区域>/raw.txt`，不做清洗；对数据集标注的差异在 `mnn-suite/<区域>/reference.diff`，两框架逐字差异在 [`comparison/report.json`](comparison/report.json) 及同目录 `.diff`。MNN 单独诊断报告为 [`mnn-suite/suite-report.json`](mnn-suite/suite-report.json)，其 `quality_gaps` 明示公式格式、公式与表格非逐字标注一致。该报告中的 `reference_model_output=not_verified` 指**单独运行 MNN 时未提供原框架输出**；后续独立运行的有效原框架对照见 `comparison/`。

原始模型来自 `ATH-MaaS/OvisOCR2`，本地 `model.safetensors` SHA-256 为 `9270560288656ece5cb3a6989001afcf5af8d223bceed4a423c33a008861d009`，与 Hugging Face 修订 `1fc9221b7823a371d6e97f92d527cc847e24e107` 的 LFS SHA-256 一致。用本地 Transformers 5.16.1 / Torch 2.13.0+cu130 在 CPU、float32 运行，图片经原包 `AutoProcessor`，greedy、同提示词及 512-token 上限。原框架的聊天模板以 `<|im_end|>`（ID 248046）结束，包内 `text_config` 默认 EOS 为 `<|endoftext|>`（ID 248044）；`model_probe_ovis_original.py` 显式指定 248046，三次均实际正常停止。原框架和 MNN 各自承担视觉预处理；二者的动态像素张量未逐值比较，且作者正式 vLLM 示例另覆写 `min_pixels=448²`、`max_pixels=2880²`。因此本对照是可追溯原框架诊断，并不宣称 bit-exact 模型转换或作者 vLLM 服务数值对齐。旧 `output/ovis-original-text/`、`output/ovis-original-formula/` 和 `output/ovis-suite-issue3/` 属短提示词或错误 EOS 的**已撤回探索**，不作为本报告基线。

## 隔离、失败与有界超时

- 策略是**每个识别区域启动独立子进程并重新构建模型实例**，不依赖尚未实测的同实例 `reset()`。独立 B、A→B、损坏 PNG 失败→B 均在 `mnn-suite/` 保存各次 `raw.txt` 和 `report.json`。B 三次原始 SHA-256 均为 `f5f90f8fa95ff4fc823311196df525ad975463c5daab867945b539f98e4c33ee`，停止状态均正常且有视觉工作；此样本没有残余差异。这只验证当前独立进程策略与样本，不证明同实例重置安全。
- 损坏 PNG 由 MNN 返回进程码 0，但视觉耗时及像素数均为 0，最后触 512-token 上限并产生无意义文本；入口判 `error` / `incomplete`，随后 B 正常且与独立 B 字节相同。不能把退出 0 当作成功识别。
- 真实正常图片的 8-token 用例见 [`token-limit/report.json`](token-limit/report.json)：进程码 0、MNN `MAX_TOKENS_FINISHED`、输出仅 23 字节，标记 `incomplete`。1 秒真实超时见 [`timeout/report.json`](timeout/report.json)：约 1.378 秒有界结束，进程码 -15，`process_group_terminated=true`。入口用独立进程组，先 TERM、最多等 2 秒后 KILL；[`timeout/post-timeout-process-check.json`](timeout/post-timeout-process-check.json) 记录无存活探针进程，超时后再次识别 B 与独立 B 字节相同。

## 复现命令

在仓库根目录，需已有 `/home/dr/project/MNN/build/libllm.so` 与模型权重；入口自动编译小型 C++ 探针，不修改 MNN 或模型目录。

```bash
python3 scripts/model_probe_ovis.py --suite --out output/ovis-suite-issue3-author-prompt --max-tokens 512 --timeout 120
python3 scripts/model_probe_ovis.py --out output/ovis-token-limit-issue3 --image tests/fixtures/ovis/complete_table.png --reference tests/fixtures/ovis/complete_table.reference.txt --max-tokens 8 --timeout 120
python3 scripts/model_probe_ovis.py --out output/ovis-timeout-issue3 --image tests/fixtures/ovis/complete_table.png --max-tokens 512 --timeout 1
OMP_NUM_THREADS=4 /home/dr/project/google_edge/litert-env/bin/python scripts/model_probe_ovis_original.py --image tests/fixtures/ovis/chinese_text.png --out output/ovis-original-author-text --max-tokens 512
OMP_NUM_THREADS=4 /home/dr/project/google_edge/litert-env/bin/python scripts/model_probe_ovis_original.py --image tests/fixtures/ovis/printed_formula.png --out output/ovis-original-author-formula --max-tokens 512
OMP_NUM_THREADS=4 /home/dr/project/google_edge/litert-env/bin/python scripts/model_probe_ovis_original.py --image tests/fixtures/ovis/complete_table.png --out output/ovis-original-author-table --max-tokens 512
python3 scripts/compare_model_probe_ovis.py --mnn-suite output/ovis-suite-issue3-author-prompt --original-text output/ovis-original-author-text --original-formula output/ovis-original-author-formula --original-table output/ovis-original-author-table --out output/ovis-comparison-issue3
```

本次实际退出码依次为 `0、0、1、0、0、0、0`；超时命令返回 1 是预期诊断结果。原框架三次实际运行外层另加 GNU `timeout --signal=TERM --kill-after=5s 180s`，均未触外层限时。`python3 -m unittest tests/test_model_probe_ovis.py -v` 验证探测入口的状态判定、无视觉拒绝、套件结构检查和进程组清理。完整测试使用已有隔离环境 `/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v`，本次 11 项通过；系统 `python3` 未安装既有 Layout 测试需要的 NumPy，故不作为完整测试解释器。
