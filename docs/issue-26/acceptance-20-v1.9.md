# Issue #26：DocumentIR 1.9 最终20页结构验收

2026-10-01，**最新1.9候选已通过收窄后的完整结构验收**。生产公共 CLI 重新识别完整20页，20/20 exit=0，逐页执行前后候选哈希一致。25/25 CTest通过。验收后实现提交为 [4b1cf08](https://github.com/bestkojima/docprase/commit/4b1cf084de80e188a67eaf254d7c77413e11b556)；逐文件校验该提交与实际运行的冻结源码完全一致。

Ovis 只有 text / formula / table 三种区域识别。其他文字标签先集中映射；image 保存资源并由 Markdown 引用，不调用 Ovis。后续其他 label 可以按明确策略 skip，规则已写入 [标签处理契约](label-policy.md) 与 #28/#29/#27。#26没有额外加入新的水印/装饰 skip 策略。

## 最终结果

- 768个选中 LayoutBlock，504个区域/资源坐标记录；264个子块由父区域唯一输出。原候选、来源、归属、裁图和资源与冻结参照及上一轮1.8一致。
- 实际识别任务451个：text 430、formula 8、table 13；共452条生成尝试。53个 image资源的 Ovis生成尝试为0，不进入实际识别顺序。
- 8个横排图片组、36个图注绑定。相对1.8新增 odb-07 人物说明与 odb-08“第8题”两处关联；20页 OCR内容、OCR状态及阅读顺序均与1.8一致。
- 共同可评 GT顺序对3743个，原基线正确3457对，当前3596对；保留既有139对修复，新增错序0，仍有147对原有错序。相对已验收1.8没有新增错序，也没有新的顺序修复。
- 20页实际调度均与识别前计划一致；图片地址存在，来源唯一，父裁图包含子块。首次 Markdown与生产重新导出逐字节一致，JSON及资源保持等价。
- 作业状态：7页 ok、13页 partial。块状态：456个 ok、48个 partial；其中正常图片资源53个，实际 OCR结果仍为403个 ok、48个校验 partial。

资源状态修正消除了 odb-07/08/13/15/18 的假 partial，没有改变模型文字或隐藏校验失败。48个校验回退仍为42处行内公式语法、4处独立公式语法、2处表格结构问题。整体内容与质量验收继续由 #27/#24承接。

## 重点页与可查看输出

| 页面 | 核验结果 | 最新 PNG |
|---|---|---|
| odb-03 | 第17/18题 A→B→C→D，八个真实标签绑定；水印c34未混入组 | [整页](acceptance-1.9-png/odb-03/04-reading-order.png)、[17题](acceptance-1.9-png/odb-03/q17-comparison.png)、[18题](acceptance-1.9-png/odb-03/q18-comparison.png) |
| odb-07 | vision_footnote映射为text，斐波那契说明绑定图片；图片资源不识别 | [PNG](acceptance-1.9-png/odb-07/04-reading-order.png) |
| odb-08 | 普通text“第8题”绑定单图；第11题三行及12个标签保持 | [PNG](acceptance-1.9-png/odb-08/04-reading-order.png) |
| odb-13 | 两组、四个标签及双栏关系保持，资源状态正常 | [PNG](acceptance-1.9-png/odb-13/04-reading-order.png) |
| odb-11 | 完整原页5556×8175，输入/裁图/资源哈希通过 | [PNG](acceptance-1.9-png/odb-11/04-reading-order.png) |
| odb-17 | 完整原页5556×8125，输入/裁图/资源哈希通过 | [PNG](acceptance-1.9-png/odb-17/04-reading-order.png) |

最新完整运行的 [odb-07 Markdown](/home/dr/project/docprase/output/issue-26/acceptance-20-v1.9/odb-07/review.md)、[odb-08 Markdown](/home/dr/project/docprase/output/issue-26/acceptance-20-v1.9/odb-08/review.md)使用绝对图片地址方便本机查看；正式 `job/document.md`保留相对资源地址。全部正文与正式输出一致。

## 来源与复现

推理时基点仍为 `d5cd4beeb6b2cee17ab907b08fa4436718206edd`，工作区未提交；实际候选按逐文件SHA冻结。验收后提交的源码逐文件验证一致，见 [提交绑定凭据](evidence/acceptance-20-v1.9-submission.json)。没有把基点提交或上一轮1.8报告冒充本轮已提交源码与成绩。

固定真实MNN/Ovis、CPU单线程、SmartResize Lanczos/0.3；模型为逐页前后哈希检查的硬链接快照，项目库实际加载路径由ldd核验。20页推理作业累计1597.95秒。PNG由本轮真实产物重画，最长边2048，超大页保留完整预览，JSON坐标保持原尺寸。

- [逐页结构、原基线及1.8对照](evidence/acceptance-20-v1.9.json)
- [冻结代码/配置/数据/运行时](evidence/acceptance-20-v1.9-freeze.json)
- [20页命令完成记录](evidence/acceptance-20-v1.9-run-summary.json)
- [核验日志](evidence/acceptance-20-v1.9.log)、[25项CTest日志](evidence/region-mapping-ctest.log)
- [102张PNG来源与哈希](evidence/acceptance-20-v1.9-png-manifest.json)、[提交的重点PNG索引](acceptance-1.9-png/index.json)

完整作业与所有张量、mask、裁图在 `/home/dr/project/docprase/output/issue-26/acceptance-20-v1.9`；全部102张中间量PNG/ZIP在 `/home/dr/project/docprase/output/issue-26/acceptance-20-v1.9-png`。Git只提交报告、哈希和重点图。上一轮 [1.8报告](acceptance-20.md)与历史重放分别保留。

```sh
python3 scripts/issue26_run.py \
  --cli /tmp/docprase-issue26-build/dococr_cli \
  --config /home/dr/project/docprase/output/issue-26/region-mapping-07-08/.runtime/config.json \
  --data /home/dr/project/docprase/output/omnidocbench/selected-20 \
  --out /新的空输出目录

python3 scripts/issue26_acceptance.py \
  --run /home/dr/project/docprase/output/issue-26/acceptance-20-v1.9 \
  --before /home/dr/project/docprase/output/issue-26/structure-final \
  --previous /home/dr/project/docprase/output/issue-26/acceptance-20 \
  --data /home/dr/project/docprase/output/omnidocbench/selected-20
```

## 逐页记录

| 页面 | 候选 | Region/资源 | Ovis任务 | 图片 | 图组 | 图注 | 状态 |
|---|---:|---:|---:|---:|---:|---:|---|
| odb-01 | 68 | 44 | 43 | 1 | 0 | 0 | partial |
| odb-02 | 101 | 48 | 48 | 0 | 0 | 0 | partial |
| odb-03 | 37 | 30 | 21 | 9 | 2 | 8 | partial |
| odb-04 | 92 | 42 | 35 | 7 | 0 | 4 | partial |
| odb-05 | 44 | 14 | 14 | 0 | 0 | 0 | partial |
| odb-06 | 71 | 34 | 32 | 2 | 1 | 2 | partial |
| odb-07 | 11 | 11 | 9 | 2 | 0 | 1 | ok |
| odb-08 | 44 | 44 | 27 | 17 | 3 | 13 | ok |
| odb-09 | 10 | 10 | 9 | 1 | 0 | 0 | partial |
| odb-10 | 23 | 11 | 11 | 0 | 0 | 0 | partial |
| odb-11 | 12 | 8 | 8 | 0 | 0 | 0 | partial |
| odb-12 | 20 | 15 | 12 | 3 | 0 | 0 | partial |
| odb-13 | 16 | 16 | 12 | 4 | 2 | 4 | ok |
| odb-14 | 13 | 9 | 9 | 0 | 0 | 0 | partial |
| odb-15 | 18 | 18 | 17 | 1 | 0 | 1 | ok |
| odb-16 | 19 | 14 | 13 | 1 | 0 | 1 | partial |
| odb-17 | 55 | 22 | 20 | 2 | 0 | 0 | partial |
| odb-18 | 24 | 24 | 21 | 3 | 0 | 2 | ok |
| odb-19 | 74 | 74 | 74 | 0 | 0 | 0 | ok |
| odb-20 | 16 | 16 | 16 | 0 | 0 | 0 | ok |
