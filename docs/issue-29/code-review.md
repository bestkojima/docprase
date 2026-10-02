# Issue #29 双轴代码审查

按照 implement 要求使用 `/code-review`，固定基线 `235bc9f5922b7941a80671dc11709dc4484d3adc`，在提交前针对 `git diff --cached 235bc9f --` 派发两个独立只读审查代理。其他既有未暂存改动不在本次范围内。

## Standards

硬性违规 **0 项**。文档使用简体中文，并区分候选、Region、内容归属与识别结果。

判断性建议 **1 项，非阻塞**：`src/pdf_job.cpp` 的版本聚合继续沿用已有多分支判断，每增加 Schema 版本都需补排除条件，属于可能的 Repeated Switches。后续可集中定义版本顺序；当前分支未发现实际错误，本票不扩大重构范围。

## Spec

问题 **0 项**。精确绑定原页、实际裁图、候选和 mask；只允许 class 14 明确确认 skip；模糊和过期证据保留；三类 Ovis 任务和失败记账不变；Schema/PDF/重新导出同步支持；验证方法覆盖内容、裁图、顺序、选项组和唯一归属，并区分新 Layout 推理与历史输出重放。

代理结论针对实现与验证方法；最终执行结果另由主代理核对 `evidence/verification.json` 及测试日志。Standards：0 硬性违规、1 低优先级建议；Spec：0 问题。
