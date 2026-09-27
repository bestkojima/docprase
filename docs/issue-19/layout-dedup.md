# 版面去重规则与核验记录（Issue #19）

核验日期：2026-09-27。适用参照为 PaddleX `release/3.7` 的
[`ffb64904d23708863ff5b8da312a5cbd52a7f462`](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/models/object_detection/processors.py)。
本票只对齐 NMS 和多候选大图片框过滤；分数阈值仍为本地既有 0.5，几何无效框仍先按本地安全约束过滤。PaddleOCR-VL-1.5 管线参照使用严格 `> 0.3`；本地使用 `>= 0.5`，因此 0.5 边界可保留。这一差异尚需独立质量对照，不能据此声称完整官方后处理对齐。

## 模型及契约

- 本地制品为 ModelScope `dr3334/PP-DocLayoutV3-mnn` revision `c67c1a858d5f6c855172d4cfdf931798dafa2edd`，SHA-256 `5f1a43441d70f6843012b47eb294bed7edd3d0ef2344f0074700a38cb2e29c67`。仓库内模型卡没有提供转换脚本、上游权重 revision 或转换参数，故**无法证实具体转换来源**。
- MNN 图输入为 `image` `[1,3,800,800]`、`im_shape` `[1,2]`、`scale_factor` `[1,2]`；输出为 `fetch_name_0` `[300,7]`、`fetch_name_1` `[1]`、`fetch_name_2` `[300,200,200]`。本地运行时验证名称、类型及形状；公开作业还记录原始张量。候选行按类别、分数、原框四坐标、阅读 rank 解码，mask 行与候选索引一致。
- 类别映射与固定 Transformers 模型配置的 25 类 `id2label` 对应；官方 `image` 类别为 ID 14，本地也按 ID 14 执行大片图片规则。未知 ID 保留为 `unknown` 供诊断。
- 原 Transformers 对照是处理器分数阈值 0.5，并不包含本票 PaddleX 应用层 NMS 与大图片筛除；两者不是同一层的输出契约。本地真实 MNN 候选与原模型输出的数值同一性仍未证实。

## 实施与对照

本地对可用候选按分数降序做 NMS：同类 IoU `>= 0.6`、异类 `>= 0.98` 筛除。IoU 使用冻结参照的含端点面积公式（各边长加 1），包括浮点坐标。仅在 NMS 后仍有多个框时，对原始类别 ID 14 的图片框按页面内裁剪面积筛除：横页 `> 0.82`、其他页面 `> 0.93`；若此过滤会清空候选，则按官方规则恢复全部。保留原候选数组、原框、mask 行、rank、筛除原因；筛除框不生成 Region，存活框的来源 ID 不重排。

`tests/issue19_dedup.py` 经公共 CLI 验证同类/跨类完全重叠、NMS 阈值两侧、0.5 分数边界、大片图片、未知类别、空候选（原 `layout_integration`）、Markdown/JSON/资源、Region 来源、阅读顺序和 JSON 重新导出。受控夹具输出的候选筛除原因为 `nms_same_class`、`nms_cross_class`、`large_page_image`、`below_score_threshold`。

代表性真实整页 `tests/fixtures/layout/exam-jee-346-pil-rgb.png` 经生产 CLI 跑到完整作业输出，旧基线及本次均保留候选 ID `0..15`，输出 16 个块、16 个阅读顺序项；其余 284 个候选均低于现行分数阈值，未新增重复或漏块。`python3 tests/issue19_real.py build/dococr_cli` 可复核逐候选 ID、块来源、阅读顺序项数以及 JSON/Markdown 重新导出一致性。这个样本未产生触发 NMS 的框，因此重复框行为由受控夹具验证；尚不能将单页结果推广为整体质量结论。

本票不包含 PaddleX 的包含框筛选、rank 处理、mask/多边形变换、扩框或外层 `filter_overlap_boxes` 对齐；这些仍由后续任务验收。
