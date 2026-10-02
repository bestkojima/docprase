# Markdown 数学排版回归

公共测试边界为 CLI 首次导出、生产 JSON 重新导出，以及数学感知 Markdown 解析后交给真实 KaTeX 的排版结果。测试不依赖模型、不访问网络，也不把 `status=ok` 当作排版成功。

普通 CTest 中的 `markdown_math_integration` 核验正文转义、LaTeX 命令与运算符、默认美元符号分隔符、公式空白规范化、Schema、原始模型输出及首次/重新导出的一致性。`markdown_katex_integration` 再解析同一批导出的 Markdown，调用 KaTeX，检查完整 LaTeX 输入、行内/显示模式、HTML/MathML、矩阵/分段函数行数和排版异常。包含相邻数字、反斜杠及跨行公式，确保公式中的 HTML 样式文字仍作为数学文本排版。

从仓库根目录准备锁定的测试依赖并启用真实排版回归：

```bash
npm ci --prefix tests/math-rendering
cmake -S . -B build/issue27 -DDOCOCR_TEST_KATEX=ON
cmake --build build/issue27 -j2
ctest --test-dir build/issue27 -R '^markdown_.*integration$' --output-on-failure
```

`DOCOCR_TEST_KATEX` 只控制是否运行需要 Node.js 的附加测试；导出行为无需新配置，生产 CLI 无 Node.js 或 KaTeX 依赖。开启测试但缺少 Node.js 或 npm 依赖时明确失败，不跳过或伪造排版结果。

数学内容由 `markdown-it-texmath` 在 Markdown 解析阶段识别，随后交给 KaTeX；先用普通 Markdown 解析器处理 LaTeX 会损坏反斜杠和矩阵换行。预览采用 `trust=false`；KaTeX API 与分隔符配置参考 [官方 API](https://katex.org/docs/api.html)和[分隔符说明](https://katex.org/docs/autorender.html)。
