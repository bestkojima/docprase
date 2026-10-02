# #27 Markdown 数学转义与实际排版修复

2026-10-02。用户要求：正文沿用转义，数学区域保留 LaTeX 的 `<`、`>`、矩阵 `&`；首次导出和重新导出使用相同规则，默认输出 `$...$` / `$$...$$`；补真实 KaTeX 排版测试；仅重新导出刚测试的两页，不重新运行 OCR。本轮基点为 `b14c103221dae9690c5d3cff9c0dc36920177a0b`。

## 行为

修改集中在公共 `render_markdown_block`：正文的 HTML、图片及链接保护沿用原规则；通过既有公式校验的完整数学区域保留 LaTeX，不做 HTML 实体转义。独立公式沿用行内/显示属性，正文中的旧式 `\(...\)` 与 `\[...\]` 统一为美元符号分隔符，去掉公式首尾空白。公式内的物理换行统一为空格，避免行内公式跨行或空行分段使 Markdown 解析丢失数学区域；LaTeX 命令、矩阵列分隔符及显式 `\\` 换行保持原样。行内和显示公式紧贴数字或前邻反斜杠时补必要空格，使数学插件能够识别分隔符。

首次识别和 JSON 重新导出本来就调用同一渲染函数，本次继续复用。公式边界识别也复用现有校验，未闭合、未知语法、转义美元符号和普通金额不能任意绕过正文保护。原始输出、DocumentIR、阅读顺序、内容归属和已有失败状态保持原样。未新增生产导出或数学引擎配置。

## 回归

先复现正文不等号错误转义，再复现独立矩阵的列分隔符错误转义，分别修复。新增公共 CLI 回归 19 组，覆盖两页集合题、行内/显示数学、正文中的矩阵、分段函数、反应箭头、待填写等号、旧式分隔符、首尾空白及公式相邻正文的 HTML/链接保护。审查另发现编号紧贴旧式公式时，转换后的美元分隔符会被数学插件当作金额；复现后为行内和显示公式补分隔空格。含 LF / CRLF 及空行的公式也加入回归，正文内与独立公式共用物理换行规范化。验证公式中的 `\text{<img ...>}` 按文本排版，不成为活动图片。排版测试同时检查行内/显示模式，防止显示公式退成行内公式。

真实 KaTeX 0.19.0 测试处理的是导出的 Markdown，不是单独挑选的原始公式。它核验实际 LaTeX 输入、排版错误、HTML/MathML 和矩阵/分段函数的两行结构；相关 Node.js 依赖锁定在 `tests/math-rendering/package-lock.json`，只用于测试和预览。启用方式见[测试说明](../../tests/math-rendering/README.md)。

历史 1.1 的 JSON 和原 Markdown 固定样本没有修改；回归明确预期新版重新导出只恢复其中的 `\beta>0`。此前误把 `x&gt;0` 当作正确结果的断言已改为原始不等号。

构建、完整 CTest **29/29**、Python unittest **37/37**、Python 编译、Node.js 语法及差异空白检查通过。常用 `build/linux-current` 入口已重新构建。完整日志保存在本机 `.scratch/issue27-math-ctest-final.log` 和 `.scratch/issue27-math-unittest-final.log`。[两轴复审](math-export-review.md)均为 0 项未解决问题。

## 两页重新导出与浏览器验证

从刚完成真实识别的 `output/issue27-two-page-20261002T041025Z/*/job/document.json` 重新导出到 `output/issue27-math-export-20261002T044351Z/`，没有再次运行模型。原 JSON 逐字节一致，所有引用资源 SHA-256 一致。

| 页面 | 当前 Markdown 中实际排版的公式 | KaTeX 错误 | 原状态 |
|---|---:|---:|---|
| 黄冈数学卷 | 72 | 0 | partial：47 块 ok、1 块 partial |
| 成都数学卷 | 48 | 0 | ok：44 块 ok |

Chromium 153.0.8010.12 中再次验证：120 条公式均有可见的排版区域，LaTeX 输入无错误 HTML 实体，KaTeX 字体及图片资源加载正常，没有失败请求。修改前后公式对比页含 16 条正常排版的公式、矩阵/分段函数的 4 行 MathML；修改前错误转义产生的 8 处排版错误保留用于比较。

- [对比预览与截图](../../output/issue27-math-export-20261002T044351Z/index.html)
- [黄冈 Markdown](../../output/issue27-math-export-20261002T044351Z/odb-02/document.md)、[成都 Markdown](../../output/issue27-math-export-20261002T044351Z/odb-01/document.md)
- [重新导出证据](../../output/issue27-math-export-20261002T044351Z/reexport-verification.json)、[KaTeX 证据](../../output/issue27-math-export-20261002T044351Z/katex-verification.json)、[浏览器证据](../../output/issue27-math-export-20261002T044351Z/browser-verification.json)
- [最终构建再次导出核验](../../output/issue27-math-export-20261002T044351Z/final-runtime-verification.json)：最终修复后的入口重新导出与预览 MD 逐字节一致，原 JSON、资源不变。

这些预览、图片、资源和完整日志为本机产物，`output/` 不随源码提交。Markdown 文件需要支持数学分隔符的查看器；对比预览已保存 HTML、CSS 与字体，可离线打开。

黄冈第 9 题的半开区间仍受既有语法校验影响而回退；本次重新导出不改变其状态。边缘文字和转写错误仍保留，本轮不代表 #27 全量 OCR 精度达标。
