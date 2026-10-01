# Issue #26：image 资源与 odb-07/08 实测

2026-10-01。本报告保留统一映射的两页初次实测；源码与完整验收证据现已提交，最终结果见 [1.9完整20页验收](acceptance-20-v1.9.md)。按用户本轮要求，图片的 `recognition_type` 为 **image**，保存为资源并由 Markdown 引用；Ovis 只处理 text / formula / table。

## 查看 Markdown

- [odb-07：完整 Markdown](/home/dr/project/docprase/output/issue-26/region-mapping-07-08/odb-07/review.md)
- [odb-08：完整 Markdown](/home/dr/project/docprase/output/issue-26/region-mapping-07-08/odb-08/review.md)

查看版仅把图片地址转换为本机绝对路径，正文与识别结果不变。生产输出的 `job/document.md` 保留相对资源地址，原始 DocumentIR 和裁图均保存在同一作业目录。

## 实测结果

通过生产公共 CLI 重新执行完整原页，使用真实 MNN / PP-DocLayoutV3 / OvisOCR，SmartResize Lanczos、阈值0.3、CPU单线程。没有复用历史转写。

| 页面 | Ovis 文字区域 | image 资源 | 图片 Ovis 调用 | 图注绑定数 | 页面状态 | 推理作业耗时 |
|---|---:|---:|---:|---:|---|---:|
| odb-07 | 9 | 2 | 0 | 0 → 1 | partial → ok | 40.2秒 |
| odb-08 | 27 | 17 | 0 | 12 → 13 | partial → ok | 69.7秒 |

两页合计36个文字任务、19个图片资源。所有 Markdown 图片地址均存在，全部图片块的生成尝试为空。资源保存成功记为 ok，不再因无需识别而导致页面 partial，也不算作 OCR 成功次数。

- odb-07：c10 `vision_footnote` 的斐波那契说明归一化为 text，绑定图片 c9，在 Markdown 中紧随图片输出。
- odb-08：c42 普通 text 的“第8题”绑定图片 c20，不要求图片先进入横排组。

与上一轮已验收的1.8生产结果比较：候选、裁图、内容归属、OCR文字和阅读顺序一致；本次增加两处显式图注关联并修正资源状态。共同 GT 顺序对没有新增错误，也没有新的顺序修复。原有其他顺序问题仍保留，本次不宣称整页阅读顺序或整体 OCR 质量已经解决。

## 实现与核验

类别表集中在 `src/layout_region_policy.cpp`。图注规划消费归一化 text 类型及集中定义的用途提示；真实脚注、标题和页眉页脚继续保留各自用途。图片不进入识别计划，识别入口也拒绝 image 任务。

DocumentIR 新输出为1.9：`regions[].recognition_type` 明确记录 text / formula / table / image，未支持来源另记 unknown；`structure_plan.recognition_order` 只列实际 OCR 区域。原始类别、class ID、rank、框与 mask 不覆盖。1.8及更早文件继续按原契约重新导出。

25/25 CTest 通过。专项回归覆盖两个实际几何反例、真实脚注、标题、偏离图片中心的短正文、纯图片页、被篡改的类型/识别顺序/图片生成证据。两页生产重新导出的 Markdown、JSON 和资源与第一次导出等价。

核验记录：

- [冻结来源与运行配置](/home/dr/project/docprase/output/issue-26/region-mapping-07-08/freeze.json)
- [两页结构核验](/home/dr/project/docprase/output/issue-26/region-mapping-07-08/acceptance-partial.json)
- [与上一轮1.8的直接比较](/home/dr/project/docprase/output/issue-26/region-mapping-07-08/review.json)
- [odb-07 新标注 PNG](/home/dr/project/docprase/output/issue-26/region-mapping-07-08-png/odb-07/04-reading-order.png)
- [odb-08 新标注 PNG](/home/dr/project/docprase/output/issue-26/region-mapping-07-08-png/odb-08/04-reading-order.png)

本报告仅记录这两页初次实测。此前1.8完整20页与后来完成的1.9完整20页分别保留，不能互相冒充。
