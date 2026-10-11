# 识别前页面计划

第 1 项结构优化将内容归属、区域身份与裁剪、图片与图注分组以及顺序规划集中到 `page_plan` 模块。`page_pipeline` 通过一个入口 `plan_page(PagePlanInput)` 得到完整计划，随后由 `page_execution` 按计划创建资源和执行识别，再由 pipeline 调用导出。

```mermaid
flowchart LR
    A[版面推理与候选筛选] --> B[plan_page]
    B --> C[冻结页面计划]
    C --> D[创建裁剪与资源]
    C --> E[按 recognition_order 识别]
    C --> F[按 block_order 导出]
    D --> E
    E --> F
```

## 文件职责

| 文件 | 职责 |
|---|---|
| `src/page_plan.hpp` | 输入候选、完整计划和单一规划入口；不暴露引擎、像素、资源存储或识别结果 |
| `src/page_plan.cpp` | 表格归属、重叠文本归根、行内公式归属、稳定身份、裁剪扩展和三份顺序 |
| `src/region_structure.cpp` | 图片分组、图注关联及歧义检测，作为规划内部实现 |
| `src/layout_geometry.hpp` | 筛选、规划与导出关联共用的私有几何规则 |
| `src/page_pipeline.cpp` | 组合候选筛选、复核、冻结计划、区域执行和文档导出，负责预算与资源提交 |
| `tests/page_plan.cpp` | 直接通过规划入口验收，不需要模型或图像解码 |

## 计划约定

- 输入候选已经过筛选，包含页面内有效的几何框、原始类别、候选编号和模型 rank。候选编号可稀疏；重复编号、无效页面尺寸或越界框被拒绝。
- `regions` 按几何身份顺序保存。`layout_box` 保留检测几何，`box` 包含归属子内容的完整裁剪；扩展裁剪不改写原版面框。
- 归属先处理表格，再处理重叠文本归根，最后处理行内公式。歧义关系保持独立输出。仅版面作业关闭归属合并。
- `block_order` 与 `region_order` 表达阅读顺序；`recognition_order` 表达可识别区域的执行顺序。图片保存为资源，图片分组不合并识别区域。仅版面作业保留顺序描述但不执行内容识别。
- 图片作业保持 `b0001` / `l0001` / `r0001` 身份；PDF 页保持 `p0007-` 等页前缀。重新排序不重建身份。
- 调用方持有 `const PreRecognitionPagePlan`，识别状态、异常、重试和识别文本不能改变计划。水平图片组通过显式结构信息参与排序，不改写模型类别标签。

## 验证

`page_plan` 直接测试覆盖归属优先级与歧义、跨框裁剪、归属链、稀疏候选编号、双栏与 rank 冲突、PDF 身份、图片与图注、三份顺序的一致性、空页、未知类别和非法输入。

既有 `issue27_structure` 已纳入 CTest，继续验证实际作业的图注分组及计划与识别请求顺序。`issue26_structure` 同时覆盖图注识别失败后绑定关系保持稳定。

第 1 项完成时，Release 无模型构建的 35 项 CTest 全部通过，包含 PDF、取消、重试及作业控制。重构前当前工作区的原有 33 项也已通过；该轮新增页面计划直接测试并注册既有图注结构测试。

2026-10-11 对重构前当前工作区和重构后构建进行了 5 组端到端对照：归属、双栏、图片分组、未知类别和空页。排除耗时字段后，完整 DocumentIR、Markdown 与全部资源 SHA256 一致。此项是结构与行为回归验证，未评测真实模型质量或 Android 性能；Android 仓库内的源码快照尚未同步。

构建与运行无模型测试：

```sh
cmake -S . -B /tmp/docprase-page-plan-check -DCMAKE_BUILD_TYPE=Release \
  -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/tmp/docprase-no-mnn \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build /tmp/docprase-page-plan-check -j2
ctest --test-dir /tmp/docprase-page-plan-check --output-on-failure
```

Python 环境需要 Pillow 和 jsonschema。集成测试会创建裁剪、叠加图和作业工件，应将 `TMPDIR` 指向空间充足的位置。
