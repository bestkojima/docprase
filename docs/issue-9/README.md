# Issue #9：整表内容归属与结构化导出

本项在 #7 的真实单页作业和 #8 的公式归属之上，使用同一 Layout、Ovis、统一提示词和公共 C ABI。验证范围为 Linux CPU 上清晰印刷教材页。Ovis 转写只被当作原始证据；表格结构和单元格内容分别检查，不用正常停止推断内容准确。

## 处理契约

- 内容规划先查找完全包含子文字或公式区域的表格。仅当最小面积表格唯一、严格大于子区域时，表格拥有该子区域；再对剩余公式应用 #8 的文本父框归属。部分交叠、等面积竞争和表外标题、表注保持独立。被拥有的候选仍保留 Layout 记录、原框、mask 与 `content_owned_by` 关系，不再重复转写或导出。
- 完整正常停止的表格必须是闭合、单一、矩形的 `<table>`。解析器接受 `tr`、`td`、`th`、`thead`、`tbody`、`tfoot`、`br` 和有限正整数 `rowspan`/`colspan`；Ovis 常见 `border=0/1` 只作输入兼容，规范化 HTML 不保留该显示属性。按合并跨度建立单元格行列坐标，校验重叠、空洞和越界；单元格的原页 `bbox` 为 `null`。
- 安全导出只包含上述白名单元素与属性。未知标签、`img/src`、脚本、事件或样式属性、重复跨度属性、未闭合标签、HTML 前缀、非矩形结构、token 上限均不会成为成功表格。此时 `status=partial`，结构对象为 `null`，面向阅读的内容为空并显示原图占位；原始模型输出仍完整保存在 `provenance.raw_output`，裁剪 PNG 可读。模型虚构的 `images/bbox...` 不会变成导出资源或 Markdown 图片引用。
- 表格转写路径有表格块时使用 [DocumentIR 1.2 Schema](document-ir-1.2.schema.json)：`content.text` 是规范化 HTML，`content.table` 包含行数、列数、每格位置、跨度、表头标志、文字和 `bbox:null`。原始 HTML 保存在 provenance。旧的无表格归属公式路径保持 1.1；Layout-only 和旧 fixture 作业保持 1.0。1.0/1.1 Schema 未修改。Markdown 直接放入经白名单构造的 HTML 表格；JSON 保留再次导出所需的表格语义。

## 固定业务样例与质量

普通表格使用 [OmniDocBench 教材原页](../../tests/fixtures/ovis/source_page.jpg)，源图 SHA-256 为 `c8cf71eb2f717727dc2d8a3ae5da1e388f6be7bb1e2c4addbde5d40dafb270f6`，完整来源、裁剪与参考见 [原有样例清单](../../tests/fixtures/ovis/manifest.json)。JPEG 先无损转为 RGB PNG，以便独立按原页像素核对导出的裁剪。真实整页公共作业的普通表格是 5 行 × 3 列，15 格结构与参考完全一致，14 格文字逐字相同；首格 `序号` 对参考 `序 号` 仅差空格。该页其他小文字区域仍可达到 token 上限，整页状态为 `partial`。

合并单元格使用 [OmniDocBench 简体中文教材原页](../../tests/fixtures/ovis/merged_table_book_page.png)和[来源与人工标注摘录](../../tests/fixtures/ovis/merged_table_book_page.manifest.json)，源图 SHA-256 为 `c9830bd637f0795420feff161f469cbb69fddcb6af6c30b68498caaaa65988fc`。真实整页主表是 10×4、26 格，`rowspan`/`colspan` 的行列位置逐格与参考一致；26 格都非空，但仅 8 格逐字相同，其余主要是 TeX 表记差异，不能声称内容精度通过。该页第二张普通表是 2×4、8 格，结构一致，6 格逐字相同。主表与小表共拥有 19 个表内公式候选，未生成表外重复块；外部“表 E.0.2-1 …”表题仍独立保留。人工标注中的数学 `<` 未作 HTML 转义，独立比较器只在**参考侧**转义该比较符后解析，原始标注及 SHA 保留供复核。

另以同数据集的物理教材页 `page-d5f79be0-5d57-4849-9897-6106dd32117a.png` 核对不支持结构，下载页 SHA-256 为 `dc35626bf3405e7829e35c36265b06be82c95d5d03318a6eab17662afc4f6601`。Ovis 在单元格输出 `<img src="images/bbox_...">`，这些路径没有对应资源。产品将该表格标为 `partial/invalid_table_structure`，只保留 raw 与整表裁剪，不伪造图片资源或单元格坐标；[原始结果和状态证据](evidence/unsupported-image-cell.json)可复核。单元格内真实插图重建、嵌套表格、任意 HTML/样式及跨页合并均未支持；它们需要另行定义图像来源和定位契约。

## Linux CPU 复核

```bash
cmake -S . -B /tmp/dococr-issue9 -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN
cmake --build /tmp/dococr-issue9 -j2
ctest --test-dir /tmp/dococr-issue9 --output-on-failure
/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v
python3 -c "from PIL import Image; Image.open('tests/fixtures/ovis/source_page.jpg').convert('RGB').save('output/issue-9/source-page-rgb.png')"
/tmp/dococr-issue9/dococr_cli --config configs/printed-page.example.json --input output/issue-9/source-page-rgb.png --out output/issue-9/real-ordinary-rgb
/tmp/dococr-issue9/dococr_cli --config configs/printed-page.example.json --input tests/fixtures/ovis/merged_table_book_page.png --out output/issue-9/real-merged
python3 tests/printed_table_real.py output/issue-9/real-ordinary-rgb output/issue-9/real-merged output/issue-9/source-page-rgb.png output/issue-9/real-summary.json
```

真实比较器核对两页 SHA、DocumentIR 1.2 Schema、表格结构与单元格内容、唯一表格输出、19 条实际归属、外部表题、所有 selected mask、逐块资源以及表格裁剪与原图逐像素一致。汇总及原始命令结果见 [证据目录](evidence/)。模型/工件哈希、CPU、有效参数和每区域停止原因在作业的 `run-manifest.json` 中；这些证据限定于固定模型与页面，不代表所有教材表格的准确率。
