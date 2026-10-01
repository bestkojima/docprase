# 历史 Issue 证据索引

本索引将已提交材料按领域串联，不搬迁或重写历史证据。任务状态快照核对于北京时间 2026-10-02；实时状态以 [GitHub Issues](https://github.com/bestkojima/docprase/issues) 为准。当前操作见[现行文档索引](README.md)，生产格式见[稳定 Schema](../schemas/document-ir/README.md)。

## 首轮 Linux 实现

T01～T14 是 #2～#15 的任务别名，均已关闭。最后两票的实际验收范围修订为 Linux；早期 Windows 设计不能作为 Windows 已验收的证据。

| 领域 | Issue | 已提交证据入口 |
|---|---|---|
| Layout 真实模型契约与对齐 | [#2](https://github.com/bestkojima/docprase/issues/2) | [对齐报告](research-evidence/runtime/layout/issue-2-report.md) |
| Ovis 转写与会话隔离 | [#3](https://github.com/bestkojima/docprase/issues/3) | [探针与隔离记录](research-evidence/runtime/ovis/issue-3/README.md) |
| 单页公共作业与配置计划 | [#4](https://github.com/bestkojima/docprase/issues/4)、[#5](https://github.com/bestkojima/docprase/issues/5) | [作业契约](issue-4/README.md)、[配置验收](issue-5/README.md) |
| 真实版面与双模型正文 | [#6](https://github.com/bestkojima/docprase/issues/6)、[#7](https://github.com/bestkojima/docprase/issues/7) | [Layout 作业](issue-6/README.md)、[真实单页](issue-7/README.md) |
| 公式、表格与阅读顺序 | [#8](https://github.com/bestkojima/docprase/issues/8)、[#9](https://github.com/bestkojima/docprase/issues/9)、[#10](https://github.com/bestkojima/docprase/issues/10) | [公式](issue-8/README.md)、[表格](issue-9/README.md)、[顺序](issue-10/README.md) |
| PDF、作业控制与再导出 | [#11](https://github.com/bestkojima/docprase/issues/11)、[#12](https://github.com/bestkojima/docprase/issues/12)、[#13](https://github.com/bestkojima/docprase/issues/13) | [PDF](issue-11/README.md)、[作业控制](issue-12/README.md)、[再导出](issue-13/README.md) |
| Linux 入口与综合验收 | [#14](https://github.com/bestkojima/docprase/issues/14)、[#15](https://github.com/bestkojima/docprase/issues/15) | [Linux 入口](issue-14/README.md)、[综合验收](issue-15/README.md)、[旧 7 页清单](issue-15/samples.json) |

## 正确性修复与质量评测

| 领域 | Issue 与快照状态 | 证据及实际结论 |
|---|---|---|
| 20 页整页基线 | [#17](https://github.com/bestkojima/docprase/issues/17)，完成关闭 | [口径](issue-17/README.md)、[结果](issue-17/results.md)、[机器报告](issue-17/report.json)；失败页计入完整分母，属于开发基线 |
| 小裁图与视觉拦截 | [#18](https://github.com/bestkojima/docprase/issues/18)，完成关闭 | [策略、真实图注/页眉及连续作业](issue-18/verification.md) |
| 版面去重 | [#19](https://github.com/bestkojima/docprase/issues/19)，完成关闭 | [冻结参照与验收](issue-19/layout-dedup.md) |
| 包含、几何及内容归属 | [#20](https://github.com/bestkojima/docprase/issues/20)，替代关闭 | [实现与剩余缺口](issue-20/geometry-and-ownership.md)；未完成范围先由 #26 承接，再分至 #27/#28/#29 |
| 异常隔离与定向重试 | [#21](https://github.com/bestkojima/docprase/issues/21)、[#22](https://github.com/bestkojima/docprase/issues/22)，完成关闭 | [隔离规则与回归](issue-21/README.md)、[重试和真实恢复](issue-22/README.md) |
| 带标注评测材料 | [#23](https://github.com/bestkojima/docprase/issues/23)，按修订范围完成关闭 | [材料及恢复说明](issue-23/README.md)、[用户修订](issue-23/evidence/requirements.json)、[逐页核验](issue-23/evidence/verification.json)；12 页就绪，未声明严格独立留出 |
| 整页回归与原门槛 | [#24](https://github.com/bestkojima/docprase/issues/24)，开放 | [结果](issue-24/results.md)、[机器报告](issue-24/report.json)、[门槛](issue-24/thresholds.json)、[工程条件](issue-24/engineering-conditions.json)；该候选工程与质量条件未全部满足 |
| 12 页冻结质量验收 | [#25](https://github.com/bestkojima/docprase/issues/25)，用户豁免结案 | [结果](issue-25/results.md)、[机器报告](issue-25/report.json)、[复现与冻结说明](issue-25/README.md)；原九指标通过 2/9，实测未达标 |
| 识别前结构与图片/标签关系 | [#26](https://github.com/bestkojima/docprase/issues/26)，完成关闭 | [实现](issue-26/README.md)、[1.9 全部 20 页结构验收](issue-26/acceptance-20-v1.9.md)、[原错误图片复验](issue-26/closure-verification.md)、[标签契约](issue-26/label-policy.md)；结构验收与全文质量分别判定 |
| 质量修复及多 GT 内容核验 | [#27](https://github.com/bestkojima/docprase/issues/27)，开放 | [规格快照](issue-27/issue.json)、[对齐口径](issue-27/alignment-policy.md)；最终完整交付和原门槛复验继续由本票跟踪 |
| 官方完整后处理核验 | [#28](https://github.com/bestkojima/docprase/issues/28)，开放 | [从 #26 拆出的范围](issue-26/scope/official-postprocess.md)；完整核验仍待完成 |
| 水印与装饰误检 | [#29](https://github.com/bestkojima/docprase/issues/29)，开放 | [误检任务范围](issue-26/scope/watermark-detections.md)；过滤策略与有效插图保护仍待验收 |

父规格 [#1](https://github.com/bestkojima/docprase/issues/1) 和 [#16](https://github.com/bestkojima/docprase/issues/16) 仍开放。#26 的实现已由 [PR #30](https://github.com/bestkojima/docprase/pull/30) 合并；合并状态与各轮质量分数分别记录。

## 材料与版本身份

| 材料 | 身份与用途 |
|---|---|
| [OmniDocBench 20 页清单](omnidocbench-20/manifest.json) | 开发/评测材料，允许诊断与调参；不是严格独立留出集 |
| [旧 7 页清单](issue-15/samples.json) | 六份输入、七页固定回归，参考摘录与整页标注分开使用 |
| [12 页带标注材料](issue-23/manifest.json) | 图片与原始参考按固定 revision/SHA 封存；用户修订取消来源隔离和历史曝光准入要求 |
| [原冻结门槛](issue-24/thresholds.json) | 原九项指标、分母与失败计分；整理文档不会改变门槛 |
| [#25 冻结副本](issue-25/evidence/freeze/) | 包含当时实际使用的评分工具与材料记录；不能用新工具给旧执行补标新版本 |
| [Schema 字节清单](../schemas/document-ir/SHA256SUMS) | 同时记录稳定副本和历史来源的 SHA，Schema 内容及 `$id` 保持原字节 |

运行或重放一轮前，从该轮说明定位候选、实际二进制/运行时、配置、模型、输入、参考、评分工具和产物哈希。`output/` 中的原图、模型输出资源和 ZIP 通常不入 Git，应按对应说明恢复并核验。缺少这些工件时，不把文档链接存在当成已完成复验。

## 现行引用与历史引用

- CMake 和当前公共作业/重新导出回归读取 `schemas/document-ir/`；更新当前引用时保持全部旧版本兼容。
- 原始 Issue 说明、封存 SHA、冻结评分脚本与候选回放继续使用历史路径。`tests/issue24_real.py`、`tests/issue25_real.py`、`tests/issue27_real.py` 及历史轮次脚本属于这类版本绑定的验证器，保留原文件，按对应轮次使用。
- 本地未提交的 #27 完整产物及其他研究补充保持原状，不因本索引新增而被宣称已经提交或验收。
- 后续归并 Issue 时，将剩余验收要求、阻塞关系和证据链接写入承接票，再按实际原因关闭旧票；已完成、被替代和豁免结案分别表述。
