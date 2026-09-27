# Issue #13：从 DocumentIR JSON 重新导出

## 使用

```sh
dococr_cli --reexport old-output/document.json \
  --asset-root old-output --out new-output
```

`--asset-root` 是旧作业的资源根目录，必须显式提供；资源路径按 JSON 中保存的相对路径解析。`--out` 必须是尚不存在的新目录，且不得与资源根重叠。成功后得到 `document.json`、`document.md` 和 `assets/`。原 JSON 按输入字节复制；不会启动文档作业、模型或 PDF 渲染器。入口在生产 `dococr_cli` 中，未接受 `fixture:*` 后端的生产限制保持不变。

## 读取契约

支持已发布的 DocumentIR 1.0、1.1、1.2、1.3 和 1.4；不修改旧 Schema。构建时把五份已发布 Schema 原文嵌入 CLI，按 `schema_version` 校验字段、类型、枚举及条件约束。读取器再检查原页号、ID 唯一性、坐标边界、版面块到区域到内容块的引用、`reading_order` 全覆盖、内容归属和图文语义关系以及资源引用。1.2 及之后的成功表格还用原有严格表格解析器核对保存的 HTML 与结构化单元格是否一致；1.0/1.1 的成功 HTML 表格同样须通过严格解析。1.4 检查选中页范围、原页顺序、页错误占位及已保存的 PDF 几何字段。错误输入以非零退出码和字段诊断结束，不生成输出目录。

输出资源包括 `resources[]` 中的插图/区域裁剪，以及文档级或页面级 `layout_diagnostics` 的 overlay、原始张量和候选 mask。所有引用资源须在资源根内真实存在且为普通文件；相对路径不得越界，符号链接资源被拒绝。逐字节复制后，Markdown 中的图片和状态占位引用仍指向新目录中的同一路径。缺少任一资源时不会留下半成品输出目录。

Markdown 使用与原作业共同的 `render_markdown_block`。所有正文、公式和失败占位都按相同展示规则转义 HTML 特殊字符及外部图片语法；仅通过严格表格解析器且文本与规范化 HTML 一致的成功表格作为 HTML 输出。展示转义不改变保存的正文、公式、状态、ID、原页、源区域、关系、`confidence:null`、原始输出及来源信息，也不重跑识别。

## 验收

`tests/reexport_integration.py` 只从生产 CLI 和测试专用作业 CLI 两个公共入口观察结果。它固定覆盖 1.0 的文字、公式、表格、图片夹具，以及 1.1 公式归属、1.2 表格结构、1.3 图文顺序/关系和 1.4 多页 partial/failed/blank 的往返。`tests/fixtures/reexport/` 另保存从 #8/#9 历史真实作业缩减的 1.1 公式归属和 1.2 整表固定文档、原 PNG 与 Markdown 期望值；提取只删除未选块及可选诊断，不改选中内容。旧版件先经对应已发布 JSON Schema 校验。逐字节比较原 Markdown 与全部引用资源，逐语义或逐字节比较 JSON，并测试未知版本、非法字段/引用、伪造表格/资源、路径越界、符号链接、重复 JSON 键、HTML 注入、缺失资产和 PDF 页错误。测试不加载模型权重。

见 [verification.md](verification.md) 的实际命令与结果。
