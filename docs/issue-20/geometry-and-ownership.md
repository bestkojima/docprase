# Issue #20：包含关系、几何处理与内容归属记录

状态更新（2026-10-01）：用户要求将旧任务按范围更新关闭，未完成验收迁移至 [Issue #26：在 Ovis 前形成版面区域计划并修复选项图片与标签关联](https://github.com/bestkojima/docprase/issues/26)。关闭不代表完整官方后处理或真实整页内容归属验收已通过；以下保留历史实验与缺口。

核验日期：2026-09-30。参照为 PaddleX `release/3.7` 固定提交
[`ffb64904d23708863ff5b8da312a5cbd52a7f462`](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/models/layout_analysis/processors.py)。模型来源与 MNN 张量契约沿用 `docs/issue-19/layout-dedup.md` 的核验结果。

## 已落地的处理链

模型返回 300 行候选及一一对应的 300 行 mask；每行保留原候选 ID、原框、rank 和 mask 行号。生产作业先按本地安全约束验证框与页面交集，再执行 #19 的分数筛选、NMS、大图片筛选。本票新增包含关系筛选：按交集/小框面积 `>= 0.9`，对类别 ID `3,5,6,15,17` 执行 `large` 规则，其余类别按 `union` 保留原框。类别 5 公式受官方 `check_containment` 的方向保护。筛除原因写入 `layout_diagnostics.candidates[].filter_reason`，原框和 rank 仍留在候选诊断中。

其后按页面裁剪框执行外层重叠过滤：同一内容区域的交集/小框面积 `> 0.7` 时保留面积较大的框；`reference` 类别过滤。表格内文字至少 90% 落在父表内、行内公式至少 85% 落在父正文内时，便保留子 LayoutBlock 并由区域规划唯一归属给父块，记录 `content_owned_by` 关系。与正文框重叠的图注/脚注若面积差距明显则保留；大小相近的重复框仍由外层规则筛除。相邻文字框没有合并入口，默认配置也没有合并实验。

`layout_unclip_ratio` 固定为 `[1.0,1.0]`，模型框扩展阶段为恒等操作。原始浮点框保存在 `original_bbox`，页面边界内的整数框保存在 `crop_bbox`。归属子块可能有少量边缘伸出父块，父块的识别裁图因此取父子框并集；LayoutBlock 仍保留原版面框，Region 和 BlockResult 使用实际识别裁图框。诊断新增 `recognition_crop_bbox` 和 `crop_expansion_reason: owned_content_union`，只在裁图扩大时赋值。这是归属内容的裁图修复，不合并相邻正文块。

选中 LayoutBlock、Region、BlockResult 的来源 ID 与资源路径可回映原页。输出按现有阅读顺序模块处理 rank 与双栏几何冲突；mask 行号始终是原候选 ID，过滤与排序均不重排原 mask。版面专用作业的未识别表格以 `table: null` 和空 Markdown 占位写出，使 JSON 重新导出符合 DocumentIR 1.3。

## 与官方完整契约的差异

这些差异不能被称为完整 PaddleX 等价：

- 未指定参数时沿用 `auto` 和 `>= 0.5`。经开发集对照，两个示例配置显式选择 `smartresize_lanczos` 与 `>= 0.3`；官方管线为 `> 0.3`。这些是开发集调优结果，不是官方默认参数。#19 数值回归显式使用原 `reference/0.5` 配置。
- 官方在 NMS 前对框坐标做 `np.round`，本地 NMS 沿用原始浮点框；同候选实验中提前取整少匹配一个标注，因而没有更换本地数值规则。
- 官方 `filter_boxes` 删除宽或高小于 6 像素的框，并可能删除被正文覆盖的图注。本地阅读顺序回归实际出现标题和图注漏块，因而保留这些短框及合法注释块。小于 6 像素的 2×2 历史契约夹具仅验证旧几何边界，不执行新增包含/外层过滤。
- 本地 mask 保存原始 RLE 与页面叠加资源，类别/rank/mask 行对应可核对。实验执行冻结源文件中的官方 `auto` mask→polygon/quad 几何提取，记录与矩形模式的定位差异；生产识别裁图仍为矩形，不能宣称多边形级对齐。
- 本地对先验非法框和页面边界先做安全约束，官方结构化边界处理在扩框之后。对越界但仍与页面相交的框，原框与裁框均保留；更复杂的顺序差异仍需同候选核验。

## 历史基线验证

`tests/issue20_geometry.py` 从公共 CLI 作业入口检查类别包含、约 86% 公式/90% 表格子块覆盖边界、相近尺寸图注重复框、rank/mask 行对应、越界回映、双栏顺序、裁图资源及 JSON 重新导出。既有 `printed_table_integration`、`reading_order_integration` 和 #19 去重用例继续覆盖真实内容归属与旧边界。代表性真实整页 `tests/issue19_real.py` 在生产 MNN CLI 上仍保留候选 ID `0..15`、16 个块和 16 个阅读顺序项，JSON/Markdown 重新导出一致。

OmniDocBench 20 页开发集使用生产版面 CLI 全部运行：18 页在旧 `max_page_pixels=16000000` 范围内，候选 ID 集合与 #17 旧整页输出逐页完全一致；`odb-11`、`odb-17` 原图分别为 5556×8175、5556×8125，走整页 smartresize 后也完成版面作业，分别保留 9、47 个候选。这两页没有旧版候选输出可供同候选对比。`odb-01` 曾因外层规则误删一个正文覆盖率 93% 的公式框，`odb-06` 曾误删两个正文覆盖率约 88%–90% 的公式框；改为保留并按父正文归属后，18 页不再有新增候选漏块。此对照只核验版面候选，不代表 20 页完整识别质量验收。

## 整页输入适配

Layout 作业的解码安全上限为 6400 万像素，实际页面必须严格满足配置的 `max_page_pixels`；1600 万配置不能隐式放行更大的原图。Layout 与印刷页示例显式配置为 6400 万，普通后端仍限制为 1600 万。PDF 在栅格化前按配置和请求 `max_page_pixels` 两者中较小者检查，同样允许显式预算内的大页进入后续适配。原图超过 1600 万像素时，DocLayout 前将整页按单一比例缩到 800×800 画布内并居中补白，保持内容纵横比（整数内容尺寸产生小于一像素的取整差异）；模型 `scale_factor` 固定为 1，模型框按实际整数内容尺寸与补白逆变换到原页，原始张量仍留在资源中。`layout_diagnostics.input_transform` 记录源图、画布、内容尺寸、补白和逆仿射。原页尺寸、LayoutBlock/Region/BlockResult 框和裁图资源仍在原页坐标；mask 行号仍按原候选 ID，页面 mask 使用相同变换回映。诊断叠加图改在 800×800 画布上绘制，以免大页的诊断 PNG 独占输出预算。普通不超过 1600 万像素的页面继续走原来的固定参考预处理，避免改变既有输出。

`tests/issue20_large_page.py` 通过公共 CLI 验证 4100×4200 的完整图片和 PDF：旧预算拒绝、显式预算放行、PDF 请求预算在栅格化前拒绝、补白参数、框回映、非零 mask 的原页像素位置、原尺寸裁图的红色标记内容、schema 与 JSON/Markdown 重新导出。生产 MNN 对 `odb-11` 与 `odb-17` 分别成功输出 9 与 47 个块；`odb-17` 的 47 个框均在原页范围内、诊断预览为 800×800、作业产物约 54 MiB，JSON/Markdown 重新导出一致。修正预算绕过后，两页在显式 `max_page_pixels=64000000` 下再次运行，完整 DocumentIR 与前次结果逐字段一致；新产物为 `output/issue-20/odb-11-smartresize-budget/` 与 `output/issue-20/odb-17-smartresize-budget/`，运行清单记载实际配置预算。该路径目前只在真实超大页触发，模型候选质量仍需与原始整页标注单独核验。

以上是早期 `auto/0.5` 路径的记录。现在可通过 `execution.layout_preprocess` 显式选择 `reference`、`auto`、`smartresize_bilinear`、`smartresize_area`、`smartresize_lanczos`；三个显式 smartresize 模式对预算内的每页均保持比例缩放和补白。`input_transform.resample` 与运行清单记录实际方法，`execution.layout_score_threshold` 接受 `[0,1]` 范围内的有限数字。

## 对照实验与当前验收状态

实验方法、逐页证据和真实识别结果见 [experiments.md](experiments.md)。七种预处理、八档阈值、三种矩形处理规则、官方 auto 模式及归属阈值组成 201 个开发配置。受控用例新增 `tests/issue20_tuning.py` 和 `tests/issue20_owned_crop.py`，验证抗锯齿张量、参数拒绝、原页回映、父子裁图并集、扩图跨中线时的顺序、同一行图片/短图注的一像素top抖动和 JSON 重新导出。几何分栏以页面中线为界，行锚点使用较短框高度10%（至少2像素）的top容差，排序只作用于几何回退，有效模型rank仍优先。

面积采样取得最高定位 F1，但新增一处正文漏检，未选入示例。Lanczos/0.2 保留了该处正文，但真实转写出现重复图注，未被选用。最终 Lanczos/0.3 在现行规则的统一输入基线对照中保住原匹配标注，新增匹配21个。完整官方处理顺序和多边形生产实现仍有差异，真实转写与语义归属仍需进一步核验。**Issue #20 已按范围迁移关闭，未完成验收由 Issue #26 承接**，不能因定位分数提高或夹具通过而宣称完整验收。
