# 现行文档索引

本页是当前代码与运行行为的阅读入口。GitHub Issues 记录任务与规格；[历史证据索引](evidence-index.md) 记录每轮交付的来源。Issue 目录中的报告、日志和冻结副本保留当时的结论，不能直接当作当前检出版本的验收结果。

## 运行与接入

| 需要了解的内容 | 入口 |
|---|---|
| 当前能力、平台范围与质量限制 | [仓库 README](../README.md) |
| Linux 构建、图片/PDF、页范围和重新导出 | [运行指南](linux-current.md) |
| 公共作业与结果/资源的内存所有权 | [C ABI](../include/dococr/dococr.h) |
| 模型工件、能力绑定和实际执行预算 | [生产配置](../configs/printed-page.example.json)、[配置校验](../src/config.cpp) |
| 当前数据格式及旧版本兼容 | [DocumentIR Schema](../schemas/document-ir/README.md) |

本项目使用页面、版面块（LayoutBlock）、识别区域（Region）、区域识别结果（BlockResult）和文档中间表示（DocumentIR）这些术语。版面块与识别区域不要求一一对应；内容归属决定由哪个区域输出，阅读顺序决定最终展示顺序。根目录已有 `CONTEXT.md` 时，以其中的领域词表为准。

## 当前行为与实现来源

| 行为 | 实现入口 | 已提交的规则与验收来源 |
|---|---|---|
| 版面候选、筛选、归属及识别前计划 | [core.cpp](../src/core.cpp)、[区域结构](../src/region_structure.cpp) | [去重](issue-19/layout-dedup.md)、[几何与归属](issue-20/geometry-and-ownership.md)、[结构修复](issue-26/structure-fix.md) |
| 三类识别、图片资源、标签导出及显式跳过的记账 | [标签映射](../src/layout_region_policy.cpp)、[导出策略](../src/label_export_policy.cpp) | [当前标签流水线](label-pipeline.md)、[1.9 历史契约](issue-26/label-policy.md) |
| 小裁图适配与视觉成功证据 | [视觉适配](../src/visual_adaptation.cpp)、[MNN 识别后端](../src/printed_page_mnn_backend.cpp) | [视觉适配验收](issue-18/verification.md) |
| 异常、不完整、失败与待核验展示 | [输出判定](../src/output_assessment.cpp)、[Markdown](../src/markdown.cpp) | [异常隔离](issue-21/README.md) |
| 定向重试与作业取消/恢复 | [区域识别](../src/region_recognition.cpp)、[C ABI 实现](../src/abi.cpp) | [重试与恢复](issue-22/README.md) |
| 已保存 JSON 的校验和重新导出 | [重新导出](../src/reexport.cpp)、[Schema 校验](../src/schema_validator.cpp) | [再导出验收](issue-13/README.md)、[当前契约](../schemas/document-ir/README.md) |

Ovis 的任务为 text / formula / table。图片作为 image 资源保存；已归属的内嵌公式和表格子块由父区域输出一次。资源、子块归属和显式 skip 分别记账，不作为 Ovis 识别成功。具体类别与过滤策略以源码和对应版本记录为准。

## 质量评测入口

- [20 页开发基线](issue-17/README.md)、[固定数据清单](omnidocbench-20/manifest.json)。
- [旧 7 页回归清单](issue-15/samples.json)、[12 页带标注材料](issue-23/README.md)。
- [原冻结门槛](issue-24/thresholds.json)、[开发集整页回归](issue-24/results.md)。
- [12 页历史验收结果](issue-25/results.md)：#25 的用户豁免结案与实测质量未通过分别记录。
- [多 GT 内容对齐口径](issue-27/alignment-policy.md)、[质量修复任务 #27](https://github.com/bestkojima/docprase/issues/27)。

当前历史评测工具按候选与版本组织，运行命令在对应 Issue 的说明中。重放旧结果使用当时的评分工具、冻结副本和材料身份；新代码必须建立新候选，不能把新构建或文档整理的结果绑定到旧轮。

## 文档维护规则

1. 当前操作和能力入口放在 README、本页及运行指南；稳定格式放在 `schemas/document-ir/`。
2. 规格和待办在 GitHub Issues 中维护。现行规则保留来源 Issue 链接，避免将整轮日志复制进说明。
3. 历史 `docs/issue-*`、原始输出、失败轮次、SHA 清单及冻结副本保留原路径和原字节；通过证据索引查找。
4. 不可轻易逆转的领域决策记录在 `docs/adr/`；维护根目录的单一领域词表，不按 Issue 重建多套术语。
5. 旧标题、平台计划和发布快照只代表当时范围；读取任务状态时核对远端 Issue，区分完成、替代关闭、豁免结案及待验收。
