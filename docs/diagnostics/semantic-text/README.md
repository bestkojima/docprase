# 图例与答案/解析的排版诊断

2026-10-02。密封线标签修复提交 `8eef447` 后，继续处理用户报告的图例误标题及 `## \[答案]:A \[解析]:` 排版问题。正式任务和规格仍以 GitHub Issues 为准，此处记录可复现诊断和验证证据。

## 根因与最小复现

`odb-01/b0042` 的源类别是 class 24 / `vision_footnote`，Ovis 在同一个块中输出两行 `## ...`。内容分别是“电子信息制造业企业利润总额增速”“工业企业利润总额增速”，对应图中两条曲线。渲染器直接保留标题前缀，导致 Markdown 生成两个二级标题。

答案行在 class 22 / `text` 与 class 17 / `paragraph_title` 中都有出现。原始 `## [答案]:A [解析]:` 缺少字段分段；安全转义器又把 `[答案]:` 识别为 Markdown 引用定义，因此输出为带反斜杠的标题。仅修改版面标签无法同时解决这两个问题。

公共 CLI 的最小用例位于 `tests/semantic_text_rendering.py`。修复前执行 `--case legend`、`--case answer` 均在“误生成标题”断言失败；独立行的答案和解析还通过 `--case separate-lines` 复现了缺少段落间隔的问题。引用示例用例曾暴露错误拆分，现通过保护公式、代码和成对引号解决。

## 修复与契约

修复位于共享 Markdown 渲染器：先按语义标签消除图注/图例的误标题，再识别明确的答案/解析标记并分段，最后沿用安全转义。初次导出与离线导出都为 DocumentIR 1.10 传入语义标签；1.0～1.9 保持历史渲染行为。

不改写 `content.text`、`provenance.raw_output`、检测类别、状态或阅读计划。空解析不补写，误识别文字不猜测修正。当前图例框部分覆盖图表底边，尚无 `caption_of` 关联；此次解决展示，未新增几何关联规则或题目归属推断。

## 验证

```sh
cmake --build build/linux-current -j4
ctest --test-dir build/linux-current --output-on-failure -j4
/usr/bin/python3 tests/semantic_text_rendering.py \
  "$PWD/build/linux-current/dococr_cli_fixture" \
  "$PWD/build/linux-current/dococr_cli"
/usr/bin/python3 docs/diagnostics/seal-margin/replay.py \
  --mode original --out output/semantic-text-fixed-20261002
```

当前构建 31/31 项 CTest 通过。新增回归覆盖 18 个最小用例，包括图注、图例、误标成标题的答案、行内答案、多选、中文括号、跨行解析、空解析、真标题、公式/代码/引用示例、安全转义及 1.9 历史展示。

两张开发页的完整原始检测张量/mask 和历史 Ovis 输出重放通过，分别有 7、12 组明确的答案/解析字段正确分段，图例不再生成标题。原有标题保留；密封线泄漏仍为零，20 个旁注块及原文、状态、裁图完整保留。新作业的首次导出和离线导出逐字节一致。

另用生产 CLI 直接重新导出此前保存的两个 1.10 JSON，JSON 文件逐字节保持不变，Markdown 与完整重放生成的新 Markdown 一致。这证明排版修复不需要重新识别。新的完整重放 JSON 与此前作业仅有识别耗时差异，未发生内容变化。

提交前另从暂存内容抽取完整源码进行隔离构建，排除工作区已有但不属于本次提交的修改。隔离构建的 31/31 项 CTest 通过；两页完整重放通过，Markdown 与上述工作区验证一致。验证记录补充前的源码树、二进制哈希和结果保存在 `evidence.json` 的 `commit_candidate_validation` 中；产物位于 `output/semantic-text-commit-verified-20261002/`。

轻量结果及哈希见 [evidence.json](evidence.json)。本地可浏览结果位于 `output/semantic-text-fixed-20261002/odb-01/job/document.md` 和 `odb-02/job/document.md`；已有 JSON 的重导出结果位于相应 `prior-json-reexport/`。

以上为确定性重放，未执行新的模型推理；两页属于已曝光开发样本，不能推导未见文档的整体识别质量。
