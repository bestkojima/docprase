# #27 VLM 后处理：公式内容、多段公式与表格数学

## 本轮范围与复现

用户要求先复现三项问题，再实施修复。本轮基点为 `782bd5e`，范围限于公式内容兼容性、独立公式区域多段内容，以及表格内数学排版。沿用 CLI 首次导出、生产 JSON 重新导出和实际 KaTeX 排版边界；不修改模型、提示词、采样、裁图、重试次数或质量评分工具。

在当前源码重新构建后，经固定模型响应的公共作业入口复现：

| 输入 | 修复前 | 修复后 |
|---|---|---|
| 正文 `$p_{sbl}$`、`$q_{sik}$` | `partial / invalid_inline_formula_syntax` | 原样导出，实际排版通过 |
| 正文 `$FeO$` | 同上 | 原样导出，实际排版通过 |
| 正文 `$s_{甲}^{2}=1.2$` | 同上 | 原样导出，实际排版通过 |
| 独立公式区域包含说明及两条积化和差公式 | `partial / invalid_formula_syntax` | 一个 Region、一次识别，按原顺序导出所有片段 |
| `<table><tr><td>$x^2$</td></tr></table>` | 表格状态 `ok`，数学渲染调用 0 次，保留未排版源码 | 单元格数学进入真实 KaTeX 排版 |

最小案例取自已保存输出中的记法，多段公式使用等价的最小复现文本。原始识别文字全程保留。修复前日志、当前源码构建和逐步红绿结果位于本机 `.scratch/vlm-postprocess/`；精简前后结果与工件身份见 [证据](vlm-postprocess-evidence.json)。这些是固定输出重放和格式验证，不是新模型推理或 OCR 精度提升的证明。

## 行为与兼容性

公式校验允许上下标参数中的长字母记号和中文标签，以及由大写字母和可选小写后缀组成的化学记号。它检查表示法，不推断物理量、化学元素或答案是否正确。参数范围之外的普通长说明、未知命令、损坏花括号/环境、生成截断和异常输出仍按既有路径拒绝。

独立公式优先按单个表达式解析；失败后仅在完整文字中存在至少一个合法数学片段，且所有数学分隔符及片段均有效时，保留原始混合内容。`type=formula`、Region、来源和一次识别记录不变。单表达式继续使用 `format=latex`；混合内容使用 `format=markdown` 和 DocumentIR 1.11，具体契约见 [Schema 说明](../../schemas/document-ir/README.md)。这不是将公式任务改成正文任务，也不会补写遗漏内容。

普通 `--reexport` 保留原 JSON 和状态。显式 `--revalidate` 可用当前规则重新判断保存的语法失败；1.10/1.11 中恢复的混合公式升级到 1.11，并保存旧 JSON 和前后记录。更早版本缺少 1.10 的标签契约时，不凭空补造字段以恢复混合公式。PDF 使用 1.11 时保留跨页结构和资源。

表格继续输出规范化 HTML 和单元格结构。可复用的参考渲染器先解析 HTML、恢复单元格文本，再只解释数学分隔符，保持合并单元格与普通文字。首次导出和重新导出的 Markdown 使用同一渲染器验证。浏览器渲染失败显式报错，不改变已有 JSON 的识别状态。

## 验证与预览

针对性回归覆盖长下标、化学式、中文下标、多段及混合行内/显示公式、损坏第二条公式、未知命令、截断、原始输出保留、JSON 篡改拒绝、重新校验和 PDF。表格覆盖合并单元格、矩阵、不等号 HTML 实体、四种数学分隔符、普通文字/链接/HTML 保护、代码块、货币及错误传播。

```bash
cmake -S . -B build/vlm-postprocess -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_TEST_KATEX=ON -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON \
  -DDOCOCR_MNN_ROOT=/home/dr/project/MNN -DPython3_EXECUTABLE=/usr/bin/python3.12
npm ci --prefix tests/math-rendering
cmake --build build/vlm-postprocess -j4
ctest --test-dir build/vlm-postprocess --output-on-failure
.scratch/issue27-test-env/bin/python -m unittest discover -s tests -p 'test_*.py' -v
```

离线预览使用 `node tests/math-rendering/preview.mjs 作业目录/document.md`，详见[渲染说明](../../tests/math-rendering/README.md)。本机[修复预览](../../output/issue27-vlm-postprocess-20261002/document.html)及[截图](../../output/issue27-vlm-postprocess-20261002/preview.png)已通过 Chromium 检查：12 条公式（其中 6 条在表格内）可见，数学错误和失败资源请求均为 0，字体加载与合并单元格正常。

生产 C++ CLI 无新增 Node.js 依赖。其他 Markdown 查看器仍须支持 HTML 表格内数学；本轮交付参考渲染器和离线预览，并不修改外部查看器。#27 的父区域完整质量验收继续保持独立，本轮不关闭该 Issue。

完整验证：Linux/MNN 构建通过，CTest 32/32（80.77 秒）、Python unittest 40/40（70.021 秒）通过；Python 编译、Node.js 语法、Schema SHA 和差异空白检查通过。测试时工作区原有 `src/region_structure.cpp` 修改保留且未纳入本轮提交，其源码 SHA 单独记录。

双轴审查发现的货币混排及数字开头公式回归已修复，最终 Standards/Spec 均为 0 项未解决，见[审查记录](vlm-postprocess-review.md)。完整套件对应 `6ff6e11`，后续 `4fb1ffb` 通过受影响的两项导出/排版 CTest；常用 `build/linux-current` 已重新构建，首次导出/生产重新导出及最终浏览器预览再次验证通过。
