# #27 数学内容展示与校验修复

2026-10-02，用户明确要求根据前轮诊断完善 Markdown 渲染流程与条件机制。审查基点为 `093cd6ffd7975351a6d4231daf60f916030a5738`；沿用既有公共 CLI 作业、DocumentIR 和生产 JSON 重新导出边界。

## 问题与结果

原解析器拒绝 `\rightleftharpoons` 等常用数学命令，并把 `$z=$` 这类试卷待填写表达式当成不完整公式。正文中的任一公式失败会使整块 `partial`，Markdown 只展示原图和提示，即使 Ovis 已正常完成并返回可用文字。

现在，正常完成的反应式、试卷留空等号、常用角度/集合/比较/逻辑符号、向量、几何点名及带中文或英文标签的箭头可以保留为正常内容。正文原文不改写，独立公式只沿用既有数学包装规范化；首次 Markdown 与 JSON 重新导出一致。

## 条件机制

- 视觉有效、生成正常结束、空输出、截断和重复仍由原输出检查判定；允许等号留空不绕过这些检查。
- 补齐历史失败文本中出现的数学命令，保持未知命令拒绝。命令来源参考 [KaTeX 支持表](https://katex.org/docs/supported.html)，不声明支持完整 TeX。
- `\overrightarrow` 必须有非空参数。`\xrightarrow/\xleftarrow` 支持必需的上方标签及可选的下方标签；中文/英文说明仅在标签参数范围内允许，参数中的公式结构继续检查。
- 三至四字母的全大写点名如 `ABC/ABCD` 可用于几何表达式；普通长说明仍不当作独立数学表达式。
- `$z=$` 和 `$$z=$$` 在正常完成时可展示；相同内容在 token 截断时仍为不完整输出。未闭合、参数缺失、未知命令和损坏结构仍显示原图与原因。
- 四类内容、三类 Ovis 任务、识别前阅读顺序、父子唯一归属及图片资源路径继续沿用现有计划与导出入口，未增加按细分标签执行的任务。

生产代码修改集中在 `src/core.cpp` 的公式校验。Markdown 渲染器与重新导出器沿用既有公共链路，合格输出现在到达其正常内容分支。Schema、原质量门槛及历史分数没有修改；重新导出历史 JSON 保留原状态，不自动晋升为成功。

## 验证

先在公共 CLI 写失败回归，再分别修复反应箭头、留空等号、常用符号、带文字标签的箭头和几何点名。每个阶段先观察真实 `partial / invalid_inline_formula_syntax` 失败，再验证正常展示。新增 `tests/issue27_rendering.py` 含 20 个正反例，核验原始输出、类型、状态、Schema、实际图片资源及首次/重新导出的 Markdown 字节一致。

```bash
cmake --build build/issue27 -j2
ctest --test-dir build/issue27 -R '^(issue27_rendering|issue27_formula|printed_formula_integration)$' --output-on-failure
ctest --test-dir build/issue27 --output-on-failure
.scratch/issue27-test-env/bin/python -m unittest discover -s tests -p 'test_*.py' -v
python3 -m compileall -q scripts tests
```

当前 Linux/MNN 构建通过；针对性回归 3/3、完整 CTest 27/27（44.74 秒）、Python unittest 37/37（55.999 秒）通过，Python 编译检查和差异空白检查通过。使用项目既有的 Python 测试环境，未新增运行时依赖。

常用 `build/linux-current` 入口也已重新编译；同一 20 个公共回归使用其生产 CLI 重新导出，全部通过。双轴审查 Standards 0 项、Spec 0 项，详见 [审查记录](rendering-review.md)。

### 历史文本重放

对现有 38 作业/39 页中 79 个公式校验失败块，固定历史 `raw_output` 和保存的生成文字，经当前公共作业入口及重新导出器重放：

| 类型 | 原失败数 | 新规则可展示 | 仍待核验 |
| --- | ---: | ---: | ---: |
| 正文 | 71 | 60 | 11 |
| 独立公式 | 8 | 1 | 7 |
| 合计 | 79 | 61 | 18 |

全部重放保持原始输出，Schema、资源及 Markdown 重新导出核验通过。逐块原始输出 SHA、来源文档 SHA、结果与受测工件 SHA 见 [重放记录](rendering-replay.json)。诊断脚本和本轮完整测试日志保存在本机 `.scratch/issue27-rendering-*`；这不是模型新推理，也不是 #27 原九项质量门槛重新验收。当前工作区已有其他未提交修改，本轮提交只纳入本次渲染校验、回归及说明。

18 个剩余块包括混合端点区间、数学范围内的说明文字、长变量下标、化学式及真正损坏的分隔符等，仍保留原图与原因。未将全部历史失败直接改成成功，也不由重放结论声明模型精度已经达标。#27 保持开放。
