# #27 半开区间校验与保存输出重新校验

2026-10-02。承接用户确认的方案：按 LaTeX 结构校验，允许普通与可伸缩半开区间；保留真正的分组、环境、分隔符和生成完整性检查；补公共 CLI 与实际 KaTeX 回归；从保存输出重新校验第 9 题并导出两份文档，保留新旧判定。基点 `715fad782c19590edb151868b3a5d0485fd2e0ed`。

## 校验规则

普通圆括号和方括号是显示符号，不作为 LaTeX 分组栈。`[a,b]`、`(a,b)`、`[a,b)`、`(a,b]` 以及无穷端点均允许。`\left` / `\right` 允许不同端点符号，包括 `\left[-1,+\infty\right)` 和另一种开区间写法 `\left]a,b\right[`；仍须成对出现并处于同一花括号分组和环境。

`{}` 分组、公式包裹符、环境配对、必需参数及已有不支持的命令检查继续生效。生成完整性依据保存的完成状态、停止原因、视觉证据和异常输出判定；可排版的公式遇到 token 上限仍保持 `partial`。程序不进行数学纠错，普通括号的数学写法不合理不能直接等同 LaTeX 语法错误。旧测试中 `a+(b`、`([)]`、`({)}` 改为验证原样导出；真正损坏的花括号及环境另有反例。

首次识别与保存输出重新校验复用同一解析与正文数学校验函数，Markdown 导出规则沿用上一轮。

## 重新校验入口

普通 `--reexport` 保留 JSON 原字节与历史状态。需要应用当前公式规则时显式执行：

```bash
build/linux-current/dococr_cli --revalidate 原document.json --asset-root 原资源目录 --out 新输出目录
```

入口先按既有契约核验完整 DocumentIR。只重新判断已保存为 `invalid_inline_formula_syntax` / `invalid_formula_syntax` 的 `partial` 文本或公式；保存的完成与视觉判定通过、当前语法校验通过时才生成新 `ok` 判定。原始模型输出、采用尝试、视觉记录、ID、阅读顺序及资源保持原样。真正损坏的公式和截断内容继续回退；缺少可核验判定的历史内容不自动升级。

输出包含新版 `document.json` / `document.md`、原字节的 `previous-document.json` 和 `formula-revalidation.json`。判定记录包含前后状态、错误、内容、完整 assessment、原 JSON 与新 JSON SHA-256，执行类型为 `saved_output_no_ocr`。无状态变化时，新 JSON 保持原字节。本入口不加载模型、不重新识别、不覆写原目录；更新后的 JSON 再次普通导出仍遵守同一规则。

## 实际两页结果

产物保存在 `output/issue27-interval-fix-20261002T054037Z/`，来自上一轮的两份保存 JSON。

| 页面 | 变化 | 当前状态 |
|---|---|---|
| 黄冈卷 | 仅 b0022 / 第 9 题，从 `partial` / `invalid_inline_formula_syntax` 经重新校验变为 `ok` / `normal_completion` | 48 块 ok |
| 成都卷 | 无变化，新 JSON 原字节一致 | 44 块 ok |

两页的文字、原始输出、采用尝试、视觉记录和资源 SHA-256 均保持不变；原文件保持原字节。黄冈只改变第 9 题的状态、错误及 assessment 的 state / reason，并据块结果更新页和文档状态。再次普通导出的 JSON 与 Markdown 逐字节一致。

- [第 9 题前后预览](../../output/issue27-interval-fix-20261002T054037Z/index.html)
- [黄冈新版 MD](../../output/issue27-interval-fix-20261002T054037Z/odb-02/document.md)、[成都新版 MD](../../output/issue27-interval-fix-20261002T054037Z/odb-01/document.md)
- [黄冈新旧判定](../../output/issue27-interval-fix-20261002T054037Z/odb-02/formula-revalidation.json)、[两页核验](../../output/issue27-interval-fix-20261002T054037Z/revalidation-verification.json)

## 验证范围

公共 CLI → DocumentIR → 首次/重新导出 → 实际 KaTeX 的回归包含 25 组，新增普通区间、可伸缩半开区间、嵌套分式、另一种开区间及矩阵区间。公式损坏反例覆盖缺失花括号、缺失 right、额外 right、跨花括号/环境伸缩分隔符。重新校验另有 7 组公共 CLI 用例，验证旧误判恢复、真损坏与截断保留、无变化保持原字节、判定记录与再次导出。

完整 CTest **30/30**、Python unittest **37/37** 通过，日志位于 `.scratch/issue27-interval-ctest.log` 和 `.scratch/issue27-interval-unittest.log`。两页新版 Markdown 经 KaTeX 0.19.0 实际解析排版：黄冈 77 条、成都 48 条公式，0 个排版错误。Chromium 153.0.8010.12 中所有公式均可见，字体及图片加载正常、无失败请求；第 9 题修复前为原图回退，修复后 5 条公式正常排版。

- [KaTeX 核验](../../output/issue27-interval-fix-20261002T054037Z/katex-verification.json)
- [浏览器核验](../../output/issue27-interval-fix-20261002T054037Z/browser-verification.json)
- [两轴复审](interval-revalidation-review.md)：Standards 0 项，Spec 0 项。

本轮修复公式规则并恢复两页显示；单个坏公式的局部回退仍作为后续改进，#27 全量识别质量验收尚未完成。
