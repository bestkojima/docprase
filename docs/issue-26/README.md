# Issue #26：识别前结构、标签映射与图片资源

对应 [Issue #26](https://github.com/bestkojima/docprase/issues/26)。最新实现为 DocumentIR 1.9，已通过 [完整20页生产结构验收](acceptance-20-v1.9.md) 与25项 CTest。实现与证据已提交到 `codex/issue26-region-mapping`，#26进度已更新；分支合并前保留 OPEN。

## 当前实现与标签规则

Ovis 只识别 text / formula / table。其他文字细类集中映射到三类；内嵌公式由所属 text 输出，表格子块由 table 输出。image 保存为资源并跳过 Ovis，由 Markdown 引用图片地址。后续其他 label 可按明确策略 skip，不把资源、已归属子块或 skip 计为 OCR 成功。

- [后续标签处理契约](label-policy.md)：已写入 #28/#29/#27。
- [统一映射计划与实现边界](region-mapping-plan.md)。
- [odb-07/08 实测与完整 Markdown](region-mapping-07-08.md)。
- [DocumentIR 1.9 image schema](document-ir-1.9-image.schema.json)、[PDF schema](document-ir-1.9-pdf.schema.json)。
- [25项回归日志](evidence/region-mapping-ctest.log)。

## 历史验收与范围

[1.8完整20页验收](acceptance-20.md) 是上一候选的已完成结果，不能替代最新1.9的完整验收。[横排选项结构修复记录](structure-fix.md) 保存原 C/D 错序、公共回归和冻结重放证据。第17/18题八个标签绑定、公式/表格唯一归属以及旧版导出兼容继续保留。

[范围调整与后续任务](scope/README.md)：官方后处理链审计在 #28，水印/装饰及 skip 策略在 #29，完整内容与质量验收在 #27/#24。原规格与失败记录继续保留，不把结构验收称为整体 OCR 质量通过。

## 中间量 PNG

每页输出五个阶段：分数筛选候选、后处理选中框、最终区域、阅读顺序、原始 GT。红框是图片，蓝框是文字，紫框是公式，绿框是表格；原始候选编号、rank、mask和原框可追溯。

最新1.9重点图：[第17题](acceptance-1.9-png/odb-03/q17-comparison.png)、[第18题](acceptance-1.9-png/odb-03/q18-comparison.png)、[odb-07](acceptance-1.9-png/odb-07/04-reading-order.png)、[odb-08](acceptance-1.9-png/odb-08/04-reading-order.png)。

上一轮1.8重点图：[第17题](acceptance-png/odb-03/q17-comparison.png)、[第18题](acceptance-png/odb-03/q18-comparison.png)。完整历史中间量和 ZIP 保留在本地 `docs/issue-26/annotations/` 与生产输出目录；Git只提交报告、哈希和重点图。

完整生产作业、原始张量/mask/裁图及全部20页中间量可由 `scripts/issue26_run.py`、`scripts/issue26_acceptance.py`、`scripts/issue26_annotations.py` 复现。真实模型推理、冻结历史重放、从已有结果重画 PNG 分别记录，不相互冒充。
