# Issue #20：包含关系、几何处理与内容归属记录

核验日期：2026-09-27。参照为 PaddleX `release/3.7` 固定提交
[`ffb64904d23708863ff5b8da312a5cbd52a7f462`](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/models/layout_analysis/processors.py)。模型来源与 MNN 张量契约沿用 `docs/issue-19/layout-dedup.md` 的核验结果。

## 已落地的处理链

模型返回 300 行候选及一一对应的 300 行 mask；每行保留原候选 ID、原框、rank 和 mask 行号。生产作业先按本地安全约束验证框与页面交集，再执行 #19 的分数筛选、NMS、大图片筛选。本票新增包含关系筛选：按交集/小框面积 `>= 0.9`，对类别 ID `3,5,6,15,17` 执行 `large` 规则，其余类别按 `union` 保留原框。类别 5 公式受官方 `check_containment` 的方向保护。筛除原因写入 `layout_diagnostics.candidates[].filter_reason`，原框和 rank 仍留在候选诊断中。

其后按页面裁剪框执行外层重叠过滤：同一内容区域的交集/小框面积 `> 0.7` 时保留面积较大的框；`reference` 类别过滤。表格内文字、行内公式及图注/脚注按内容归属与语义关联规则保护，不因外层重叠误删。表格内文字和行内公式只要至少 90% 落在父块内，仍由现有区域规划唯一归属给表格或父正文，保留子 LayoutBlock 与 `content_owned_by` 关系。相邻文字框没有合并入口，默认配置也没有合并实验。

`layout_unclip_ratio` 固定为 `[1.0,1.0]`，扩框阶段为恒等操作。原始浮点框保存在 `original_bbox`，页面边界内的整数裁图框保存在 `crop_bbox`；选中 LayoutBlock、Region、BlockResult 的来源 ID 与资源路径可回映原页。输出按现有阅读顺序模块处理 rank 与双栏几何冲突；mask 行号始终是原候选 ID，过滤与排序均不重排原 mask。版面专用作业的未识别表格以 `table: null` 和空 Markdown 占位写出，使 JSON 重新导出符合 DocumentIR 1.3。

## 与官方完整契约的差异

这些差异不能被称为完整 PaddleX 等价：

- 现有生产分数阈值为 `>= 0.5`，官方管线为 `> 0.3`；本票不擅自更改 #19 冻结基线。
- 官方在 NMS 前对框坐标做 `np.round`，本地 #19 的 NMS 沿用原始浮点框；尚未取得足够的相同候选数值对照来证明两者可互换。
- 官方 `filter_boxes` 删除宽或高小于 6 像素的框，并可能删除被正文覆盖的图注。本地阅读顺序回归实际出现标题和图注漏块，因而保留这些短框及合法注释块。小于 6 像素的 2×2 历史契约夹具仅验证旧几何边界，不执行新增包含/外层过滤。
- 本地 mask 保存原始 RLE 与页面叠加资源，类别/rank/mask 行对应可核对；尚未复现官方 `auto` mask→polygon/quad 的 OpenCV 几何提取。当前识别裁图为矩形，不能宣称多边形级对齐。
- 本地对先验非法框和页面边界先做安全约束，官方结构化边界处理在扩框之后。对越界但仍与页面相交的框，原框与裁框均保留；更复杂的顺序差异仍需同候选核验。

## 验证

`tests/issue20_geometry.py` 从公共 CLI 作业入口检查类别包含、行内公式与表格子块保留、外层重复框、rank/mask 行对应、越界回映、双栏顺序、裁图资源及 JSON 重新导出。既有 `printed_table_integration`、`reading_order_integration` 和 #19 去重用例继续覆盖真实内容归属与旧边界。代表性真实整页 `tests/issue19_real.py` 在生产 MNN CLI 上仍保留候选 ID `0..15`、16 个块和 16 个阅读顺序项，JSON/Markdown 重新导出一致。另在 OmniDocBench 开发集 `odb-01`、`odb-13`、`odb-20` 运行生产版面 CLI，与旧整页候选输出比较后分别保留 57/57、16/16、15/15 个候选，无新增漏块。`odb-01` 曾因外层规则误删一个正文覆盖率 93% 的公式框，改为保留并按父正文归属后通过；这三页仍不能代替 20 页完整质量验收。

本票的完整官方契约、20 页真实整页与无新增漏块验收仍未证实；其余差异保留在本记录，后续必须按相同候选及真实页面复核，不能因受控夹具通过而关闭质量条件。
