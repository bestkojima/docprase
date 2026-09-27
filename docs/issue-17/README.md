# #17：OmniDocBench 20 页整页开发基线

本集合只用于开发与评测，不是独立留出集，也不代表 OmniDocBench 全量榜单。源书身份尚有不确定性，不能从上游 `exam_paper` 标签推断每页都是完整试卷；具体实际内容见 [`../omnidocbench-20/README.md`](../omnidocbench-20/README.md)。

本次运行的逐页结果和失败摘要见 [results.md](results.md)，完整机器评分见 [report.json](report.json)。

## 冻结输入与运行

- 主集合：[`../omnidocbench-20/manifest.json`](../omnidocbench-20/manifest.json) 的固定 20 页；数学 12、物理 3、化学 2、生物/语文/英语各 1。数据 revision 为 `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec`，页面、子集标注和上游标注哈希由清单固定。原始图像和上游格式标注位于 `output/omnidocbench/selected-20/`，评分器在运行前逐页核对哈希和 `page_info`。
- 旧 7 页回归继续使用 [`../issue-15/samples.json`](../issue-15/samples.json) 的 `frozen-1` 清单；它与本 20 页集合分别统计。部分旧页已用于开发，不能作为严格留出成绩。
- 固定生产配置为 [`../../configs/printed-page.example.json`](../../configs/printed-page.example.json)：MNN CPU 1 线程、每区域 512 token，原始图像整页提交。版面包 revision `c67c1a858d5f6c855172d4cfdf931798dafa2edd`，识别包 revision `20f12e49d846941e67829a7a7c3645693e485942`，实际工件由配置及运行清单 SHA 锁定。每个新 CLI 进程经公共文档作业入口处理 1 页；`run-manifest.json` 保留实际模型工件 SHA、运行时、有效参数与停止原因。`report.json` 固定 CLI 与配置 SHA。无页面挑选和识别后调阈值。
- 可复现命令：`python3 scripts/issue17_baseline.py --cli build/linux-current/dococr_cli --out output/issue-17`。可用 `--score-only` 根据现存作业重新评分。`--only` 仅供调试，不得将其结果当作完整 20 页基线。脚本保留每页 `command.json`、`stdout.log`、`stderr.log`、`time.txt`、`job/document.json`、`job/document.md`、`job/run-manifest.json` 和资源。失败页同样进入报告；超时上限为每页 3600 秒。

## 冻结评分口径

评分直接读取原始 `OmniDocBench.json`。`ignore=true` 和 `abandon` 不进入内容分母；其余原始顶层标注按正文（`text_block`、标题、页眉页脚、页码、图表标题/脚注）、独立公式、表格、图片分别计数。正文中的嵌套 `equation_inline` 另计行内公式，不以 8 个独立公式替代其分母。原始标注不改写。

同类块按参考框覆盖率做全局贪心一对一匹配：正文、表格、图片至少 0.5，独立公式至少 0.8。未配对标注为漏块；额外输出区分与已有参考重叠的重复和完全未配对输出。合并/拆分导致无法一对一匹配时保守计为漏块或额外输出，逐项保留供复核，不把覆盖当内容正确。异常状态（`partial`、`failed`、`skipped`）即使有 raw 也计回退，不得用其文本得分。退出失败或没有 `document.json` 时，该页全部参考项计漏块，并计入失败页。

`quality_claim_blocked` 在文档状态异常、任何漏块/回退/重复/额外输出、内容不精确或顺序对遗漏/错误时为真；它只是防止报告把受控缺陷误说成全对，不是质量门槛或达标声明。

重复同时有两种记录：空间匹配后多出的版面块记 `duplicate_outputs`；区域 raw 中同一连续 16 字符片段出现至少三次，或单块出现至少五个数字 Markdown 标题，记 `raw_repetition_suspicions`。后者是待复核信号，可能包含合法重复内容，不直接当作确认错误；它仍阻止自动宣称全对，原始文本保留在 `document.json`。

正文只折叠连续 Unicode 空白并去首尾空白，逐参考块记录精确匹配、编辑距离和参考字符数；全集 CER 为总编辑距离／总参考字符数，漏块和回退用空串计删除。公式仅忽略空白和一次外层 `$$`，行内公式还须位于匹配正文块的有效内容中。表格解析参考 HTML 与产出结构，分别比较行列数、单元格坐标及跨度、单元格文字；上游 HTML 中未转义的小于号只在临时解析副本转义。图片只核对检测和正常状态，不声称内容识别正确。阅读顺序按上游 `order` 的有序参考块两两比较；全部参考对为分母，未匹配对单列，不计正确。上述口径是诊断基线，不是最终产品质量门槛。

## 标注争议

[`../omnidocbench-20/ERRATA.md`](../omnidocbench-20/ERRATA.md) 中 ODB-15 `anno_id=17` 的时间符号已对[原始页裁剪](evidence/odb-15-anno17-review.png)目检：图中为 `t=0`，上游标注为 `t=4`。核验记录和单字段替换见 [`errata.json`](errata.json)。脚本保留原始标注不变，报告同时给出上游 CER 与勘误 CER；勘误只更正该数字，不凭模型输出修改真值。
