# Issue #21：隔离重复、截断与待核验输出

实现 [#21](https://github.com/bestkojima/docprase/issues/21)，父规格为 [#16](https://github.com/bestkojima/docprase/issues/16)。沿用公共 C ABI 文档作业与 CLI 重新导出边界。基线提交为 `b2f6bdc`。

## 可见行为与保存契约

转写作业保存 DocumentIR 1.6。图片区分于 PDF，Schema 分别为 [图片契约](document-ir-1.6-image.schema.json) 和 [PDF 契约](document-ir-1.6-pdf.schema.json)。纯版面作业继续使用既有契约。块级状态保持 `ok/partial/failed/skipped`，具体判定保存在 `blocks[].provenance.assessment`：

| 最终判定 `state` | 块级状态 | Markdown 行为 |
| --- | --- | --- |
| `ok` | `ok` | 输出经过既有公式、表格检查的正常正文 |
| `failed` | `failed` | 原裁图与“识别失败”提示，说明视觉输入缺失、超时或识别未完成 |
| `incomplete` | `partial` | 原裁图与“识别不完整”提示 |
| `anomalous` | `partial` | 原裁图与“确认异常”提示，说明空编号或重复片段 |
| `unverified` | `partial` | 原裁图与“待核验”提示 |
| `skipped` | `skipped` | 保持既有图片或未处理占位 |

不可靠输出不进入 Markdown 正文，其他正常块继续导出。原始文本保留在 `raw_output`，非法 UTF-8 字节沿用 Base64 证据；原裁图资源、视觉证据和定位保持可访问。判定记录保存策略版本、最终状态、原因、生成完成原因、停止原因、原后端错误、规范化前的识别文字 `generation_text`，以及非空行数、空编号行数、重复片段起始字节、单元字节数、次数、覆盖率和证据来源。证据来源可以是原始输出或适配后的文字，避免二者不同时漏检正文。重新导出使用保存的识别文字，不能用原始输出替代后端实际返回的空文字。

公共运行清单的区域状态与块状态保持一致，`stop_reason` 原样保留；具体的可靠程度通过 DocumentIR 判定记录观察。本任务不增加生成重试，重试与逐次尝试配置属于 [#22](https://github.com/bestkojima/docprase/issues/22)。

## 冻结的开发判据

策略版本为 `region-output-v1`，只依据输出证据判定风险，不声称验证了文字与原图逐字相符。

- 无有效视觉输入、运行错误、超时或取消为失败；空输出为失败。
- 空编号结构：至少 **8 个**只有编号而没有题干的行，且占非空行数至少 **80%**。同时达到生成上限时为确认异常；正常停止而结构可疑时为待核验。带题干的连续题号不计入空编号行。
- 重复片段：检查 **8～256 字节**的连续相同片段，至少 **3 次**、至少 **48 字节**，且包含文字；重复段覆盖输出至少 **50%** 时触发。达到生成上限或后端明确因重复停止时为确认异常，否则为待核验。保存字节定位，保留原文，不删除重复证据。表格由结构校验负责，重复单元格或相同行不触发普通文字的重复启发式。
- 未命中异常判据而达到生成上限时为不完整。完成原因与停止原因不能确认正常结束、后端附带错误，或公式/表格结构校验未通过时为待核验。
- 题号是否连续、字符总长度、视觉耗时或像素计数均不单独决定正常或异常。

开发对照来自 #17 保留的真实模型原始结果，通过受控后端经公共作业回放。结果见 [开发审计](evidence/development-audit.json)：18 页中 379 个非空文本输出，300 个正常、41 个确认异常、38 个待核验；此前标为正常的 300 个全部仍为正常。`odb-11` 和 `odb-17` 缺少可回放的文档，明确列入缺失清单。回放只验证结果判定，fixture 提供的视觉信号不是这批原始结果的真实视觉成功证据，也不是重新运行20页模型或产品准确率评分。

已知 323×31 图注和 82×9 页眉的历史异常输出各有103个空编号行，最后一个未完成标题，共719字节，SHA-256 均为 `3b5169765724f998f070b7410a881d7fd6eac27c7972f1a473eaaa07d3aabdf8`。原始失败输出保存在 [图注](evidence/caption-numbering.txt) 和 [页眉](evidence/jee-numbering.txt)，来源为本机 `output/resolution-diagnosis/` 的相应原图运行；原运行状态为 `MAX_TOKENS_FINISHED`、512 token。公共作业回归要求其明确回退，不能冒充正文。

## 重新导出与历史兼容

1.6 重新导出检查块状态与最终判定一致，检查停止原因与视觉记录一致，并从保存文字重算判定依据；不允许把缺少视觉证据、截断或重复结果改标为正常。正常正文必须与保存的识别文字相同；公式、表格必须匹配首次识别的规范化结果，避免只修改展示文字后绕过判据。公式校验复用首次识别的解析器。首次导出和重新导出调用相同 Markdown 渲染逻辑。

1.0～1.5 仍按其原 Schema 校验，缺失的判定或视觉证据不写回、不补造。重新导出可根据已有原文、错误和停止证据隔离明显重复或截断；JSON 原始字节保持不变，因此异常旧文档的 Markdown 可能比历史导出更保守。既有历史1.1、1.2文件及1.3、1.4、1.5转换文档的兼容性由重新导出测试覆盖。

## 真实回归

使用本机固定 MNN/LLM、`configs/printed-page.example.json`、CPU1 与512 token配置，经生产公共 CLI 重新运行，结果见 [已知页面摘要](evidence/known-pages-summary.json) 与 [PDF 摘要](evidence/pdf-real-summary.json)。完整文档、资源与运行日志留在被 Git 忽略的 `output/issue-21/`。

| 输入 | 实测结果 |
| --- | --- |
| 印刷双栏科学页 | 11 个正常块、1 个跳过图片；323×31 图注正确得到 `Figure 1. Recorded readings.` |
| JEE 页面 | 6 个正常块、3 个待核验、2 个跳过；82×9 页眉正确得到 `JEE (Advanced) 2023` |
| 自制两页中文教材 PDF | 两页各5个正文块，全部匹配既有逐行真值，作业 `ok` |

两张已知页面和 PDF 均验证原裁图的可访问性，JSON 与 Markdown 重新导出逐字节一致，复制资源逐字节一致。JEE 的三个待核验块均因既有行内公式语法检查未通过而回退；它们保留原文与判据，没有被计入正常结果。以上是工程回归，不能据此宣布父规格的整页质量或独立留出验收通过。

## 验证命令

```sh
cmake -S . -B build/issue21 -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON \
  -DPython3_EXECUTABLE=/usr/bin/python3.12
cmake --build build/issue21 -j4
ctest --test-dir build/issue21 -R 'issue21_quality|reexport_integration' --output-on-failure
python3 scripts/issue21_audit.py --fixture-cli build/issue21/dococr_cli_fixture \
  --corpus output/issue-17 --out /tmp/issue21-development-audit.json
python3 tests/issue21_real.py build/issue21/dococr_cli /tmp/issue21-known-pages
python3 tests/pdf_real.py build/issue21/dococr_cli /tmp/issue21-pdf
```

红灯记录见 [首个失败测试](evidence/red-test.log)。公共作业覆盖已知空编号、普通截断、运行错误、无视觉输入、待核验重复、正常题号与长文、适配文字与原文不一致、截断表格、合法重复表格单元格，以及同一引擎连续作业。重新导出覆盖原文保留、资源读取、旧版异常隔离和伪造判据拒绝。

最终 C++17 Release 构建包含 MNN/LLM 后端；[完整 CTest](evidence/final-ctest.log) 22/22 通过，[Python unittest](evidence/final-unittest.log) 25/25 通过。Python 参考探针使用本机已有 `/home/dr/project/google_edge/litert-env/bin/python`，没有修改依赖锁文件。两轴审查、发现与修复见 [代码审查记录](code-review.md)。
