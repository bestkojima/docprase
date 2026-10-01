# Issue #26：收窄后的最终20页验收

这是上一轮 DocumentIR 1.8 候选的历史完整20页验收；最新1.9另行冻结运行与报告，不能复用本报告作为新候选成绩。
2026-10-01 完成最终一次冻结候选的完整20页生产识别；无历史转写复用。全部20个公共 CLI 命令 exit=0，运行期间源码、模型、项目动态库与配置哈希未变化。此报告验收识别前计划、横排选项、图注引用及等价导出；不声明全部语义关联、全部阅读顺序或 OCR 质量正确。

## 结构结果

- 768个选中候选、504个 Region；实际新推理的候选原框、rank、mask 行、过滤/扩框证据、Region 身份/来源/裁图与冻结候选参照一致，原页裁图 PNG 字节不变。所有来源唯一归属，父裁图覆盖已归属子块。
- 8个横排图片组、34个图注绑定。全部20页的实际处理顺序等于识别前计划，首次 Markdown 与生产重新导出逐字节一致；重新导出未改变 DocumentIR 或裁图资源。
- 沿用原一对一配对口径，双方相同候选归属的共同可评 GT 顺序对为3743个，正确数从3457增至3596；修正139对，新增错序0，身份变化导致的配对排除为0。仍有147对原有 GT 错序，本轮未宣称全页排序已完全正确。多 GT/单 Region 内容评测继续归 #27。
- CTest 24/24通过，包括受控分组、几何抖动、标签识别失败、候选争用、原图提示、计划篡改拒绝、兼容、PDF及作业控制。不确定图注在受控公共作业中验证；这批真实20页未出现被当前规划器标记为 ambiguous 的候选，不能把这一点当作所有图注语义都已确认。

## 真实重点页

| 页面 | 核验结果 | 新运行标注图 |
|---|---|---|
| odb-03 | 第17/18题均按 A→B→C→D 位置排列，八个原图标签绑定正确。c34 水印仍单独保留，未混入组选项；没有编造标签或删除图片。 | [整页](acceptance-png/odb-03/04-reading-order.png)、[第17题](acceptance-png/odb-03/q17-comparison.png)、[第18题](acceptance-png/odb-03/q18-comparison.png) |
| odb-07 | 历史0.2阈值的生日图注重复反例，在本轮0.3下未复现；b0007 独立输出一次，出生年份片段在 Markdown 只出现一次。未新增图片组/显式图注绑定，不宣称全部人物图文关系已解析。 | [PNG](acceptance-png/odb-07/04-reading-order.png) |
| odb-08 | 第11题三行各自成组，(1)/(2)/(3)与 A/B/C 为12个实际 OCR 标签；组内图片从左到右，每图紧接其标签。第8/9题的既有复杂图文排序仍保留，不能把本轮结果概括为整页无错序。 | [PNG](acceptance-png/odb-08/04-reading-order.png) |
| odb-13 | 两组各两张图片，甲/乙与(a)/(b)四个实际标签绑定正确；双栏分组与来源保留，共同 GT 对修正2对，无新增错序。 | [PNG](acceptance-png/odb-13/04-reading-order.png) |
| odb-11 | 使用完整原图 5556×8175；输入哈希、裁图和全部资源核验通过。 | [整页预览](acceptance-png/odb-11/04-reading-order.png) |
| odb-17 | 使用完整原图 5556×8125；输入哈希、裁图和全部资源核验通过。 | [整页预览](acceptance-png/odb-17/04-reading-order.png) |

## 状态与实际限制

作业状态：18页 partial、2页 ok；块状态：403个 ok、48个 partial、53个图片 skipped。48个回退分别为42处 `invalid_inline_formula_syntax`、4处 `invalid_formula_syntax`、2处 `invalid_table_structure`，原输出和原图证据仍保存。图片跳过识别是既有路径；没有把 skipped 当作文字识别成功。

公式/异常校验由 #21、重试由 #22 跟踪；全文内容对齐及产品质量由 #27/#24 继续验收。本批未重新计算或放宽严格 CER 门槛，未改动 #25 的豁免结案或未达标实测。官方完整后处理审计为 #28，水印/装饰误检处理为 #29。

## 来源、资源与复现

Git 基点是 `d5cd4beeb6b2cee17ab907b08fa4436718206edd`，实现尚未提交；实际运行源码以 [freeze.json](evidence/acceptance-20-freeze.json) 的逐文件 SHA-256为准，不能把基点提交当作本轮已提交代码。模型采用逐页前后哈希检查的硬链接快照，项目库由 ldd 核实实际加载快照目录。该冻结记录的 SHA-256为 `cc68b97c0341ce3cd98b27a11c08943dda484313803af5718a0bae1c45e1e628`。

- [逐页结构、GT 对与产物哈希](evidence/acceptance-20.json)
- [20页命令完成记录](evidence/acceptance-20-run-summary.json)
- [核验日志](evidence/acceptance-20.log)、[PNG生成日志](evidence/acceptance-20-annotations.log)、[最新CTest日志](evidence/scope-ctest.log)
- [102张PNG的来源与哈希](evidence/acceptance-20-png-manifest.json)、[仓库内重点PNG索引](acceptance-png/index.json)

完整真实作业及全部张量/mask/裁图资源在 `/home/dr/project/docprase/output/issue-26/acceptance-20`；102张中间量 PNG、每页凭据和完整 ZIP 在 `/home/dr/project/docprase/output/issue-26/acceptance-20-png`。仓库只复制8张重点图，索引标明各原作业路径。PNG 最长边2048，超大页预览保留整页而非裁切；原输入与 JSON 坐标保持原尺寸。

```sh
python3 scripts/issue26_run.py \
  --cli /tmp/docprase-issue26-build/dococr_cli \
  --config /home/dr/project/docprase/output/issue-26/real-final/.runtime/config.json \
  --data /home/dr/project/docprase/output/omnidocbench/selected-20 \
  --out /新的空输出路径

/usr/bin/python3 scripts/issue26_acceptance.py \
  --run /home/dr/project/docprase/output/issue-26/acceptance-20 \
  --before /home/dr/project/docprase/output/issue-26/structure-final \
  --data /home/dr/project/docprase/output/omnidocbench/selected-20
```

核验可重复执行，使用新重新导出目录保留已有产物，实际命令记在报告中；不会再次运行模型。历史20页张量/转写重放与本批新推理分别保留，[structure-replay-summary.json](evidence/structure-replay-summary.json)只用于隔离结构修改，不能称为新模型成绩。

## 逐页记录

| 页面 | 候选 | Region | 图片组 | 图注 | GT 对 | 修正 | 新增错序 | 作业状态 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| odb-01 | 68 | 44 | 0 | 0 | 171 | 0 | 0 | partial |
| odb-02 | 101 | 48 | 0 | 0 | 231 | 0 | 0 | partial |
| odb-03 | 37 | 30 | 2 | 8 | 190 | 12 | 0 | partial |
| odb-04 | 92 | 42 | 0 | 4 | 231 | 0 | 0 | partial |
| odb-05 | 44 | 14 | 0 | 0 | 91 | 0 | 0 | partial |
| odb-06 | 71 | 34 | 1 | 2 | 66 | 2 | 0 | partial |
| odb-07 | 11 | 11 | 0 | 0 | 45 | 0 | 0 | partial |
| odb-08 | 44 | 44 | 3 | 12 | 946 | 123 | 0 | partial |
| odb-09 | 10 | 10 | 0 | 0 | 36 | 0 | 0 | partial |
| odb-10 | 23 | 11 | 0 | 0 | 55 | 0 | 0 | partial |
| odb-11 | 12 | 8 | 0 | 0 | 6 | 0 | 0 | partial |
| odb-12 | 20 | 15 | 0 | 0 | 78 | 0 | 0 | partial |
| odb-13 | 16 | 16 | 2 | 4 | 105 | 2 | 0 | partial |
| odb-14 | 13 | 9 | 0 | 0 | 28 | 0 | 0 | partial |
| odb-15 | 18 | 18 | 0 | 1 | 91 | 0 | 0 | partial |
| odb-16 | 19 | 14 | 0 | 1 | 55 | 0 | 0 | partial |
| odb-17 | 55 | 22 | 0 | 0 | 105 | 0 | 0 | partial |
| odb-18 | 24 | 24 | 0 | 2 | 190 | 0 | 0 | partial |
| odb-19 | 74 | 74 | 0 | 0 | 903 | 0 | 0 | ok |
| odb-20 | 16 | 16 | 0 | 0 | 120 | 0 | 0 | ok |
