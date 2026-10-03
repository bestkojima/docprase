# #31 父区域评测与 Paddle 去边实验

## 当前口径

`issue31-parent-region-1` 通过 `scripts/issue31_evaluation.py` 公开命令提供父区域评分。正文沿用 #27 的几何多对一配对和不复用输出字符的编辑路径；包含行内公式的完整父正文参与 CER。八项门槛逐项与 #31 一致，行内公式严格匹配率只诊断。原九项评分始终单列，不改历史 GT、转写或成绩。

参考中的冗余子项只有完全包含在唯一参考父 text/table 内、且原始内容在父参考中唯一出现时才归父。这个决定仅依赖 GT，不依赖预测或成功状态；完整子参考、原分母和转移原因保留。无唯一父项或父参考未包含子内容时不删除分母，保持未核验。当前 20/12 页原标注中没有此类冗余顶层子参考。预测的区域合并不能将独立公式的参考分母消掉。父表格及单元格继续以完整原 HTML 比较位置、跨度与文字。

额外文本计为插入；已归父项再次输出记空间重复。内部顺序需要输出中唯一且不重叠的原文出现位置，不能从参考顺序或编辑路径推导正确。失败正文计完整删除，原始响应仅作诊断。行内公式表示差异不自动视为等价，待核验记录不通过内容审计；这不是新增第九项指标。零公式或零表格分母的单页不能独立声明全集质量通过。

## 先运行，再参照 Paddle

2026-10-03 新编译实际生产 CLI 后完整执行 `candidate-12`。20 项正文 GT，共 805 字符，输出 13 个父 text：旧一对一配对为 586 次编辑（72.80%）、漏匹配 7 项；父区域配对为 49 次编辑（6.09%）、漏匹配 0 项。同一输出的重新计分不是识别改善。原图核对后仍有空白线长度、标点、空格及模型生成的标题标记差异，不能宣称逐字正确。身份、命令和原始产物 SHA 见 `evidence/english-diagnosis.json`。

随后核对固定 PaddleX `ffb64904d23708863ff5b8da312a5cbd52a7f462`：

- [区域准备及公式分支](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/pipelines/paddleocr_vl/pipeline.py)：先滤重、裁图、组合，再收集 VLM 输入；公式调用 `crop_margin`。
- [crop_margin](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/pipelines/paddleocr_vl/uilts.py#L1076)：灰度归一化后以 200 阈值取非白内容包围框。

对数列页六个公式只改变去边处理，保持 Ovis、提示词和 65536–313600 像素预算。`b0006` 原始转写的 `2^{54}` 恢复为 `2^{64}`；其他五项没有观察到新数值/字母错误，`b0017` 出现标题标记及分隔符、句点变化。完整前后原始响应、命令、裁图 SHA 见 `evidence/margin-probe.json`，实验程序见 `evidence/margin-probe.py`。

**最终生产没有启用去边。** 后续建立过可追溯实际输入坐标的生产候选，但旧公式页实跑出现 `α→a` 与指数因子缺失，已完整撤回。不能将该单页实验当作全量改善，更不能按标准答案选择实验输出。完整生产对照及否决依据见 [诊断](diagnosis.md)。此前统一提高公式预算产生退化的历史证据继续保留。

## 全量运行

```bash
python3.12 scripts/issue31_run.py --cli build/issue31/dococr_cli --out output/issue31-new-run
python3.12 scripts/issue31_suite.py verify-exports --run output/issue31-new-run
python3.12 scripts/issue31_suite.py report --run output/issue31-new-run
```

复用已验证的串行运行器，每次启动 38 个全新公共 CLI 作业、共 39 页（20 页开发集、旧 7 页，其中一项为 2 页 PDF，以及 12 页全集）。冻结实际模型、二进制、动态库、源码、配置、输入、GT、评分工具、Schema 和数学渲染器。旧版本评分工具保留；仅给共用运行器添加可选冻结函数，默认仍为 #27。

新验收使用当前生产 Schema 路径，并分别对首次导出和生产 JSON 重导出的 Markdown 调用实际 KaTeX。资源尺寸、SHA、字节等价与内容质量分别记录。自动化和真实取消/超时/重试隔离的凭据必须绑定本次候选及评测 SHA，旧凭据不能复用。报告退出码 2 表示有效报告但条件未通过；冻结或输入非法为 1。

本轮材料及六公式实验均为已曝光开发资料，不声明独立泛化质量。成功完整新跑目录为 `output/issue31-final-20261003`，38 作业/39 页工程通过，开发集及评测全集各仅 2/8 项质量指标通过。完整结果及尚未满足项见 [results.md](results.md)；工程通过不代表 #31 产品质量达标。
