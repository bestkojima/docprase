# Issue #28：官方完整后处理核验

核验日期：2026-10-03。规格：[Issue #28](https://github.com/bestkojima/docprase/issues/28)。

本次交付可重复执行的官方参照核验工具、公共作业反例和真实整页证据，**不修改生产候选筛选、内容归属或识别配置**。已核验的差异不等于本地与官方完全等价，也不等于全量识别质量通过。没有证据的结论仍保留“未通过核验”。

## 冻结来源与本次纠正

- PaddleX `release/3.7`，commit `ffb64904d23708863ff5b8da312a5cbd52a7f462`。完整源文件 URL 和 SHA-256 在 [reference-sources.json](evidence/reference-sources.json)，每次执行先校验全部文件；缓存被修改时直接失败。
- 配置为 [PaddleOCR-VL-1.5.yaml 快照](evidence/pipeline.yaml.gz)。模型标签另冻结于 PaddlePaddle/PP-DocLayoutV3 revision `241f8bdfc77a7c7bee915a5057aaee58c235a8d3` 的 [inference.yml](evidence/inference.yml)，**这个模型元数据 revision 不属于 PaddleX 代码 commit**。
- 旧 `scripts/issue20_compare.py` 使用 HF 标签别名：5/15 都叫 `formula`，9/13 分别叫 `footer/header`，23 叫 `text`。官方原始名称分别为 `display_formula/inline_formula`、`footer_image/header_image`、`vertical_text`。官方 `labels.index("formula")` 在原生标签表下得到 `None`，而旧参照得到 5；外层专门针对 `inline_formula` 的规则在旧参照下也不会触发。因此 #20 的历史分数不能作为本次原生官方参照的成绩；本次不改写历史结果。
- VL 管线显式以 `filter_overlap_boxes=False` 调用模型，在模型生成 `order` 后，调用 `paddleocr_vl/uilts.py::filter_overlap_boxes`。独立 Layout 模型默认路径则先执行 `layout_analysis/processors.py::filter_boxes`，再更新 `order`。两者都只过滤一次，但前者会保留编号间隙。工具同时输出 `model_output / pipeline_output / standalone_output`。

## 顺序、参数与逐项结论

全部类别、参数和来源映射见 [parameters.json](evidence/parameters.json)。下表“通过”表示本次有执行证据核实该环节，不表示所有输入都与本地一致。

| 环节 | 冻结官方行为 | 本地行为与结论 |
|---|---|---|
| 模型图内处理 | 本次重新转储固定 MNN 制品的 2655 个算子；已有框解码/尺度恢复、sigmoid/TopK、mask 二值化和 rank 生成 | **通过静态核验**：不再叠加 sigmoid、框解码或 mask 阈值化。完整图见 `evidence/model-graph.txt.gz`；未见专用 NMS 算子。原 Paddle 与 MNN 数值等价仍未通过核验 |
| 几何与分数 | 首先 `np.round` 框，再用严格 `score > threshold`；VL 默认 0.3 | **已证实差异**：本地保留浮点框，`>=`，缺省 0.5、示例 0.3；先验证退化/出界框。`score_boundary/subpixel_width/page_boundary` 覆盖边界 |
| NMS | 分数降序、含端点 `+1` 面积，同类 `>=0.6`，跨类 `>=0.98` | **通过规则核验**：本地同阈值、浮点几何；舍入和同分排序可能改变临界结果，不能宣称全输入等价。沿用 #19 阈值反例与原回归 |
| 大图片 | NMS 后多候选才处理 ID 14；裁到页内后面积比例横页 `>0.82`，其余 `>0.93`；若清空则恢复 | **通过规则核验**；本地面积按 floor/ceil 裁图框，官方按已舍入框。`large_page_image` 与 #19 反例覆盖 |
| 包含 | 交集/被包含框面积 `>=0.9`；3/5/6/15/17 为 large，其余 union；union 不合并相邻正文 | **已证实差异**：原生官方无 `formula` 名称保护；本地保护 ID 5。`display_formula_in_chart` 官方删除公式、本地保留。`union_adjacent` 保留两个正文框 |
| rank/mask | 按第七列 rank 排序，同步筛选/排序 mask；随后去掉 rank 列 | **通过**：逐阶段比较原始 mask 行字节哈希；原始 candidate ID、rank 和 mask 行保留，非按 class/score 猜来源 |
| mask 几何 | rect 不使用 mask；auto 执行 mask 裁切、最近邻放大、轮廓、polygon/quad/rect 判断 | **已证实差异**：本地保留 mask 资源，识别裁图始终为原图矩形。官方 epsilon、quad 判据等见参数文件；不把矩形裁图宣称为多边形等价 |
| 扩框 | mask 几何后执行宽高倍率，默认 `[1,1]` 无扩框 | **通过**：默认及非默认 `[1.5,2]` 边界测试；本地父区域 `owned_content_union` 是内容归属后的裁图扩展，与官方 unclip/union 都不同 |
| 结构化/边界 | 扩框后转成 label/score/coordinate，裁到页界并舍弃无效框；polygon 作为独立几何保留 | **已证实差异**：本地 floor 左上、ceil 右下保护原图像素，不把坐标舍入误差当精度改进 |
| 外层过滤 | reference 删除；宽或高 `<6` 删除；inline_formula 重叠 `>0.5` 删除；一般小框覆盖 `>0.7`，auto 加 polygon 判定；混合 image/table/seal/chart 有例外 | **已证实差异**：本地保留短框、合法注释和可归属的表格/正文子块，并有重叠文字行保护。官方按 rank 后数组遍历，本地按原候选顺序遍历；之后才做阅读顺序规划 |
| 顺序索引/阅读顺序 | 独立模型过滤后编号；VL 在外层过滤前编号；部分 label 的 order 为 null | **已证实差异**：本地阅读顺序由识别前页面计划统一确定，模型 rank 冲突时有几何回退。不能把模型输出行号、官方 order 和本地阅读顺序混称同一个字段 |

`StageTrace` 使用 Python 行事件只读观察冻结 `apply` 的阶段局部数组，不替换后处理算法。官方函数和类从校验过的 AST 装载，只移除依赖/计时装饰器及包导入；保留 `SKIP_ORDER_LABELS`。包含、NMS、mask、扩框、结构化和两种外层过滤均执行官方函数。若候选完整行也重复、无法唯一追溯，工具明确报错，禁止按相同分数猜测来源。

`removed_at` 表示删除阶段；`removal_reason` 进一步区分 `reference_label / short_box / inline_formula_overlap / smaller_overlapping_box`。`OuterTrace` 观察官方实际执行的删除分支，并记录 `related_candidate_id`，不通过另一套重叠算法猜测原因。

## 三类识别、资源与 skip

25 个原始类别统一映射为 17 个 text、2 个 formula、1 个 table、5 个 image；本地原始别名与官方名称分别保存。image 包括 class 3/9/13/14/20（chart、footer_image、header_image、image、seal），不调用 Ovis。旧说明中“9/13 为 text、image 只有 3 类”的表格已落后于当前源码；本次以公共作业 `all_labels` 的输出和 `src/layout_region_policy.cpp` 为依据，未覆盖修改已有文档。

- 独立公式 5/15 → formula；整表 21 → table；标题、正文、图注、页眉页脚文字、编号等 → text。
- 已确认归属的公式/表格文字仍保留来源 LayoutBlock，由父 Region 一次输出；受控整表和正文父子作业均检查 Region 与 `content_owned_by`。
- class 18 当前生产仍显式 `outer_reference` 删除；19 `reference_content` 保留为 text。**未证明所有真实 reference 标签均不含有效正文**，这一现存策略的内容安全仍未通过核验，不能借官方同样删除就视为合理或新增更多 skip。
- 未知 class 25 受控用例保留为 unknown、保存来源，不在官方 25 类表上强行运行。资源保存、内容归属、显式筛除与 Ovis 成败分别记录。受控 fixture 的转写是合成占位，不用于证明模型识别正确。

## 证据与质量边界

本轮 21 张真实整页共保留本地 787 个候选，官方 rect/auto 都保留 512 个，两个官方模式的候选集合一致；auto 有 95 个输出包含不同于矩形框的多边形。275 个差异中，271 个为 class 15 行内公式，另有 1 个 class 5 公式和 3 个正文候选。逐页 ID、框、阶段原因均已归档；这些是检测候选统计，不是 OCR 成功数或确认的内容损失数。

- 受控规格在 [issue28-cases.json](../../tests/fixtures/layout/issue28-cases.json)，加上动态构造的全部 25 类案例。覆盖合法父子、短标题、图注、部分包含、独立公式、原图边界、亚像素宽框、阈值边界、同类/跨类 NMS、相邻正文、reference 和未知类别。
- 真实作业通过 `dococr_cli --config … --input … --out …`，包含 #19 原 reference/0.5 整页以及冻结 20 页开发集的当前 smartresize_lanczos/0.3 配置。先在生产入口得到原始张量，再把**同一批候选/mask**送入官方矩形和 auto；不比较不同模型各自重新产生的候选。
- smartresize 的原始模型 mask 位于 800 画布；工具使用公共输出中的逆仿射把框回映原页，并将 mask 最近邻重采样到原页 200 网格，适配器名称为 `page_grid_nearest_200`。这是为了使官方函数接收同一页面坐标系，**不是官方原生预处理输出，也不证明全分辨率 polygon 精度等价**。#19 原样本使用 identity，不经过此适配。
- 历史原 reference 回归 `tests/issue19_real.py` 单独执行，核对固定候选 0..15、16 个块、16 个阅读顺序项以及 JSON/Markdown 重新导出。#26 结构与 #31 输入规划回归也执行。
- 本次不更改生产候选/归属，所以没有引入新的生产内容策略；20 页结果是后处理差异证据。没有重跑全量 Ovis，也没有把候选增减解释为实际文字损失、重复或跨栏混合。全量转写质量与冻结门槛仍归 #24/#27；受控行为、几何差异与人工确认的内容问题必须分别判断。
- **未通过核验**：所有真实 reference 删除的内容安全、Paddle/MNN 数值等价、smartresize polygon 全分辨率等价、全量 Ovis 内容零损失/零重复/零跨栏混合。不得用本报告替代上述验收。
- 本轮真实页中未找到宽/高 `<6` 的高分短标题，短标题保护只有受控反例；其真实反例覆盖仍为未通过。真实整页证据不能用来替代尚未出现的边界场景。

逐页数量、来源哈希、控制案例裁图/归属、压缩的完整官方阶段轨迹和测试结果见 [证据摘要](evidence/summary.json)。完整运行产物留在 `output/issue-28/audit-3/`，归档文件都有哈希可核对。源码基点为 `ce0dbae`；运行时包括工作区已有的 `src/region_structure.cpp` 修改，具体源码和 MNN 动态库哈希见 [local-sources.json](evidence/local-sources.json)。这些既有修改不属于本次提交。

审查后把归属/路由断言纳入 runner，重新执行全部 17 组受控作业；随后对原 38 份公共作业产物校验原图输入张量、候选、mask、DocumentIR 和清单哈希，重放增强后的原因追踪。归档 `comparison.json.gz` 为这次重放版本，官方矩形/auto 输出与初次执行逐项一致。[初次记录](evidence/run-report.json) 与 [重放记录](evidence/review-replay-report.json) 分别保存，不把重放记作新模型推理。初次全套 32 CTest、42 Python 测试通过；审查修复并增加负例后，最终全套 46 Python 测试、6 个定向测试、38 份重放和编译检查通过。生产代码未变，没有重复运行 CTest。

## 复现

使用 Python 3.12 安装 [requirements.txt](requirements.txt)，构建启用 MNN 的 CLI，并准备仓库固定模型和 `output/omnidocbench/selected-20` 数据：

```bash
cmake -S . -B build -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON
cmake --build build -j 4
python scripts/issue28_audit.py --cli build/dococr_cli \
  --fixture-cli build/dococr_cli_fixture --out output/issue-28/new-run --real
python -m unittest discover -s tests -p 'test_issue28*.py'
python tests/issue19_real.py build/dococr_cli
ctest --test-dir build --output-on-failure
python scripts/issue28_replay.py --from-report output/issue-28/new-run/report.json \
  --out output/issue-28/new-replay
```

输出目录必须不存在，以免混合不同运行来源。去掉 `--real` 仅执行受控公共作业。默认参照缓存 `.scratch/issue28-reference`；不存在时从固定 URL 下载并校验。单独执行官方参照：

```bash
python scripts/issue28_reference.py --input case.json --out comparison.json
```

输入含 `size: [width,height]`、`rows: [[class,score,x0,y0,x1,y1,rank],…]`；可提供 `threshold`（默认 0.3）、`unclip`（默认 `[1,1]`）、`mask_rectangles`（200×200 网格，每行一个矩形；省略时空 mask）。CLI 的参考测试覆盖 rank/mask 过滤、编号间隙、同分不同候选、非默认扩框及边界裁剪。
