# Issue #22 两轴代码审查

按照 `code-review` 技能分别以两个只读子代理审查规范与规格。初审比较范围为 `b31fb74...b4ea9984799bc657c20ccd04fae9ebe9d89e338a`；后者为 `commit-tree` 创建的暂存审查快照，没有移动工作分支。后续在工作树复查修复；用户原有未提交文档、配置及研究文件不在本次范围内。

## Standards（规范）

未发现硬性仓库规范违规或需要报告的判断性代码气味。中文说明、GitHub Issue 关联、领域术语及公共作业边界符合约定。

初审发现一项 P2 功能问题：普通后端异常的业务判定原因被包装为 `region_inference_exception:<原错>`，重新导出重算原始后端错时拒绝这份合法文档。公共作业复现和 [红灯记录](evidence/review-red-test.log) 已保留。修复仅允许失败状态下精确匹配这个包装原因，继续核对原错、原始输出和采用尝试。`first-exception` 回归及规范轴复查通过。

最终结论：硬性违反0项；功能问题1项已修复；未发现新增问题。

## Spec（规格）

初审发现两项 P2，均违反 #22 验收4的重新导出一致性要求。

1. 视觉重试在第二次重置或执行异常时没有实际视觉变换，重新导出却要求第二次实际变换优于第一次，拒绝正确回退的文档。修复保存独立 `input_visual` 计划变换，验证像素预算、画布、缩放、补白及取整，允许失败尝试没有实际视觉处理结果。`visual-exception` 公共回归先失败（见 [红灯](evidence/review-visual-red-test.log)）、修复后通过。
2. 首次截断输出使用 Base64 保存无效 UTF-8 时，重新导出将其恢复为空，误判为不允许重试的空输出。修复严格解码 Base64 并以实际原始字节重判；`encoded-first-cut` 验证首轮字节保留、第二次恢复正常正文和重新导出一致。代理的初审复现与修复复查均已完成。

规格轴还复查了普通异常包装的修复。最终结论：初审问题2项均关闭；没有发现范围扩张或新增缺陷。

## 最终验证

[修复定向回归](evidence/review-green-tests.log) 6/6通过，覆盖重试、异常隔离、重新导出、PDF、作业控制及取消。规格复查额外运行单个 `issue22_retry`，1/1通过；两轴没有重复运行全量测试。

完整 CTest、Python unittest 和最终真实后端记录分别见 [CTest](evidence/final-ctest.log)、[unittest](evidence/final-unittest.log)、[真实日志](evidence/real-final.log)。最终1.7真实文档已按新增计划变换字段刷新，未以旧证据代替最终契约。

两轴分别统计：规范轴硬性违反0项、功能问题1项已修复；规格轴问题2项已修复。均无未解决发现。

原始证据保留导出与 stderr 的尾部换行：`real-recovered/document.md`、`real-after-timeout/document.md`、`review-red-test.log`、`review-visual-red-test.log`。`git diff --check` 仅排除这四份字节证据，其余暂存改动通过检查；导出文档的 SHA-256 记录在 `real-document-sha256.json`。
