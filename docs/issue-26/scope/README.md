# Issue #26 范围调整

2026-10-01 用户确认采用收窄建议。已更新 GitHub #26、#27、#24，并创建 #28、#29；各 Issue 的发布正文均已读回核对。原规格保存在 [issue-26-before.json](issue-26-before.json)，原依赖票正文也分别保留，未将迁出的未完成项改称已验收。

| 工作 | 调整后归属 |
|---|---|
| 识别前固定 Region、裁图、唯一所有权、图片组及调度；横排选项顺序、标签绑定、等价导出 | [#26](https://github.com/bestkojima/docprase/issues/26) |
| 官方完整后处理链、类别/mask/rank/边界对照与合理差异解释 | [#28](https://github.com/bestkojima/docprase/issues/28) |
| 水印/装饰误检诊断、过滤与有效插图保护 | [#29](https://github.com/bestkojima/docprase/issues/29) |
| 全量多 GT/单 Region 内容对齐、评分粒度扩展、原质量门槛重新验收 | [#27](https://github.com/bestkojima/docprase/issues/27)，由 [#24](https://github.com/bestkojima/docprase/issues/24) 集成 |
| 公式校验、异常输出及针对性重试 | [#21](https://github.com/bestkojima/docprase/issues/21)、[#22](https://github.com/bestkojima/docprase/issues/22) |

几何身份、裁图和内容所有权在识别前固定；证据不足的语义关联允许保持不确定，不要求在首次 OCR 前解决全部语义。DocumentIR 保存未确定引用，Markdown 提供对应裁图与“图注与图片的对应关系尚未确认，请核对原图。”提示；已有识别失败状态单独保留。

GT 框数不要求与 Region 数相同，内嵌公式由所属 text Region 输出是合法粒度。完整内容核验继续在 #27 跟踪；不得把评分口径变化当作识别改善。#25 按用户此前指令豁免结案，其质量未达标实测及 #27 原冻结门槛保持原样。

日常迭代使用受控公共回归和冻结张量/转写重放；最终交付仍保留一次完整20页的新生产识别，逐页核对真实结构、来源、资源与导出。仅已有 OCR 错字或 partial 状态不阻断结构票，结构修改造成的新增确认的内容遗漏、错序、重复或误绑定仍须修复。

发布正文分别见 [issue-26.md](issue-26.md)、[issue-27.md](issue-27.md)、[issue-24.md](issue-24.md)、[official-postprocess.md](official-postprocess.md)、[watermark-detections.md](watermark-detections.md)。当前实现与验收证据见 [结构修复说明](../structure-fix.md)。

后续 #28/#29/#27 已按用户要求追加 [标签处理契约](../label-policy.md)：Ovis只有三类识别，其他文字标签集中映射，image走资源路径；其他label可添加明确skip并保留处理原因。最新交付见 [1.9完整20页验收](../acceptance-20-v1.9.md)。
