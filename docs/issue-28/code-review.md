# Issue #28 双轴代码审查

使用 `code-review` 技能，由两个独立子代理分别审查。基点 `ce0dbaed6faea125000f0ffcfbc4e7f114f9067d`；初审 `3eede0c`，第一次修复 `6ef3fad`，最后复核包括公共路由完整性修复和负例测试。范围仅本任务提交，不包括已有工作区修改。

## Standards

未发现 `AGENTS.md`、`docs/agents/` 或领域术语的硬规范违规。

- 初审发现公共作业通过条件只比较候选集合，归属与资源检查虽在归档时独立执行，但没有纳入复现命令。已把父子唯一 Region/Block、归属 owner/source 和全部保留类别的 LayoutBlock→Region→Block 完整对应纳入 runner。
- 复审发现“仅检查已经存在的 image Block”仍会让全部图片输出缺失时通过。已要求每个保留类别恰好一个正确类型的 Region 和输出，五种 image 都必须有资源且没有 Ovis 调用。删除全部 image、错误识别任务和重复输出负例均失败。
- 可选维护建议 `possible Duplicated Code`：两处 CLI 测试准备逻辑重复。已提取统一 `run_reference_cli`。

最后只读复核：上述发现均可关闭，无新增实质问题。硬规范未解决 0，正确性未解决 0，维护建议未解决 0。

## Spec

初审两项 P2：

1. 原规格“已确认属于正文的内嵌公式由父 text 一次输出；表格文字/公式子块由父 table 一次输出，不重复调用 Ovis。”原 runner 不能因归属、路由或输出遗漏而失败。已用公共 DocumentIR 完整关联断言修复，并加入行为负例。
2. 原规格“原框、裁图框、rank、mask 行与处理原因可追溯。”初版将所有外层删除只记为 `outer_overlap`，无法区分短框和类别排除。已保留阶段字段，新增观察官方实际分支所得的 `removal_reason` 与 `related_candidate_id`；不重新实现重叠决策。

最后复核：两个 P2 均可关闭，实跑 6 个 Issue #28 测试通过，没有新的实质问题。README 如实保留真实短标题、reference 删除内容安全和全量 Ovis 等未通过状态，没有伪称整体精度通过；未发现范围外生产策略改动。

最终未解决发现：Standards 0；Spec 0。规格中明确保留的未通过核验项目见 README，不因代码审查通过而变成已验收。
