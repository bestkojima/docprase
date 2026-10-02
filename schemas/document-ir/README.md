# DocumentIR 版本化 Schema

本目录是生产 CMake 构建和当前公共作业、JSON 重新导出回归读取 Schema 的稳定入口。普通识别输出为 1.10；包含正常混合公式区域的输出为 1.11。保存后的 1.0～1.11 文档继续按其版本与 `source.type` 选择对应契约。部分无识别或固定结果作业沿用较早的输出版本。

1.0～1.9 的 15 份 Schema 从历史 Issue 目录逐字节复制，没有修改字段、格式、版本、约束或 `$id`。1.10 新增标签导出策略、版面语义标签及正文序号，见 [标签流水线](../../docs/label-pipeline.md)。历史来源保留原路径，供冻结报告及旧候选回放使用；来源链接和当时结论见[证据索引](../../docs/evidence-index.md)。

## 版本与来源

| 版本 | 输入类型 | 稳定文件 | 历史来源 |
|---|---|---|---|
| 1.0 | image | [Schema](document-ir-1.0.schema.json) | [#4](../../docs/issue-4/document-ir-1.0.schema.json) |
| 1.1 | image | [Schema](document-ir-1.1.schema.json) | [#8](../../docs/issue-8/document-ir-1.1.schema.json) |
| 1.2 | image | [Schema](document-ir-1.2.schema.json) | [#9](../../docs/issue-9/document-ir-1.2.schema.json) |
| 1.3 | image | [Schema](document-ir-1.3.schema.json) | [#10](../../docs/issue-10/document-ir-1.3.schema.json) |
| 1.4 | pdf | [Schema](document-ir-1.4.schema.json) | [#11](../../docs/issue-11/document-ir-1.4.schema.json) |
| 1.5 | image / pdf | [image](document-ir-1.5-image.schema.json)、[pdf](document-ir-1.5-pdf.schema.json) | [image](../../docs/issue-18/document-ir-1.5-image.schema.json)、[pdf](../../docs/issue-18/document-ir-1.5-pdf.schema.json) |
| 1.6 | image / pdf | [image](document-ir-1.6-image.schema.json)、[pdf](document-ir-1.6-pdf.schema.json) | [image](../../docs/issue-21/document-ir-1.6-image.schema.json)、[pdf](../../docs/issue-21/document-ir-1.6-pdf.schema.json) |
| 1.7 | image / pdf | [image](document-ir-1.7-image.schema.json)、[pdf](document-ir-1.7-pdf.schema.json) | [image](../../docs/issue-22/document-ir-1.7-image.schema.json)、[pdf](../../docs/issue-22/document-ir-1.7-pdf.schema.json) |
| 1.8 | image / pdf | [image](document-ir-1.8-image.schema.json)、[pdf](document-ir-1.8-pdf.schema.json) | [image](../../docs/issue-26/document-ir-1.8-image.schema.json)、[pdf](../../docs/issue-26/document-ir-1.8-pdf.schema.json) |
| 1.9 | image / pdf | [image](document-ir-1.9-image.schema.json)、[pdf](document-ir-1.9-pdf.schema.json) | [image](../../docs/issue-26/document-ir-1.9-image.schema.json)、[pdf](../../docs/issue-26/document-ir-1.9-pdf.schema.json) |
| 1.10 | image / pdf | [image](document-ir-1.10-image.schema.json)、[pdf](document-ir-1.10-pdf.schema.json) | 当前标签流水线契约 |
| 1.11 | image / pdf | [image](document-ir-1.11-image.schema.json)、[pdf](document-ir-1.11-pdf.schema.json) | [公式区域混合内容](../../docs/issue-27/vlm-postprocess-fix.md) |

1.11 保留 `type=formula` 与单一 Region。单表达式仍为 `format=latex`；含完整数学片段和说明的区域为 `format=markdown`，`text` 保存原始识别文字及分隔符，顺序不变。混合内容的 `display=false` 表示块级不再统一包裹，每个数学片段由自身分隔符决定行内/显示模式。仅普通文字、损坏数学或未完成生成不会因此成为正常公式。首次导出、重新导出均核验保存的识别文字与展示内容一致。

## 字节核验

在仓库根目录执行：

```sh
sha256sum -c schemas/document-ir/SHA256SUMS
```

[SHA 清单](SHA256SUMS) 覆盖 19 份稳定契约及 15 份历史来源。1.0～1.9 副本与历史来源的哈希相同，`$id` 中的原 Issue URL 也保留；该元数据身份不要求读取历史目录。CMake 将稳定文件嵌入 CLI，JSON 重新导出在运行时无需再读取这些源文件。

## 维护约定

- 新契约使用新版本，更新 CMake 内嵌列表、版本选择和公共 CLI 回归；已封存版本的语义与来源不静默改写。
- 校验、错误提示、原图资源、状态与唯一归属仍遵循对应版本。旧文档没有的视觉或结构证据不补造。
- 历史冻结副本、源 Schema、原报告与 SHA 清单继续作为该轮证据。新候选须重新冻结生产代码及实际使用的契约；历史脚本使用当时工具版本。
