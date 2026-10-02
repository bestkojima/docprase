# 密封线信息混入正文：诊断记录

日期：2026-10-02。用户已澄清，本轮重点是姓名、学号等密封线信息混入正文。

以下为修复前的复现、根因和处理建议，原始失败证据保持不变。用户随后选择按 Paddle 标签流水线实施；当前修复与回归见 [标签流水线](../../label-pipeline.md)。诊断脚本已增加旁注原文、源类别、状态和裁图保留断言；正式回归进入 CTest 的 `label_pipeline`。项目任务和正式规格仍以 GitHub Issues 为准。

## 已确认的错误

通过公共 CLI 重放两页真实页面的完整 Layout 张量、mask 和历史 Ovis 输出：`odb-01`、`odb-02` 各有“座位号”“考场号”“准考证号”“姓名”四项被当成正文标题。正文中的“考生务必将自己的姓名、准考证号填写在答题卡上”同时正常保留。

原图中，密封线位于 x=176～177，相关字段框均在其左侧。例如 `odb-01` 的姓名框为 `[105, 1932, 155, 2015]`。两页均为 3308×2339。原图、候选和识别区域没有把姓名与题干裁进同一块；原始输出也正确包含“姓名”。

## 可复现反馈

从仓库根目录执行，需已构建的 `build/linux-current/dococr_cli_fixture` 和 `dococr_cli`、Python/Pillow，以及 `output/issue-27/development` 的原始作业资源。归档到 `docs/issue-27` 的 JSON 不包含完整原始张量，不能单独替代这些资源。

```sh
python3 docs/diagnostics/seal-margin/replay.py --mode original
python3 docs/diagnostics/seal-margin/replay.py --mode minimal
python3 docs/diagnostics/seal-margin/replay.py --mode without-name
```

修复前已执行的结果：

| 命令模式 | 具体判定 | 退出码 |
| --- | --- | --- |
| `original` | 两页各泄漏四个密封线字段；保留正文填写说明 | 1 |
| `minimal` | 单一 `aside_text` 姓名块仍生成正文中的 `## 姓名`，约半秒，重复执行均失败 | 1 |
| `without-name` | 删除唯一候选后无字段泄漏 | 0 |

最小复现重放真实类别、分数、框及原始文字，空白图片只提供页面坐标空间，不代表重新进行了视觉识别。完整复现使用真实原图、原始检测张量及历史识别输出。两者均经过真实的核心规划、输出校验、序列化和 Markdown 导出路径；脚本还通过生产 CLI 重新导出并核对文件字节一致。各次执行的命令、输入来源及二进制哈希见输出目录的 `report.json`。

修复前断言针对单一 Markdown 正文流。当前脚本同时检查正文无泄漏和 JSON/裁图中旁注信息完整保留，防止删除结果导致假通过。修复前执行汇总见 [replay-evidence.json](replay-evidence.json)，修复后证据见 [fixed-replay-evidence.json](fixed-replay-evidence.json)。

## 假设及验证

| 排名 | 假设与预测 | 验证 |
| --- | --- | --- |
| 1 | 识别任务和正文用途混为一体；原类别即便正确为 `aside_text`，仍会进入正文 | 确认。`src/layout_region_policy.cpp` 将 class 2 配置为 `Text + Body`；class 22 也是 `Text + Body`。只改为 class 22，错误不变 |
| 2 | 缺少密封线区域的独立归属；位置变化不会产生区别于正文的角色 | 确认当前缺少此分区。将同一姓名块向正文移动 700 像素，两种位置都为 `text` 并进入页面阅读顺序；原位置没有独立的信息区展示 |
| 3 | 模型输出的 `##` 导致误当标题，删除符号即可解决 | 排除其作为完整解决方案。只把输出改成 `姓名`，仍泄漏到正文；`##` 是展示上的放大因素 |

探针命令：

```sh
python3 docs/diagnostics/seal-margin/replay.py --mode minimal --class-id 22
python3 docs/diagnostics/seal-margin/replay.py --mode minimal --move-to-body
python3 docs/diagnostics/seal-margin/replay.py --mode minimal --plain-name
```

`--move-to-body` 的 PASS 是正文位置的对照：允许同名文字出现在正文，不能把它解释为密封线修复通过。

具体链路：`layout_region_policy.cpp` 给出用途 → `core.cpp::arrange_structure` 使用该用途并安排页面顺序 → 初次 Markdown 导出遍历所有块 → `reexport.cpp::render_page_markdown` 同样遍历页面阅读顺序。原始 class ID 和 label 已保留在 LayoutBlock 中；缺少的是面向试卷的区域归属和对应导出行为。

## 诊断阶段的处理建议（历史方案）

1. **识别前确定侧边信息区。** 保留当前四类内容和三种 Ovis 任务；为 `aside_text` 增加独立的版面用途提示，再结合密封线位置、与正文的隔离、相邻竖排字段组等证据确定区域归属。单一类别、靠边位置或关键词都不足以直接排除内容。密封线断裂、页面旋转、右侧信息区等需要单独验证。
2. **按区域规划阅读顺序。** 正文区、试卷侧边信息区分别规划；仍保留全部块身份、原框、裁图和计划来源。姓名与题干分属两个区域，侧边信息不作为正文或选项组的锚点。该规划在识别前确定，不能在识别后凭文字重排来替代。
3. **识别任务继续使用 `text`。** 可确认的姓名、学号、准考证号等保存在侧边信息区；密封线提示文字也留在该区。识别后可补充字段名称及已识别值，空值或不明确值如实记录。教材旁注、侧栏说明仍可正常展示。DocLayout 的 `seal` 是印章资源类别，与试卷密封线概念不同。
4. **通过 DocumentIR 表达归属并统一导出。** 在新的 Schema 版本中加入明确的区域及角色引用，分别保存正文顺序和侧边区顺序；具体字段命名在实现规格中确定。完整 Markdown 将侧边信息集中放入单独的信息区；题目正文视图按明确的导出策略省略该区。初次导出与 JSON 重新导出复用同一规则，保留原始输出和原图资源。

两页的竖线像素测量见 [geometry-evidence.json](geometry-evidence.json)。这里的左侧搜索范围及阈值仅用于证明这两张原图存在清楚的分隔线，尚未构成通用生产检测器。

## 诊断阶段建议的验证边界

应首先让两页原始复现的八项字段停止混入正文，同时仍可从独立信息区及 JSON 追溯。验证正文填写说明、正文中出现的“姓名/学号”、教材旁注、短图注、选项标签不被错误移除，并覆盖左右密封线、旋转、虚线/断线和没有密封线的页面。完整导出与重新导出必须一致，识别状态及原始输出保持真实。

这两页来自已曝光的开发样例，不能作为未见文档验收。缺少新样例的证据不应转换为通用策略已通过。当前三类识别约束沿用 [#28](https://github.com/bestkojima/docprase/issues/28)，水印/装饰问题由 [#29](https://github.com/bestkojima/docprase/issues/29) 跟踪；密封线中的身份字段具有实际信息，应按归属处理。
