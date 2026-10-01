# Issue #22：一次有针对性的重试与安全退出

任务：[GitHub Issue #22](https://github.com/bestkojima/docprase/issues/22)，父规格为 #16；基线提交 `b31fb74`，前置 #21 的异常隔离规则继续适用。

## 冻结策略

每个识别区域最多两次尝试，每次开始前重置模型会话。第一次明确以 `token_limit` 停止且判为不完整、额度低于4096时，第二次提升到4096。已经达到4096的截断、重复异常、待核验输出、超时和一般运行错误直接回退；不启动第三次。

第一次以 `vision_missing` 停止且证据为 `no_visual_tokens` 时，先比较默认和较高像素预算的适配结果。只有画布改变且内容缩放比例提高，才以 `increase_visual_resolution` 额外尝试。默认视觉预算为65536～313600像素，修正后为262144～1120000像素；保持原裁图、等比例缩放和补白。不能适配、未知视觉失败或预算修正不能改变输入时直接回退。

配置可选 `execution.generation_timeout_ms`，默认120000，只接受1～120000。每次最多4096 token，最多两次，配置的生成预算合计不超过240000毫秒。`generation_elapsed_ms` 是同步语言生成调用实际耗时，`attempts[].elapsed_ms` 包含本次会话重置、输入适配、视觉处理及生成。运行时在 prefill/decode 检查超时，正在执行的计算可能超过配置预算；这不是包含全部前后处理的墙钟硬保证。取消沿用公共作业的 `after_backend_call` 粒度，返回后不再重试，作业结束时安全重建后端。

## 保存与重新导出

图片和 PDF 的 DocumentIR 版本为1.7。块的 `provenance.recognition` 保存策略、配置预算汇总、生成耗时汇总、全部尝试和 `selected_attempt`。每次记录实际配置、修正原因、视觉 token/变换、原始模型输出、识别文字、后端错误和停止原因；无效 UTF-8 使用对应 Base64 字段保留。未识别的图片等块保存空尝试数组及空采用索引。

视觉修正另存 `input_visual` 计划变换，独立于实际视觉处理是否成功。第二次在重置或执行时失败，仍能证明输入修正并重新导出；计划变换不能冒充成功视觉证据。

最终块内容、判定与视觉证据来自最后一次尝试。成功重试恢复正常正文；失败重试遵守 #21 的原图与提示规则。取消没有可导出的文档，已执行的尝试保存在公共运行清单中，PDF 同时保留逐页清单。

生产 `--reexport` 接受1.0～1.7，校验尝试次数、额度、修正条件、汇总和最终采用结果的一致性。保留原 JSON 字节，使用相同 Markdown 规则，不调用模型；旧文档不补造尝试记录。

## 验证边界与命令

沿用 #16 已确认的公共 C ABI 文档作业接口与 JSON 重新导出边界。受控后端只替代模型结果，不替代作业、重试、判定、资源及导出实现。真实模型验证单独报告。

```sh
cmake -S . -B build/issue22 -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON \
  -DPython3_EXECUTABLE=/usr/bin/python3.12
cmake --build build/issue22 -j4
ctest --test-dir build/issue22 --output-on-failure
/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -p 'test_*.py'
python3.12 tests/issue22_real.py build/issue22/libdococr_c.so \
  build/issue22/dococr_cli output/issue22-real
```

公共回归覆盖低额度截断后成功、第二次仍截断、第二次错误、两次尝试中的超时、重复及待核验直接回退、4096截断直接回退、明确视觉修正、无修正的视觉失败、第二次期间取消、下一作业隔离及篡改尝试记录拒绝。Schema 和重新导出覆盖图片、PDF、旧版1.6及更早保存契约。[两轴审查](code-review.md) 发现的问题均补回归并修复。

真实 MNN 最终结果见 [日志](evidence/real-final.log) 和 [摘要](evidence/real-summary.json)：区域开始后取消安全返回，下一作业在8 token截断后提升到4096，完整匹配固定中文正文；1毫秒生成预算触发真实超时，没有重试，恢复120秒预算后同一引擎再次匹配正文。超时的实际生成耗时为2408毫秒，证明预算检查不等于墙钟硬上限。恢复结果、超时结果及超时后结果均通过生产重新导出，JSON、Markdown 和所有资源逐字节一致。证据中的原图资源保存在本机 `output/issue22-real-final/`，不提交模型、构建产物或图片资源。

首轮真实脚本的最后状态检查曾失败，保留在 [原始日志](evidence/real-initial.log)。[定向复查](evidence/blank-recovery-check.log) 中超时后的两个后续作业均返回且引擎就绪；最终验收改用具有固定正文参考的正常页面验证实际识别恢复，没有以空白输出替代正常识别证据。

最终 [Python unittest](evidence/final-unittest.log) 25/25通过；完整 [CTest](evidence/final-ctest.log) 23/23通过。这些证据验证 #22 的重试和恢复行为，不代表 #16 的整页产品质量验收已经完成。
