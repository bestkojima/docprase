# Issue #8：印刷公式归属与 LaTeX 导出

本项沿用 #7 的统一 Ovis 提示词、真实 Layout 和公共 C ABI 作业。首轮验证范围是 Linux CPU 上的清晰印刷教材页；模型输出按原文保存，不做数学纠错。表格、复杂阅读顺序、多页 PDF 和 Windows 实测仍按各自后续任务处理。

## 处理契约

- Layout 选中的公式框只有被更大的文本框完整包含，且最小面积的候选父文本唯一时，才视作行内公式证据。父文本仍使用原始完整裁剪识别；子公式不再单独转写，也不再插入 Markdown。最小面积等值竞争、部分重叠或无父文本的公式保持独立。此规划仅在开启区域转写时生效；Layout-only 作业仍逐候选导出区域与 mask。
- 含归属关系的结果使用 [DocumentIR 1.1 Schema](document-ir-1.1.schema.json)。父 Region 的 source_layout_block_ids 同时引用自身和归属的公式 LayoutBlock；relations 中的 content_owned_by 指明公式 LayoutBlock 与父内容块。父裁剪保留完整像素，子候选保留原框、检测信息、诊断 mask 和无损原始 mask 张量。没有归属关系的旧作业仍输出 DocumentIR 1.0；[1.0 Schema](../issue-4/document-ir-1.0.schema.json)没有改动。
- 独立公式仅在外层分隔符完整、花括号/圆括号/方括号配对、常见 TeX 命令可辨认且必需参数存在时转为 LaTeX 内容。content.text 是去掉一次外层包裹符后的原公式内容，display 明确标记导出形式。provenance.raw_output 保留模型原样；识别置信度仍为 null。Markdown 渲染时只在展示层转义 HTML 特殊字符，JSON 里的 LaTeX 不改写。
- 文本块中明确以数学包裹符标记的公式也检查闭合与内部结构。公式跨度外的中文标点不参与数学校验。未闭合、缺少必需参数、未知命令、混入正文、模型截断或不可信公式编号标记会保留 raw、原图和 partial 错误；不会补括号、补参数或改数学内容。
- 题号、邻近数字仍作为自己的文字内容输出。本项不推测编号关系，也不自动生成公式标签。模型返回的 TeX 标签命令在没有额外几何与内容证据时不接受为可信公式内容。

## 固定业务样例

[原始教材页](../../tests/fixtures/ovis/formula_book_page.png)与[来源、SHA 和人工标注摘录](../../tests/fixtures/ovis/formula_book_page.manifest.json)来自 OmniDocBench 修订 aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec 的简体中文 book 页，包含中文段落、行内数学、两条独立公式、编号及一条混合文字的公式区域。该页不是合成测试页；受控异常另由 fixture 在相同公共作业边界验证。

复现命令：

    cmake -S . -B /tmp/dococr-issue8 -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN
    cmake --build /tmp/dococr-issue8 -j2
    ctest --test-dir /tmp/dococr-issue8 --output-on-failure
    python3 tests/printed_formula_real.py /tmp/dococr-issue8/dococr_cli output/issue-8/real-regression
    /home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests

真实回归脚本核对固定图片哈希、DocumentIR 1.1 Schema、父子引用与几何、每条归属父裁剪的原图像素、所有选中 Layout mask、独立公式与人工 LaTeX 真值（仅忽略空白）、Markdown 不重复、题号不变成标签、混合公式 partial/raw/原图，以及 JSON 往返语义。业务质量另按模型输出与真值报告，status=ok 仅表示结构解析通过，不表示数学内容识别正确。
