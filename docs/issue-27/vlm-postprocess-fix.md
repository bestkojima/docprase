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

## 常用符号与正文货币处理机制

后续检查复现两项缺口：`\div`、`\cup`、`\epsilon`、`\Omega` 被误判为语法错误并导致父正文回退；`价格 $5，公式 $x^2$。` 虽然状态为 `ok`，但未经转义的金额会干扰 Markdown 数学解析，导致预览失败。本次修复仅承接这两项，不将其他已发现的表格换行、表格数学校验或重复上下标问题计作完成。

无参数的数学符号与运算符统一维护在 [版本化目录](../../configs/formula-symbols.json)，按希腊字母、算术、关系、集合逻辑、箭头、大型运算符、具名运算符等分类。CMake 将目录嵌入产物，运行时不读取可变配置，也不依赖 Node.js。分式、根式、注释、环境等有参数或影响解析状态的命令仍走专门的结构检查；未知命令、宏定义和外部链接命令不会因扩展符号范围而获准。目录并不宣称支持完整 TeX。

扩展符号时修改目录，再运行启用 `DOCOCR_TEST_KATEX=ON` 的 `markdown_katex_integration`：测试自动遍历所有目录项，分别通过父正文和独立公式的公共 CLI、生产重新导出及固定版本 KaTeX 验证。四个原始反例另有独立回归，避免仅以目录自身作为正确性依据。带参数的命令必须新增专门校验和正反例，不能作为无参数符号加入目录。升级 KaTeX 或变更目录时必须重新运行真实排版检查。

正文校验与 Markdown 导出共用 `currency_amount_end` 的货币判定。只有判定为字面金额时，Markdown 才输出 `\$`；JSON 的内容、原始输出和来源不变。完整合法数学片段优先，保留 `$2(x+1)$`、`$2!$`、`$2FeO$` 等数字开头公式；数学续接中的反斜杠不会将未闭合公式伪装成货币。已有转义、反引号代码及代码围栏中的金额不重复转义。仅凭 `$` 无法完全消除金额与数学的歧义；明确需要字面美元符号时仍可使用 `\$`。

首次导出、重新导出和混合 formula Region 复用上述规则。普通 `--reexport` 保持保存的 JSON 和判定；`--revalidate` 可恢复旧版误拒绝的合法符号，保留旧文档及前后记录。回归覆盖中英文金额、小数和千分位、多个金额与公式、旧式公式分隔符、代码示例、历史恢复，以及未知命令、坏参数和未闭合公式的拒绝。

本轮完整验证：Linux/MNN Release 构建通过，CTest **32/32**（96.25 秒）、Python unittest **40/40**（66.198 秒），Python 编译、Node.js 语法及差异空白检查通过。全部 195 个目录符号经过父正文、独立公式、首次导出、重新导出及真实 KaTeX 排版验证。[证据与红绿记录](math-policy-evidence.json)单独保留，不覆盖上一轮结果；这仍是受控 VLM 输出的后处理验证，不是新一轮 OCR 精度结论。

上述全量套件对应 `f2b1fd7`。双轴审查发现并在 `1b37b5c` 修复一项兼容性回归：金额后紧接 `\(`、`\[` 或已转义的 `\$` 时，将其识别为后续独立内容，其他反斜杠仍作为数学续接检查。修正后的公式/历史重新校验专项通过，Markdown 导出与真实 KaTeX 两项最终复测 **2/2**（20.96 秒）；期间修正了一处测试变量被下一案例覆盖的问题，失败与重跑日志均保留。最终 Standards、Spec 各 **0 项未解决**，详见[审查记录](vlm-postprocess-review.md)。常用 `build/linux-current/dococr_cli` 已构建至该修正；先前五个原始反例也已通过该运行目录的 CLI 恢复或重新导出并生成离线 HTML。

## 剩余三项：单元格换行、坏公式与重复上下标

本节承接用户要求继续修复的另外三项缺陷。参考 [PaddleX 的分类型导出处理](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/pipelines/layout_parsing/result_v2.py#L169)与 [PaddleOCR 集中的公式后处理](https://github.com/PaddlePaddle/PaddleOCR/blob/dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf/ppocr/postprocess/rec_postprocess.py#L1361)，将结构解析、数学有效性检查与显示格式化分开。参考代码的格式转换或空白规范化本身不等于数学语法已验证；本项目继续保留原始模型输出，不通过补写花括号或改写指数来猜测正确公式。

- **单元格换行**：先合并完整单元格的文本并记录 `<br/>` 位置，再识别数学边界。公式内的换行作为 TeX 空白处理，公式外仍输出 HTML 换行；不跨单元格拼接公式，保留表头和合并单元格结构。
- **坏表格公式**：HTML/网格解析成功后，再按完整单元格调用共享数学校验。已识别内容中的坏分式、未知命令、未闭合数学片段或重复上下标使父 table Region 进入 `partial / invalid_table_formula_syntax`。沿用失败表格的空结构、原图和原始输出留存契约，不新增识别任务。
- **重复上下标**：新增公式项与括号组级检查，拒绝同一底数上的重复上标或下标，区分指数参数、命令参数、嵌套组、矩阵单元格和新底数。保留 `x_i^2`、`x^{y^2}`、`x^12^3`、`{x^2}^3` 等合法写法；最大组递归深度为 256，检查仍限定于本项目支持的 TeX 子集。

历史兼容性分为两层：保存内容与原始输出的一致性校验允许读取过去漏检的重复上下标，但这不代表当前数学有效性通过。普通 `--reexport` 保留原 JSON，在当前 Markdown 中对坏数学给出原图/待核验回退，避免带坏公式进入预览。显式 `--revalidate` 使用 `formula-revalidation-v2`，可将正文、独立公式、表格的误标成功降为 `partial`，也可恢复当前规则已支持的历史语法失败；页面/文档状态随实际变化更新，旧 JSON、前后判定及来源证据均保留。没有判定证据的早期记录不凭空补造识别证据。

没有数学判定契约的早期正文继续使用原安全转义行为，例如 `\[链接]` 不因新数学检查而整段回退。第一次完整回归检出并修复了这一兼容性问题；带判定证据的旧 VLM 正文、独立公式及结构表格仍接受当前规则检查。

数学规则与展示均作用于现有父 Region，不重新推理、不改内容归属或阅读顺序。表格新错误码区分结构损坏与数学语法未通过；用户可以从原始输出和原图检查原因。未知命令仍按支持范围拒绝，以上检查不宣称实现完整 TeX 或证明识别内容正确。

本轮验证：Linux/MNN 构建通过，最终 CTest **32/32**（82.77 秒），Python unittest **40/40**（61.415 秒），静态检查通过。常用生产 CLI 对四个原始反例分别执行普通重导出和显式重新校验，八次操作均生成可排版的离线 HTML，原始输出保持一致。首次完整回归的旧正文兼容失败、修复与最终重跑均见[独立证据](remaining-postprocess-evidence.json)。
