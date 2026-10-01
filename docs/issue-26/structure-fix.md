# Issue #26：横排图片顺序与图片标签绑定

本报告保留1.8阶段的修复和历史验证。最新1.9的三类映射及图片资源路径见 [标签处理契约](label-policy.md) 和 [odb-07/08 实测](region-mapping-07-08.md)。

本次完成对话中确定的前两项：横排图片组被页中线打散，以及普通 text 标签未绑定图片、已存在的绑定未参与 Markdown 顺序。重点真实页为 odb-03 的第17、18题。

## 结果

| 题目 | 修复前图片顺序 | 修复后图片顺序 |
|---|---|---|
| 第17题 | A、B、D、C（c8、c5、c13、c6） | A、B、C、D（c8、c5、c6、c13） |
| 第18题 | A、B、D、C（c4、c1、c2、c0） | A、B、C、D（c4、c1、c0、c2） |

每张图片后紧接其真实 OCR 标签，包括被检测成普通 text 的标签。首次导出和生产 CLI 离线重新导出的 Markdown 逐字节一致。

- [修复后整页顺序 PNG](fixed-annotations/odb-03/04-reading-order.png)
- [第17题中间量 PNG](fixed-annotations/odb-03/q17-comparison.png)
- [第18题中间量 PNG](fixed-annotations/odb-03/q18-comparison.png)
- [真实运行凭据](fixed-annotations/odb-03/command.json)
- [20页对照记录](evidence/structure-replay-summary.json)

## 根因与实现

原来的几何回退将跨过页中线的 C 图片当作另一种栏位置，导致 D 先于 C。图注关系依赖 figure_title 类别，普通正文标签遗漏；即使关系存在，Markdown 仍直接按块顺序拼接。

现在先完成候选筛选、归属和裁图，再用固定行锚点、共同题干或多图短标签证据规划图片组。组在页面排序中作为一个结构单元，组内图片从左到右展开；每张图片与唯一几何图注作为相邻成员。支持下面的短标签，以及图片旁的小题编号；候选争用同一图片时保留不确定引用，不挑选其中一个，也不补写 A/B/C/D。

所有 Region 编号、裁图、所有权、分组、绑定与调度顺序在第一次 OvisOCR 调用前确定。图片组不合并识别裁图；普通标签仍按原 text Region 识别。识别失败时保留相同身份与绑定，输出原图回退。几何排序使用临时组单元，原候选的类别、框、rank、mask 行和过滤理由保持原值。

DocumentIR 升级为1.8，页面新增 `structure_plan`，保存 `stage=before_recognition`、组、图片/标签引用、未确定图注、块顺序和 Region 调度顺序。首次 Markdown 和 `--reexport` 使用同一计划；重新导出校验顺序、覆盖、唯一性、关系、组范围和图片/标签相邻性。旧1.0～1.7继续按原契约读取，不补造计划；旧版 schema 和 #24/#25 的冻结验收工具保持原样。

## 验证与范围

- 公共 CLI 回归先在旧代码上复现 C/D 错序，再在修复后通过。覆盖两题、像素抖动、侧边标签、边缘重叠、识别失败、不确定图注及篡改计划拒绝。
- CTest 24项全部通过，包括双栏、跨栏标题、脚注、归属、PDF、取消与离线导出。日志见 [final-ctest.log](evidence/final-ctest.log)。
- 最终代码用真实 MNN + OvisOCR 公共入口新跑 odb-03，配置仍为 SmartResize Lanczos / 0.3。两组顺序、八个绑定和运行清单的调度顺序通过核验。页面仍有原有的部分识别/校验问题，作业状态为 partial；本次没有声明整页 OCR 质量通过。
- 全20页同时对照旧核心与新核心，重放相同历史检测张量、mask 和转写。768个候选、504个 Region 的身份/裁图/归属不变，检测张量与 mask 字节不变，裁图资源字节不变；新旧重放的转写、状态和错误一致，20页首次导出均与重新导出一致。得到8个横排组、34个绑定；固定匹配的3743个可比顺序对中修正139处错序，新增错序0。该项是结构回放，不能称为20页新模型推理或完整质量评分。
- 额外 Python unittest 中21个用例通过；独立 layout probe 测试因本机缺少 NumPy 未能加载，其环境错误保留在 [unittest-with-deps.log](evidence/unittest-with-deps.log)。未将其计入通过项。

20页历史1.5产物没有完整生成尝试记录。重放从保存的 raw_output 和 stop_reason 恢复生成输入，在新旧两侧使用同一当前校验策略，不把历史校验差异归因于结构修改。旧核心来自提交 `d5cd4be`，使用与新核心相同的测试后端；库、核心源码、原图、参考、张量和 mask 的 SHA-256 保存在对照记录中。

2026-10-01 用户确认 [收窄 #26 范围](scope/README.md)。完整官方后处理审计转到 #28，水印误检转到 #29，全量多 GT/单 Region 内容对齐和全文质量验收由 #27/#24 承接，公式校验与重试继续归 #21/#22；这些未完成项保留记录，不改称已通过。#26 保留最终一次完整20页新生产运行，以及结构修改没有新增确认的遗漏、错序、重复或误绑定的验收条件。

范围调整后补齐了不确定图注的用户提示：JSON 保留未确定引用，首次 Markdown 与重新导出均展示其原图及待核验说明，识别状态单独表达。该补充先通过公共回归复现提示缺失，再实现并通过全部24项 CTest，最新日志见 [scope-ctest.log](evidence/scope-ctest.log)。最终20页生产运行冻结该版本，产物单独保存在 `/home/dr/project/docprase/output/issue-26/acceptance-20/`，不复用上述真实单页或历史重放结果。

[最终20页验收](acceptance-20.md) 已完成：20个生产命令均 exit=0，768个候选及504个 Region 的来源/裁图/归属与参照一致；8个图片组、34个绑定，20页首次与重新导出均等价。3743个固定共同 GT 对修正139个错序、新增0，尚有147对原有错序；403个块 ok、48个语法/结构校验 partial、53个图片 skipped。当前结果只证明本轮结构修改和已列反例通过收窄的验收，全部图文语义及产品质量仍未达标；当时实现尚未提交，Issue 保持 open；最新交付状态以1.9完整验收和 Issue 正文为准。

## 复现

```sh
cmake -S . -B /tmp/docprase-issue26-build \
  -DDOCOCR_MNN_ROOT=/home/dr/project/MNN \
  -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build /tmp/docprase-issue26-build -j6
ctest --test-dir /tmp/docprase-issue26-build --output-on-failure

python3 scripts/issue26_annotations.py \
  --jobs /home/dr/project/docprase/output/issue-26/real-final --check-order
```

完整真实作业资源保存在 `/home/dr/project/docprase/output/issue-26/real-final/`；20页重放保存在 `/home/dr/project/docprase/output/issue-26/structure-final/`。PNG 和凭据中的坐标、候选编号和顺序直接来自这些公共作业。
