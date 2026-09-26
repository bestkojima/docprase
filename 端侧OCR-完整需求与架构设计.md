# 端侧 OCR 文档理解系统：完整需求与架构设计

版本 1.1 · 2026-09-26 · Windows → Android JNI · PP-DocLayoutV3-MNN + OvisOCR2-MNN

已确定模型，补充配置驱动的能力管理及处理决策。模型配置已核对，权重推理未实测。第 04 文档给出最新具体化设计，阅读入口见 [README](README.md)。

---

# 端侧文档理解：需求与处理流程

## 1. 已确认目标与边界

建立可移植的 C++ 文档解析核心，把图片或 PDF 转为可阅读的 Markdown、可追溯的 JSON 和关联图片资源。参考 RapidDoc 的编排与文档重建逻辑，优先接入 MNN 模型；Windows 先落地，Android 经 JNI 复用核心。

首版采用 `Page → Layout → Router → VLM → Assembler → Markdown/JSON`。用户已明确指定 **PP-DocLayoutV3-MNN + OvisOCR2-MNN**，分别对应 `dr3334/PP-DocLayoutV3-mnn` 与 `dr3334/ovrics-ocrv2_mnn`。GLM-OCR 仅作为后续扩展。模型选择已确定；模型包实际运行和数值对齐仍是接入验收条件，不能把已选定等同于已验证。

| 优先级 | 范围 | 交付要求 |
|---|---|---|
| P0 | Windows、图片/PDF、MNN Layout+VLM | 中文/英文文本、公式、表格、图片，输出 MD/JSON/资源，支持部分失败 |
| P0 | 架构与可观测性 | 后端可替换、模型专属处理、坐标追踪、取消、错误、性能记录 |
| P1 | Android | NDK 构建、JNI、文件/像素输入、生命周期与资源限制 |
| P1 | det+rec、LiteRT/LiteRT-LM/ORT | 以相同业务契约加入，不能要求重写主循环 |
| P2 | 编辑、PDF 导出、错题组织 | 基于 DocumentIR，保留稳定 ID 与原图定位 |
| 实验 | 混合 vision/decoder、抖动、NPU | 独立实验配置，验证后才进入默认路径 |

首版默认离线，不在失败时自动调用云端。图表优先保存图片，图表语义重建为可选扩展。印章、方向分类和去弯曲采用可配置处理，不把笔记中的“关闭”理解为所有输入永远不需要。

## 2. 对原始调研的校正

| 原笔记的说法 | 本设计采用的解释 |
|---|---|
| TextRecognition 仅输出位置和置信度 | 单独 rec 输出 `rec_text`、`rec_score`；整页定位由 det 提供。见 [PaddleOCR 文本识别](https://www.paddleocr.ai/v3.6.0/version3.x/module_usage/text_recognition.html)。 |
| 版面模型直接输出 Markdown | 版面模型提供类别、几何信息，部分模型提供阅读顺序；Markdown 仍依赖识别和组装。 |
| PP-OCR-VL 1.6 固定 560×560 | [所链接 LiteRT 模型包](https://huggingface.co/litert-community/PaddleOCR-VL-1.6)声明该静态约束；不能套用到原始模型、GLM 或 Ovis。 |
| GLM-OCR 链接属于现成 MNN 包 | 用户提供的是 GLM-OCR 模型页，尚未验证其中有完整 MNN 转换包。 |
| Pipeline 先实现 | 本轮用户已明确更新为 **Layout+VLM（MNN）先实现**。 |
| 小模型能够整体上 NPU | 属于验证假设；必须检查算子、动态 shape、精度及实际委托情况。 |
| 统一长提示词能用于所有 VLM | 各模型使用其训练任务提示词、模板和特殊 token；文档输出规范由组装器实现。 |
| JSON 后处理可以补回漏检公式 | 没有视觉证据的文本修补不能恢复漏检；应重检/重识别或标记待审。 |

“PP-StructureV3-Light”在本文表示裁剪后的项目配置，不假定存在一个固定同名官方模型包。原笔记中的体积、精度、速度记录作为线索，交付前以实际 artifact 和设备报告为准。

## 3. 功能需求与不变量

| ID | 要求 | 验收依据 |
|---|---|---|
| FR-01 | 图片与 PDF 逐页输入，支持页范围、DPI、最大像素限制 | 多页结果顺序稳定，不能一次加载全部大图 |
| FR-02 | 版面输出规范化类别、原始类别、框/多边形、可选顺序 | 原类别信息保留，未知类可降级 |
| FR-03 | label→任务→模型路由可配置 | 切换模型不改 OcrPipeline |
| FR-04 | 按模型契约执行可选前后处理 | 空处理链、外部处理、运行时处理均可验证 |
| FR-05 | 文本、公式、表格、图片统一入 DocumentIR | 不依靠解析最终 Markdown 恢复结构 |
| FR-06 | 原始图像与识别内容可追溯 | 块能定位到页、源区域和模型版本 |
| FR-07 | 每区域独立 VLM 状态 | A→B 与单独 B 不发生历史污染 |
| FR-08 | 错误分块记录，默认继续文档处理 | 错误位置有占位，不静默丢块 |
| FR-09 | 输出 MD、JSON、资源与运行清单 | MD 图片均存在，JSON 可重生成 MD |
| FR-10 | 提供取消、阶段事件和资源预算 | Android 不阻塞 UI，取消后状态可复用或重建 |
| FR-11 | 后端及设备实际使用情况可查询 | 请求 GPU 而实际 CPU 时显式记录 |
| FR-12 | 稳定 ID、编辑记录、题目关联扩展 | 重新导出不覆盖人工修订 |

不变量：原图不可变；模型归一化只执行一次；坐标空间必须带标签；VLM 无定位能力时只报告区域级几何；置信度未知用 `null`；每份内容只能有一个主要归属者；重排使用稳定 ID；原始模型输出与规范化结果分开保存。

## 4. 类职责与依赖边界

| 模块 | 输入 → 输出 | 负责 | 不负责 |
|---|---|---|---|
| OcrPipeline | DocumentInput → DocumentResult | 调度、错误汇总、取消、资源预算 | 模型专属 resize、解码 |
| PageLoader | 文件/流/像素 → Page | 解码、PDF 渲染、EXIF、页元数据 | 识别与阅读顺序 |
| PageProcessorChain | Page → PreparedPage | 页级方向、去倾斜、去弯曲与坐标变换 | 模型归一化 |
| LayoutDetector | PreparedPage → LayoutBlock[] | Layout 模型前后处理、类别/顺序规范化 | 输出文章内容 |
| RegionPlanner | LayoutBlock[] → RegionPlan | 重叠关系、父子归属、识别粒度、可选合并 | 调用具体推理 SDK |
| RegionRouter | RegionPlan → RegionRequest[] | 任务、模型、策略和依赖分配 | 涂白、CTC、tokenizer |
| RecognitionEngine | RegionRequest → BlockResult | 语义任务执行，选择适配器/复合引擎 | PDF 与最终 Markdown |
| ModelAdapter | 原图区域 → 标准识别结果 | 模型前处理、prompt、输出解码 | 页面语义重建 |
| IInferenceEngine | Tensor/Generation 请求 → 原始结果 | 后端推理、状态和资源 | OCR 业务规则 |
| DocumentAssembler | BlockResult[] → DocumentIR | 块归属、阅读顺序、段落/图表关系 | 再次归一化图片 |
| ResultFormatter | DocumentIR → MD/JSON | 序列化、资源链接、格式转义 | 凭空补全内容 |

原来的 `ResultFormatter` 拆成 `DocumentAssembler + Exporter`，对外可继续保留同名门面。`MnnGlmOcrEngine`可保留为便捷工厂，但内部应组合 `GlmOcrAdapter + MnnGenerationEngine`，避免每新增后端都复制一套模型逻辑。

## 5. 首版完整数据流

```text
输入校验、页范围、资源限制
  → PageLoader：PDF 逐页栅格化 / 图片解码 / EXIF 方向
  → PageProcessorChain：可选页面增强 + TransformChain
  → LayoutAdapter：准备模型输入 → MNN → 模型解码
  → Layout 规范化：坐标还原、类别映射、重复框与父子关系
  → RegionPlanner：内容归属、公式策略、粒度与切片预算
  → RegionRouter：text / formula / table / image / unknown
  → 从 PreparedPage 裁剪原始区域，保留几何映射
  → ModelAdapter：校验输入 → 模型前处理或委托运行时
  → IInferenceEngine：独立区域会话、推理、原始输出
  → ModelAdapter：token/结构解码、输出规范化、坐标回映
  → BlockResult：内容 + 原始输出引用 + 状态 + 来源
  → DocumentAssembler：归属、阅读顺序、段落与关系
  → DocumentIR
  → Markdown + JSON + images + run_manifest
```

主循环不能写 `if backend == MNN then resize`。后端差异收敛到 engine，模型差异收敛到 adapter，文档差异收敛到 assembler。

## 6. 前处理：分层、可选与唯一责任方

### 6.1 四类处理

| 层次 | 示例 | 默认行为 | 记录 |
|---|---|---|---|
| 输入规范化 | EXIF、RGB/RGBA、alpha 合成、PDF 渲染 | 按输入执行 | 源尺寸、页旋转、DPI、颜色格式 |
| 页面增强 | deskew、去透视、去弯曲、降噪 | 默认关闭，按配置启用 | 参数、变换、耗时、预览 |
| 区域几何 | 安全扩边、裁剪、拼块、切片 | 裁剪必需，其余按策略 | 父子 ID、覆盖范围、变换 |
| 模型输入 | resize、pad、normalize、layout、tokenizer | 严格遵从模型包 | 处理版本、owner、shape、dtype |

页面颜色规范建议 RGB8；这只是项目内部格式，MNN 张量或某一模型需要 BGR/NCHW 时由适配器转换。禁止依赖 OpenCV 默认 BGR 隐含传递。

### 6.2 没有前后处理是什么意思

“无前处理”可以表示输入已经满足引擎契约，或运行时内部处理；不表示可以忽略尺寸/颜色检查。“无后处理”可以表示模型直接返回标准文本；此时仍须包装为 BlockResult、检查停止原因、执行输出边界验证。

每个处理步骤声明 `owner = adapter | runtime | graph | none`。执行计划构建时校验：必需步骤恰好由一方执行；缺失、重复、顺序冲突均报 `ContractMismatch`。例如图内已有 mean/std 时，adapter 的 normalize 必须关闭；用 VLM 高层图片接口时，不可默认再送归一化张量。

### 6.3 resize、pad 与小字

尺寸策略由模型清单规定：`fixed / dynamic / bucketed`，同时声明长宽、对齐倍数、最小/最大像素、patch 限制和是否支持动态 batch。不要为全项目统一指定 560×560。

固定输入模型先使用官方等价处理验证基线；拉伸、等比 padding、切片为不同策略，不可默默替换。文字过小或超长区域允许二次切分，但段落、公式、表格各有边界：

- 文本优先按行间空白/段落切分；重叠部分记录坐标和来源，再按几何与文本一致性去重。
- 公式不从运算符中间直接切开，超大公式可以保留图像并标记待审。
- 表格切片必须保留行列、表头与合并单元格关系；首版不支持可靠重建时宁可保留整表/失败占位。
- 每块限制切片数、重试次数和总 token 预算，防止反复细分。

二值化、锐化和 Floyd–Steinberg 抖动默认关闭；以中文细笔画、公式、低对比背景做 A/B 实验，不以视觉锐利代替识别准确率。

### 6.4 页面输入与 UV/YUV 扩展

笔记中的“UV 类”含义未明确。按输入扩展点保留，若指相机 YUV，则 `ImagePlane` 应带 row stride、pixel stride、plane dimensions、色彩范围与矩阵；不可假设 YUV_420_888 是连续 NV21。Android 相机帧应在 ImageProxy 生命周期内复制/转换或明确保留所有权。

首版 PDF 全部栅格化识别，减少双通道复杂性。未来可接原生文字提取，但每一区域必须决定 native_text 或 OCR 的内容归属，避免重复。加密 PDF、损坏页、异常尺寸、透明图片和中文路径均有显式错误。

## 7. 坐标设计

定义五种坐标：`SourcePage`（源图片或 PDF page space）、`RasterPage`（未增强的页面像素）、`PreparedPage`（增强后页面）、`Region`（区域裁剪）、`ModelInput`（模型实际输入）。图像像素坐标原点左上，x 向右、y 向下；矩形采用半开区间 `[x0,y0,x1,y1)`，内部浮点，实际裁剪时 floor/ceil 后钳制。

PDF 源坐标单独声明单位、原点、CropBox、Rotate，不假设与图片坐标一致。PDF renderer 返回其页面到像素的实际映射。

每一步保存 source→target 变换与反向映射。若 `H_RP` 为区域到页面、`H_MR` 为模型到区域，则 `p_page = H_RP * H_MR * p_model`；列向量约定写入公共契约。resize 的实际舍入结果用于 scale，不只使用名义比例。

示例：区域左上角为 `(100,200)`，模型输入前缩放 0.5 并在 x/y 各 pad 10/20；模型点 `(60,70)` 回映为区域 `(100,100)`，页面 `(200,300)`。这个例子必须成为验收用例。

透视变换要映射四角再求外接框；保留多边形，不只存 bbox。非线性去弯曲用网格/稠密逆映射，不能伪装为一个 3×3 矩阵。拼接区域使用分片映射；若 VLM 无精细定位，只返回源区域集合，不能声称每字都有准确坐标。

归一化 `[0,1000)` 的框仅在模型确实输出该格式时解码，明确参考整页还是子图。不得直接把模型输出框文本作为文件路径；资源管理器生成稳定路径。

## 8. Layout 前后处理与区域规划

Layout 模型清单必须包括输入名称/shape/dtype、normalize、resize、输出语义、类别表、阈值、顺序字段和是否已经内置 NMS。已核对 PP-DocLayoutV3-MNN 仓库含模型文件，但 README 没有详细 I/O 契约；RapidDoc V3 的 800×800、1/255、NCHW 前处理可作参考基线，必须经该 MNN 图的输入输出探测与对齐后启用，不能硬套。

处理顺序：原始张量校验 → 模型输出解码 → 去掉 padding/缩放 → PreparedPage 坐标 → 无效框过滤 → 模型特定去重 → 语义归属 → 阅读顺序候选。输出已完成 NMS 时不能不加判断再做一次。

`filter_overlap_boxes`不是简单“所有框做 IoU NMS”：文本包含行内公式、表格包含文字、图片与图注都是合法关系。先按类别和 containment 建图，仅删除同语义重复区域；被抑制块仍记录原因和关联 ID。

优先保留 Layout 模型给出的 `original_order`；缺失、重复、冲突时才进入几何排序。两栏与跨栏标题必须使用分区规则，不能全页按 `(y,x)` 排序。`reading_order_source` 标记 model/geometry/manual，保留原始分数与顺序。

识别前的邻块合并默认关闭。开启时仅合并同栏、同任务、连续顺序且尺寸预算允许的文本块，不跨标题、图片、公式或表格。保留 `source_region_ids`；非连续拼图要声明 region-set 粒度。识别后的段落合并是另一项操作，不共用一个模糊的 `merge_blocks=true` 开关。

## 9. VLM 档的识别策略

### 9.1 路由表

| 规范类别 | Task | 首版处理 |
|---|---|---|
| text/title/list/caption/reference | text | 同一已验证 VLM 的文字任务 |
| display_formula | formula | 同一 VLM 的公式任务，输出裸 LaTeX |
| inline_formula | 按内容归属策略 | 默认由父文本块整体识别；不重复插入 |
| table | table | VLM 表格任务，适配输出格式 |
| image/chart | asset | 保存原图，caption 独立关联 |
| header/footer/page_number | text | 保留结果，导出时可配置隐藏 |
| seal | text 或 asset | 未验证印章能力时保存图像 |
| unknown | asset + warning | 留存内容，避免静默丢弃 |

首版 Ovis 使用转换包 README 所示的统一 Markdown 提取提示词，text/formula/table 路由共享该提示词基线，由下游 parser 按期望内容类型规范化；任务专属提示词只在后续对比验证后启用。GLM 的 `Text Recognition:` 等提示词和 PaddleOCR-VL 的输出协议属于各自 adapter，不用于 Ovis。具体包参数与证据见第 04 文档。

用户提供的整页 DEFAULT_PROMPT 与 Ovis 上游模型卡的提示词相符，首版可建立 Ovis 专用 profile；不得作为所有模型的默认提示词。裁剪后提示词中的“image”指当前子图，区域级调用效果需要实测。资源路径、跨区域阅读顺序和整份文档标题层级最终由组装器管理；模型预测的图片框仅作为可校验的几何证据。

### 9.2 行内公式的内容归属

VLM 档默认 `parent_owns_inline`：父文本区域保留公式原像素，由 VLM 连同文字一起识别；内联公式检测框作为证据/审核信息，不另行输出相同公式。独立行间公式则交给 formula 任务。

如果需要单独纠正行内公式，必须具备明确的插入锚点、定位协议或可验证的占位符；仅有一个父段落字符串与子公式框时无法可靠判断替换位置，不能通过字符串相似性盲替换。高级 split-and-merge 模式另做实验，失败保留原段落与候选公式供审核。

表格内文字/公式默认归表格所有；图注是独立块并通过关系连接。RegionPlanner 在推理前确定这些关系，以避免“公式一次、正文再一次、表格再一次”的重复输出。

### 9.3 会话与输出限制

每个区域使用新的逻辑会话；权重尽可能常驻，但历史消息、KV、图像缓存必须隔离。取消或超时后有不确定状态的实例必须重建或通过验证后的 reset 清理。

限制 max_new_tokens、上下文长度、区域耗时和输出字节数。遇到 max_tokens、重复循环、未闭合表格时标为 `partial`，不能将截断内容当作成功。生成参数与 seed 进入运行清单；不同设备并不承诺逐字节确定性。

## 10. det+rec 档：后续实现的完整位置

`TextDetRecEngine`是复合 RecognitionEngine，内部调用 det、rec；可与 FormulaNet、表格引擎分别配置。它不是一种推理后端。

```text
区域原图 + 区域内公式框
  ├─ 原图副本：供 rec 的文字片段裁剪
  └─ 检测副本：仅遮罩公式区域 → det
       → 概率图解码、框过滤、去 padding、区域坐标
       → 与公式框相交的文字框按行方向切分
       → 从未涂白的原图裁出剩余文字片段
       → rec → 文本 span
公式区域原图 → formula engine → 公式 span
文本 span + 公式 span → 行关系与文档块
```

涂白依据公式几何框，不必等待公式字符串识别结束。遮罩只作用于检测副本；原图不可改。扩边过大可能遮掉相邻汉字，默认几何范围与边界策略须由测试确定。

水平切分只适用于近水平文本；旋转/竖排应转换到文本行局部坐标后切分，或选择不切分并标注。多公式求区间并集后再做差集，不能逐次切框制造重复片段。记录极小片段被移除的原因。

det 后处理可能包含 DB 阈值、轮廓、unclip、框得分；rec 根据实际架构采用 CTC 或相应 decoder。CTC 必须绑定同版本字典、blank ID、重复消除顺序和置信度算法，不将 CTC 套给自回归公式模型。

SLANet 等结构模型还可能需要单元格与 OCR 文本匹配；“结构模型输出”不自动等于内容完整的 HTML。FormulaNet 也不能预设单次图推理即可完成，是否多图/迭代解码由实际 artifact 决定。

## 11. 后处理的三层边界

| 层 | 输入 → 输出 | 示例 |
|---|---|---|
| 模型后处理 | 原始张量/token/字符串 → 语义结果 | NMS/CTC、EOS、表格 token 转换、模型坐标解码 |
| 区域结果规范化 | 模型结果 → BlockResult | 裸 LaTeX、区域坐标、状态、证据、未知置信度 |
| 文档后处理 | BlockResult[] → DocumentIR | 阅读顺序、段落、图注、公式编号、跨页关系 |

后处理为空时使用 Identity，但输入输出类型必须一致。`RawTensor → BlockResult`不能因为配置了 `postprocess=[]` 就隐式强转。

文字只做明确约定的清理，保留换行和原始输出；不自动翻译、改写、全角转半角、删货币符号或全局移除 `$`。公式规范化处理最外层包裹符，不做数学纠错。表格按 `HTML / OTSL / model_tokens`显式选择 parser，失败保留 raw 与原图。

置信度采用 `{value, kind, source}`；区分 detector probability、CTC score、token logprob、启发式质量标记。VLM 未提供校准置信度时为 null；不同模型分数不能统一按 0.8 比较。低置信结果优先保留并标记，硬删除必须留下审计记录。

## 12. 文档重建与格式要求

### 12.1 组装顺序

1. 校验 BlockResult 的 page_id、source_region_ids、坐标、状态。
2. 依据归属表消费内容，避免嵌套区域重复。
3. 按模型顺序/几何分区生成稳定阅读序列；并发完成先后不影响输出顺序。
4. 文本行合并成段落；中文不无条件插空格，英文断词需证据。
5. 连接标题、图注、表注、脚注和公式编号；记录关联依据。
6. 跨页段落/表格合并默认关闭，启用后保留原页块和逻辑合并视图。
7. 生成 DocumentIR，再导出所有格式。

公式编号仅在同行/相邻几何、编号形式、无其他竞争对象时关联；不把相邻题号自动变成公式 `\tag`。标题级别不足以判定时使用保守层级并标记来源。

### 12.2 输出文件

```text
output/<document_id>/
  document.md
  document.json
  run_manifest.json
  images/p0001_<block_id>.png
  debug/                  # 按开关输出
    layout_p0001.png
    transforms.json
    raw_results.jsonl
    processing_trace.jsonl
```

Markdown：UTF-8、相对资源路径、行内 `$...$`、独立公式 `$$...$$`；有合并单元格的表格保留 HTML；简单表格可选 GFM。图片实际路径由 AssetWriter 生成，不能直接采用模型虚构的 `<img src>`。公式与表格渲染能力取决于查看器，MD 本身不保证每个编辑器都能显示 LaTeX。

表格 HTML 采用允许标签/属性集合，清除脚本、事件属性和外部主动资源；只保留表格语义与本地资源。原始 HTML 另存，避免不可逆修改。首版不要求重新排版为原页视觉布局。

区域失败时输出图片占位和简短状态，JSON 保留错误码；空白页标记 blank，区分真正空白与模型异常无输出。全失败、部分失败、取消是三种不同文档状态。

JSON 是权威中间结果，至少包括 `schema_version`、文档来源、页面、几何变换、块、span、关系、阅读顺序、错误、模型来源及人工修订。bbox 必须带 coordinate_space；字符级 bbox 不存在时不能伪造。

## 13. 资源、调度与性能

Windows 首版每个 VLM 实例串行；Layout、图像加载可用独立有界队列。不要默认同一个 MNN LLM 对象线程安全。模型池按 artifact hash、backend、device、precision 和配置哈希区分实例。

预算包括：页面像素内存、区域缓存、模型权重、KV cache、运行时 workspace 与资源编码峰值。页面流式处理，区域按需裁剪；避免所有区域同时保留 RGB、mask、tensor 三份副本。

设备策略区分 `requested_device` 与 `actual_device`。CPU 是基线，GPU/NPU 是逐包逐设备验证的优化。模型小不等于 NPU 可跑；不可把某设备 decode tok/s 当成端到端页耗时。

记录 load/render/preprocess/layout/crop/vision/prefill/decode/postprocess/assemble/export 时间，统计冷启动、热启动 p50/p95、峰值 RSS/设备内存、输出 token 数和每页区域数。尚无硬件目标，不设未经确认的固定秒数 SLA。

## 14. 编辑、PDF 与错题扩展

编辑采用 `original_content + edits + effective_content`，以 block_id/节点 ID 为目标；重跑模型不覆盖用户修改。错题接口接收 DocumentIR 和原图证据，输出 `QuestionGroup{question_id, block_ids, relation_type, confidence}`，与 RegionRouter 的模型路由分开。

PDF 导出作为 DocumentIR 的新 Exporter；需区分重排 PDF 与保留原页背景/文字层的 PDF，两者不是同一个功能。首版预留接口，不把“生成 MD”描述成已经支持 PDF 编辑与排版。

---

# 统一推理引擎与 Windows / Android 接口

## 1. 设计决策

业务统一调用 `RecognitionEngine::recognize()`，底层统一调用 `IInferenceEngine::execute()`。请求是带类型的联合体，允许张量推理和多模态生成。运行时暴露能力清单，适配器在创建执行计划时检查能力，不靠运行时异常猜测支持情况。

```text
OcrPipeline
  → ILayoutDetector / IRecognitionEngine          语义任务
    → ModelAdapter + ProcessingPlan              模型协议
      → IInferenceEngine                         统一生命周期与执行
        ├─ MnnTensorEngine                       Layout / det / rec
        ├─ MnnGenerationEngine                   VLM
        ├─ LiteRtTensorEngine                    后续
        ├─ LiteRtLmGenerationEngine              后续
        └─ OrtTensorEngine / VendorEngine        后续
```

MNN/LiteRT 是运行时，GLM/Ovis 是模型，CPU/GPU/NPU 是执行设备，text/table/formula 是任务；配置和代码必须区分这四个维度。将所有对象都命名为“Engine”会掩盖职责，因此公共头文件使用明确类型。

本文 C++ 代码是契约草图，`Result<T>`、`Json`、`Tensor`等项目类型需要实现；不是可直接编译的完整 SDK。建议 C++17 + CMake，避免在公共接口中使用 C++20/23 专有类型。

## 2. 公共数据契约

### 2.1 图像与张量

```cpp
enum class PixelFormat { Gray8, RGB8, BGR8, RGBA8, BGRA8, YUV420 };
enum class MemoryDomain { Host, Device };
enum class CoordinateSpace { SourcePage, RasterPage, PreparedPage, Region, ModelInput };

struct ImagePlane {
    const uint8_t* data;
    size_t byte_size;
    int width, height;
    size_t row_stride_bytes, pixel_stride_bytes;
};
struct ImageBuffer {
    int width, height;
    PixelFormat format;
    std::vector<ImagePlane> planes;
    std::shared_ptr<const BufferOwner> owner;
    ColorMetadata color;
};
struct TensorSpec {
    std::string name;
    DataType dtype;
    std::vector<int64_t> shape; // 动态维度由 ShapeConstraints 另行约束
    TensorLayout layout;       // NCHW/NHWC/NC4HW4/opaque 等
    QuantizationSpec quant;
};
struct Tensor {
    TensorSpec spec;
    MemoryDomain domain;
    BufferHandle storage;
};
```

所有 buffer 都带所有权；视图不得比其 owner 活得更久。首版后端交界使用 host buffer，优化到零拷贝需另行定义 fence、设备上下文和生命周期。非连续图像需按 stride 处理，不可直接假设 `width*channels`。opaque GPU buffer 不能当作 CPU 地址读取。

量化输入必须知道 scale/zero_point、per-tensor/per-channel 和轴；FP32 归一化不能直接写入 INT8 buffer。MNN 内部通道打包在 adapter/runtime 边界转换，不污染 DocumentIR。

### 2.2 页面、区域与结果

```cpp
struct LayoutBlock {
    Id id, page_id;
    std::string label, original_label;
    Polygon polygon;
    CoordinateSpace space;
    std::optional<float> detection_score;
    std::optional<int> reading_order;
    std::optional<Id> parent_id;
};
struct RegionRequest {
    Id request_id, page_id;
    std::vector<Id> source_region_ids;
    TaskKind task;
    std::shared_ptr<const ImageBuffer> original_crop;
    TransformChain geometry;
    std::vector<RegionRelation> relations;
    std::vector<Polygon> formula_masks;
    ContentOwnership ownership;
    ModelProfileId model_profile;
    RequestLimits limits;
};
struct BlockResult {
    Id request_id, page_id;
    std::vector<Id> source_region_ids;
    ResultStatus status; // ok / partial / failed / skipped
    std::variant<TextContent, FormulaContent, TableContent, AssetContent> content;
    std::vector<Span> spans;
    GeometryEvidence geometry;
    std::optional<Confidence> confidence;
    Provenance provenance;
    std::vector<Diagnostic> diagnostics;
    std::optional<RawResultRef> raw_result;
};
```

`masked_crop_img`不要求 Router 预先创建；RegionRequest 携带公式几何与不可变原图，`TextDetRecEngine`按需生成遮罩副本，避免 VLM 路径产生无用图片。若兼容旧接口，可提供懒加载视图。

Span 几何粒度显式声明 `character / word / line / region / region_set`。VLM 整块输出仅有 region 几何时，不得给每个字符复制整个区域 bbox 假装定位。

TableContent 包含格式、原始结构、可选 TableIR 和规范化 HTML。模型只返回 HTML 而没有单元格坐标时，单元格 bbox 为 null。FormulaContent 保存裸 LaTeX 与 inline/display 语义，Exporter 负责包裹符。

## 3. 统一底层接口

```cpp
enum class RequestKind { Tensor, Generation };
enum class StatePolicy { Stateless, FreshPerRequest };
enum class CancelGranularity { None, BetweenRuns, BetweenTokens };

struct EngineCapabilities {
    std::set<RequestKind> request_kinds;
    std::set<DeviceKind> available_devices;
    bool supports_dynamic_shape;
    bool supports_batch;
    bool supports_image_input;
    bool supports_streaming;
    bool supports_logprobs;
    bool isolated_sessions;
    int max_concurrent_requests;
    CancelGranularity cancel_granularity;
    IoContract io_contract;
    std::vector<ProcessingOwnership> processing_ownership;
};

struct TensorRequest {
    std::vector<Tensor> inputs;
    std::vector<std::string> requested_outputs;
};
struct GenerationRequest {
    std::vector<PromptPart> parts; // TextPart / ImagePart，模型模板不在主循环
    PromptProfileId prompt_profile;
    GenerationOptions options;
    StatePolicy state_policy = StatePolicy::FreshPerRequest;
};
struct InferenceRequest {
    Id request_id;
    std::variant<TensorRequest, GenerationRequest> payload;
};
struct GenerationOutput {
    std::string utf8_text;
    std::vector<int32_t> token_ids; // 后端不提供时为空，能力表明确
    std::optional<TokenScores> scores;
    FinishReason finish_reason;
};
struct InferenceResponse {
    Id request_id;
    std::variant<TensorOutputs, GenerationOutput> payload;
    RuntimeMetrics metrics;
    ActualExecution execution;
};

class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;
    virtual Result<EngineCapabilities> load(const ModelArtifact&,
                                            const EngineOptions&) = 0;
    virtual const EngineCapabilities& capabilities() const = 0;
    virtual Result<void> warmup(const WarmupSpec&) = 0;
    virtual Result<InferenceResponse> execute(const InferenceRequest&,
                                              ExecutionContext&) = 0;
    virtual Result<void> reset(ResetScope) = 0;
    virtual Result<void> unload() = 0;
};
```

`ExecutionContext`至少包含 CancellationToken、deadline、内存/输出预算与 EventSink；调用方拥有整个同步调用期间的生命周期。execute 在引擎工作线程执行，事件进入有界队列，UI 不在此线程直接运行。

加载与创建可由 `EngineFactory::create(backend_id)`分开完成；Registry 只在初始化期注册。未知后端返回 UnsupportedBackend，未知请求类型返回 UnsupportedCapability，不能静默把 Tensor 请求转换成字符串。

适配器可以输出 TensorRequest，也可以输出 GenerationRequest。对高层 VLM 接口，只传原始图片和受支持的 prompt parts，由运行时完成 tokenizer/图像处理；对低层图接口，适配器负责准备张量。具体 owner 由绑定后的能力和模型清单共同决定。

### 3.1 语义层接口

```cpp
class IRecognitionEngine {
public:
    virtual ~IRecognitionEngine() = default;
    virtual bool supports(TaskKind) const = 0;
    virtual Result<BlockResult> recognize(const RegionRequest&,
                                          ExecutionContext&) = 0;
};
class ILayoutDetector {
public:
    virtual ~ILayoutDetector() = default;
    virtual Result<std::vector<LayoutBlock>> detect(const PreparedPage&,
                                                    ExecutionContext&) = 0;
};
```

Layout 不勉强塞入只返回 BlockResult 的接口，但其内部复用相同 IInferenceEngine 与处理机制。`VlmRecognitionEngine`组合一个模型 adapter 与 generation engine；`TextDetRecEngine`组合 det、rec 和 span merge；`AssetEngine`直接保存图片，不需要加载推理后端。

## 4. 模型适配器与处理链

```cpp
class IModelAdapter {
public:
    virtual ~IModelAdapter() = default;
    virtual Result<ExecutionPlan> bind(const ModelManifest&,
                                       const EngineCapabilities&) = 0;
    virtual Result<PreparedRequest> prepare(const RegionRequest&,
                                             ProcessingContext&) = 0;
    virtual Result<BlockResult> decode(const InferenceResponse&,
                                        const PreparedRequest&,
                                        ProcessingContext&) = 0;
};
```

PreparedRequest 除 InferenceRequest 外，必须携带输入来源、处理日志、geometry、tokenizer/prompt 版本等解码所需上下文。不能把最近一次裁剪信息存在共享 adapter 可变字段里，导致并发结果串页。

ProcessingStep 声明：输入/输出 DataKind、step_id、版本、参数、执行方、是否改变几何、是否可逆、是否改变内容。PlanBuilder 验证步骤类型连通、顺序、必需项与 ownership。

| 场景 | prepare | execute | decode |
|---|---|---|---|
| Layout 输出 raw tensor | resize/normalize/tensor | Tensor engine | decode/框/顺序/回映 |
| MNN VLM 高层图片输入 | 选任务与构造多模态输入 | runtime 内部视觉处理与生成 | 解析文本/公式/表格 |
| 已有标准 Tensor | Identity + spec 校验 | Tensor engine | 对应模型 decoder |
| 标准文本输出无特殊格式 | 图片/prompt 处理 | Generation engine | IdentityContent + Block 包装 |
| 两侧都无额外处理 | 输入契约完全匹配 | 相应引擎 | 输出契约完全匹配才允许 Identity |

“禁用前处理”不是一个无条件生效的总开关。请求跳过必需 normalize 而图内/runtime 也没有时必须拒绝。页面去倾斜关闭则无需报错，因为它是可选质量操作。

### 4.1 模型包清单

下例是**待核验模板，默认不能运行**。空值必须在探测模型后填写，生产加载不得默认猜测。

```yaml
schema_version: 1
id: doclayout_v3_mnn_local
validation_status: pending
model_family: pp_doclayout_v3
artifact:
  backend: mnn_tensor
  revision: null
  files: []                     # 每项 path、size、sha256、role
  license: null
runtime:
  tested_version: null
  tested_platforms: []
io:
  inputs: []                    # name、dtype、shape、layout、quantization
  outputs: []                   # name、dtype、shape、semantic
  size_policy: null
  labels_file: null
processing:
  color_convert: {owner: null, target: null}
  resize: {owner: null, mode: null, size: null}
  normalize: {owner: null, mean: null, std: null}
  decoder: {owner: adapter, id: null, version: null}
generation: null               # VLM 则填 tokenizer/template/stop IDs/context
```

VLM 清单额外记录视觉编码器、projector、decoder、权重分片、tokenizer、chat template、special tokens、图像尺寸/patch 约束、stop tokens、上下文预算、任务提示词和输出协议。清单同时支持“运行时负责”与“图中融合”的处理声明。

模型文件加载前检查相对路径和哈希；导出路径与包路径分开。运行清单记录 artifact hash、SDK 版本、操作系统、设备/驱动、线程数、实际后端、配置 hash。模型包升级不能悄悄覆盖旧 profile。

## 5. MNN 适配

### 5.1 张量网络

依据 [MNN Session API](https://mnn-docs.readthedocs.io/en/latest/inference/session.html)，MnnTensorEngine 内部封装 Interpreter/Session、输入输出 tensor 和 host copy。生命周期大致为创建 Interpreter → 创建 Session → 查询 I/O → 必要时 resize 输入并 resizeSession → 拷贝输入 → runSession → 拷贝输出。

实现时从实际模型查询名字、shape、dtype，不预设 `input`/`output`。动态 shape 更改会影响内存规划，应使用 shape cache/bucket 或串行 session；输出不能借用下一次运行即失效的内存而让上层长期保存。

模型内置 NMS、检测框已反变换等属于模型契约，不能由 MNN 后端猜测。模型 package 相同，在 CPU/GPU 上应该共用相同 adapter。

### 5.2 多模态生成

本次读取的 [MNN llm.hpp](https://github.com/alibaba/MNN/blob/master/transformers/llm/engine/include/llm/llm.hpp)包含 `createLLM`、`load`、`reset`、多模态 `response`、`MultimodalPrompt`及状态/耗时字段。该事实只证明接口存在，不证明给定 GLM/Ovis 转换包可用。

MnnGenerationEngine 使用专用 RAII deleter 管理 Llm（按锁定版本的 destroy 约定），封装 SDK 的图像 VARP/模板，不向业务层暴露。每请求获得独占实例，开始前清空状态，结束后检查停止原因；发生取消/错误时先等待底层结束，再 reset 或销毁。

接口草图中的 `execute`不是 MNN 原生函数；实现映射到选定版本的多模态 response/generate 路径。图像数据是原始 RGB 还是预处理 VARP、维度布局如何，应查验实际多模态实现并以输入对齐用例验证，不能只由 `PromptImagePart`字段推断。

SDK 有 reset 不等于所有模型缓存都已经验证清空；必须做 A/B 会话隔离测试。若运行时未提供安全的中途取消，只能宣告 BetweenRuns，UI 可标记取消中，但必须等推理返回后才能释放资源。

编译参考 [MNN LLM 文档](https://mnn-docs.readthedocs.io/en/latest/transformers/llm.html)。锁定 SDK commit 与编译选项；普通 MNN 推理库不等于已包含 LLM/多模态支持。Windows x64、Android arm64 分别建立构建和模型验证记录。

## 6. LiteRT / LiteRT-LM / ORT 扩展

### 6.1 LiteRT Tensor

LiteRtTensorEngine 适配普通张量图，参考 [CompiledModel C++ API](https://developers.google.com/edge/litert/next/cpp)。封装环境、模型、CompiledModel、buffer 与执行；accelerator 和 shape 能力由构建及设备协商，不直接把一个 bool `use_npu`当作可用性证明。

### 6.2 LiteRT-LM Generation

[官方 C++ 文档](https://developers.google.cn/edge/litert-lm/cpp)将重量级 Engine 与有状态 Conversation 分开，提供 SendMessage/SendMessageAsync，支持由运行时处理多模态输入。适配器复用 Engine，每个区域创建新的 Conversation，输出映射为 GenerationOutput。

任务 prompt 与模型模板分开；若 runtime 已应用模板，外层不能再应用一遍。高层接口接收图像路径时，adapter 用受控临时目录或可验证的内存接口，记录磁盘开销；不假设所有 SDK 版本都提供相同内存输入 API。

用户链接的 PaddleOCR-VL 转换包在模型卡中声明静态 560×560，表格输出含结构 token；应通过对应 parser 转 TableIR/HTML，不把 token 文本直接当作 HTML。其约束仅属于该包，清单锁定包 revision 后再用于实现。

### 6.3 ORT 与厂商后端

OrtTensorEngine 可用于参考对齐和未来运行，不是首版必需的默认依赖。厂商 NPU/DSP engine 实现相同接口及能力清单；delegate 部分回退 CPU 时报告实际执行情况，若 SDK 无法提供详细分区信息则标为 unknown，而非宣称全 NPU。

### 6.4 vision encoder 与 decoder 混合后端

首版不拆。后续可用 `CompositeGenerationEngine`实现 LiteRT vision + MNN decoder，但必须验证 image embeddings 的 shape、dtype、归一化、projector、特殊 token 插入位置、图像 patch 顺序、position IDs/M-RoPE、attention mask 和 KV 协议。

使用同一原始 checkpoint 的视觉模块与 decoder；不能仅凭“底座同为 ERNIE-0.3B”替换权重。跨运行时桥接先用 CPU tensor 对齐，再讨论零拷贝。验收包括 embedding/logit 对齐、单区域文本、整页质量与传输开销；任一失败则保留单后端路线。

## 7. 生命周期、并发、错误

状态机：`Unloaded → Loading → Ready → Running → Ready`；失败可进入 Faulted；取消进入 Cancelling，底层真正停止后才回 Ready/Unloaded。load/unload/reset 不得与同实例 execute 并发。

首版一个模型实例最大并发 1；在上层调度多个区域时排队。实例池只在内存预算允许、状态隔离验证通过后扩容。模型权重共享不自动意味着 KV、图像缓存和 stream 可共享。

| 错误码 | 含义 | 默认处理 |
|---|---|---|
| InvalidInput / UnsupportedFormat | 文件或像素不合法 | 文档/页面失败 |
| ArtifactMissing / HashMismatch | 包不完整或版本不符 | 启动拒绝该模型 |
| ContractMismatch | 处理/I/O 契约不一致 | 配置失败，禁止猜测 |
| UnsupportedCapability | 请求/设备不支持 | 按显式 fallback 策略选择，否则失败 |
| OutOfMemory | 内存不足 | 降低并发/切片重试一次，记录变化 |
| Timeout / Cancelled | 超时/用户取消 | 停止调度，等待正在执行的后端安全结束 |
| InvalidOutput / Truncated | 解码/内容不完整 | 保留 raw 与原图，partial |
| BackendFailure | SDK 错误 | 隔离实例，受控重建 |

默认不无限重试，不自动换云端。备用模型重试需独立 request_id/attempt_id，成功选择一个主要结果，不把两次结果直接拼接。

## 8. 跨平台工程结构

```text
include/dococr/          # 无 SDK/JNI 类型的公共 C++ 头文件
src/core/               # 调度、模型注册、错误、结果
src/geometry/           # 坐标空间、变换链
src/processing/         # 图像处理、类型校验、PlanBuilder
src/models/             # layout/glm/ovis/paddle_vl/det_rec 适配器
src/backends/mnn/       # tensor、generation
src/backends/litert/    # 后续
src/document/           # IR、归属、阅读顺序、组装
src/export/             # MD、JSON、AssetWriter
src/platform/           # 文件/流、PDF renderer、平台服务
bindings/c/             # 稳定 C ABI
bindings/android/       # JNI + Kotlin 封装
apps/windows_cli/       # 首版入口
tests/fixtures/         # 小样本、golden、fake backend
models/manifests/       # 模型清单，无权重入仓
```

建议 CMake targets：`dococr_core`、`dococr_backend_mnn`、`dococr_c`、`dococr_cli`，Android 再加 `dococr_jni`。以 `DOCOCR_WITH_MNN/LITERT/ORT`配置选项隔离依赖；不把多个后端全部强制链接到基础库。

Windows 路径边界转为系统文件 API 所需编码，核心文本统一 UTF-8；不能把中文路径经本地 ANSI 编码传递。PDF 可通过 IPdfRenderer 使用 PDFium 等可移植实现，具体库版本、构建体积和许可在集成时确定。

## 9. C ABI：Windows 与 JNI 共用入口

DLL/so 边界不传 STL、异常、MNN Tensor。使用不透明 handle、定长数值、明确长度的 UTF-8 与成对释放函数。以下为拟议 ABI：

```c
typedef uint64_t DocOcrHandle;
typedef uint64_t DocOcrJob;
typedef struct { const char* data; size_t size; } DocOcrStringView;
typedef struct { uint8_t* data; size_t size; } DocOcrBytes;

DocOcrStatus dococr_create(DocOcrStringView config, DocOcrHandle* out);
DocOcrStatus dococr_capabilities(DocOcrHandle, DocOcrBytes* out_json);
DocOcrStatus dococr_job_create(DocOcrHandle, DocOcrJob* out);
DocOcrStatus dococr_job_run(DocOcrJob, const DocOcrInput*, DocOcrBytes* out_json);
DocOcrStatus dococr_job_cancel(DocOcrJob);
DocOcrStatus dococr_job_poll_events(DocOcrJob, DocOcrBytes* out_json);
DocOcrStatus dococr_job_destroy(DocOcrJob);
DocOcrStatus dococr_destroy(DocOcrHandle);
void dococr_bytes_free(DocOcrBytes*);
```

`dococr_job_run`是阻塞调用，调用者放在工作线程；job 先创建再执行，因此其他线程能准确取消。handle 使用带代数/有效性检查的 registry，不将裸指针直接当公开句柄。destroy 对运行中对象返回 Busy，或使用显式 cancel+wait API；不能一边推理一边释放。

所有导出函数捕获 C++ 异常并转 status；错误详情附 request_id、stage、code 与 message。内存由创建它的库释放，避免 Windows CRT 边界问题。ABI 应带版本查询与 struct_size，后续追加字段不破坏旧调用方。

## 10. JNI 与 Kotlin

JNI 的职责只有参数/所有权转换、调用 C ABI、错误映射。首选 Kotlin → JNI → C++ core → C++ backend，不在 Java 层重新实现一套模型前后处理。若未来某厂商只有 Java SDK，再通过 Android 专用桥接适配，线程回调成本与双向调用单独测试。

```kotlin
internal object NativeDocOcr {
    external fun create(configUtf8: ByteArray): Long
    external fun capabilities(handle: Long): ByteArray
    external fun createJob(handle: Long): Long
    external fun runFile(job: Long, pathUtf8: ByteArray): ByteArray
    external fun runPixels(
        job: Long, pixels: java.nio.ByteBuffer,
        width: Int, height: Int, rowStride: Int, pixelFormat: Int
    ): ByteArray
    external fun cancel(job: Long)
    external fun pollEvents(job: Long): ByteArray
    external fun closeJob(job: Long)
    external fun close(handle: Long)
}
```

上层 `DocumentOcr : AutoCloseable`管理 handle，使用后台 dispatcher/线程池执行阻塞 run；取消处理必须能从另一线程调用 native cancel，不能等阻塞调用返回后才发取消。UI 通过 Flow/事件轮询观察页/块进度，事件批量传递，不逐 token 高频穿 JNI。

参数与结果选择 ByteArray 承载标准 UTF-8 JSON，Kotlin 显式解码，避免把任意 OCR UTF-8 当作 JNI Modified UTF-8。大文档可返回输出目录与摘要，用分页接口读取 JSON；不能无限堆积单个 Java byte[]。

JNI 实现约束，依据 [Android JNI 官方建议](https://developer.android.com/ndk/guides/jni-tips)：

- `JNIEnv*`只在当前线程使用；确需 native 回调时通过 JavaVM 获取/附加线程并配对 detach。
- 跨调用保存 Java 对象用 global reference 并释放；局部引用不能逃逸调用。
- DirectByteBuffer 校验容量、stride 和溢出；首版同步调用期间借用，异步队列必须复制或持有全局引用直至完成。
- 不在推理期间长期持有 critical array pin；不要在 UI 线程执行推理。
- C++ 异常不能越过 JNI；检查 pending Java exception，按统一错误结构转换。

Bitmap 输入可锁定像素后复制为核心格式，立即解锁；处理 alpha 和 stride。Android `content://` URI 不是普通路径：由 Kotlin 打开 ParcelFileDescriptor 后通过专用 fd 入口或复制到应用缓存；FD 所有权与 dup/close 规则必须明确。

模型安装在应用可读且适合大文件访问的目录；不要假设压缩 assets 可直接作为 mmap 模型路径。导出通过 IOutputSink 适配本地目录/Android SAF，不能让核心依赖 Activity。

## 11. 平台迁移验收

Windows 先用 CPU 跑相同模型与固定样本，建立结果和处理 trace；Android CPU 对齐，再逐步启用 GPU/NPU。检测框、输入张量数值误差和识别质量分别比较，不要求跨硬件浮点结果 bitwise 一致。

最低验收包括：arm64 构建、中文路径/URI、DirectByteBuffer、取消、连续多页、销毁竞争、旋转切后台、模型初始化失败、内存不足、UTF-8 含补充平面字符、无 JNI 泄漏。Android 目标芯片、最低系统版本和内存阈值尚待设备选型，不阻碍公共架构定稿。

---

# 实施任务、验收与参考依据

## 1. 决策记录

| ID | 决策 | 理由 |
|---|---|---|
| ADR-01 | Windows 首版，Android 后续 | 用户已明确；公共核心从第一阶段不依赖平台 UI |
| ADR-02 | MNN PP-DocLayoutV3 + OvisOCR2 为 P0 | 用户已明确路线与具体模型，GLM 仅后续扩展 |
| ADR-03 | 语义引擎与推理后端分层 | 模型/任务的前后处理不随 SDK 重复实现 |
| ADR-04 | Tensor/Generation 使用带类型的统一请求 | 满足统一调用而保留真实能力差异 |
| ADR-05 | 处理步骤声明 adapter/runtime/graph/none | 防止无处理、重复处理和隐含处理混淆 |
| ADR-06 | DocumentIR 为输出依据 | 同时支持 MD/JSON、编辑、错题与后续 PDF |
| ADR-07 | VLM 行内公式由父文本拥有 | 没有精细定位时避免重复与错误插入 |
| ADR-08 | 每区域 fresh session | 避免上下文污染，保留权重常驻优化空间 |
| ADR-09 | C API 稳定边界，JNI 薄桥接 | Windows/Android 共用核心，减少 SDK 类型泄露 |
| ADR-10 | CPU 基线后再验证加速 | 精度、算子覆盖、设备适配分别验证 |

## 2. 面向实现 Agent 的总指令

先读本目录三份设计文档，再检查目标工程已有代码与 AGENTS.md。如果用户提供现有推理引擎，先列出与本文的接口映射、可复用点和缺失项，不另建一套平行引擎。没有代码时按建议目录建工程。

当前任务是文档设计，本文任务列表供后续实施。不要将本文示例视为已经存在的文件、模型清单或已通过的测试。不要虚构 MNN 模型输入尺寸、tensor 名称、tokenizer、模型支持列表、成功耗时和转换精度。

实现顺序必须先打通一套已验证模型的最小路径，再增添后端。调度层禁止判断 SDK 类型；所有图片变换必须进入 geometry/trace；所有被过滤内容必须有记录。PR/交付报告应包含实现范围、测试证据、未验证平台和具体模型 revision。

## 3. 分阶段任务与完成定义

### T0：模型包与运行时探测

输入：用户提供的模型链接、目标 Windows 环境。交付：可复现 `model_probe`报告与 ModelManifest。

1. 固定 MNN 版本/commit 和编译选项，验证普通 tensor 与多模态能力。
2. 获取 PP-DocLayoutV3-MNN 文件清单、hash、许可证、输入输出信息；确认是否有阅读顺序、多边形、内置后处理。
3. 验证已选定的 `dr3334/ovrics-ocrv2_mnn`：文件清单/README/配置已经通过 API 核对，继续完成模型加载、shape 与识别对齐。
4. GLM 不属于首版探测任务；后续启用时再核验转换包，不能把原始 safetensors 直接交给 MNN。
5. 使用文字、公式、表格三种小图验证 VLM，固定 prompt/template/stop IDs，保存 raw 输出。
6. 比较原框架/作者基线的模型输入与识别结果，完成默认 Ovis 的接入验证；失败时报告具体问题，不自动改用 GLM。

完成标准：至少一个 Layout MNN 包与一个 VLM MNN 包可离线执行，约束和后处理已明确；若未达到，报告精确缺口，不以空壳 pipeline 宣称全 MNN 可用。

### T1：核心契约与无模型贯通

交付：ImageBuffer、Geometry、错误与事件、DocumentIR、Registry、C++ 接口及 fake backend。

测试可用 fake Layout/VLM 输出跑通图片→DocumentIR→MD/JSON，验证空处理链、schema、顺序、资源命名、失败占位。fake 只验证编排，不作为真实模型成功证据。

完成标准：core 不包含 MNN/LiteRT/JNI 头文件；输入输出与内存所有权明确；无模型测试可独立编译运行。

### T2：Windows MNN Layout

交付：MnnTensorEngine、LayoutAdapter、模型 probe 命令、版面标注图。

先对齐输入张量，再对齐 raw 输出，再对齐坐标/类别/顺序。不要在检测框不准时先改全局阈值掩盖归一化或坐标错误。

完成标准：单栏、双栏、旋转输入和长宽比极端样本可定位；未知类/空框/重叠父子有确定处理；模型 CPU 基线对齐报告存在。

### T3：Windows MNN VLM

交付：MnnGenerationEngine、首个模型 adapter、三种任务解析器。

先实现单区域识别，再接路由；每区域独立状态。MNN 普通 tensor 与 LLM engine 共享日志/错误契约，不能以同一个低层 runSession 硬模拟所有生成语义。

完成标准：文本、公式、表格能进入相应 Content；无效输出/截断可区分；会话隔离、连续多次运行与取消后重用测试通过。

### T4：整页/多页编排与导出

交付：RegionPlanner、RegionRouter、Assembler、PDF PageLoader、MD/JSON/资源。

完成标准：混合页面没有重复行内公式/表格文字，双栏顺序正确；并发完成顺序不影响导出；局部失败不丢页；所有图像引用存在；可从 JSON 重新导出等价 Markdown。

到这里才算首版 P0 完成。编辑器、错题分类和 PDF 再排版不纳入该完成声明。

### T5：Android JNI

交付：Android arm64 构建、C ABI、JNI/Kotlin wrapper、最小调用示例。

以同模型同样本先比 Windows/Android CPU；再验证设备加速。覆盖 URI、像素 stride、前后台切换、取消、Close 与运行竞争、输出目录访问。

完成标准：不改 OcrPipeline 即可调用；Kotlin UI 不执行阻塞推理；核心业务无 Android 类依赖；真实设备连续文档测试无悬挂句柄/泄漏。具体设备性能另行确认。

### T6：后端与引擎扩展

依次接入 LiteRT-LM、传统 det+rec、必要时 LiteRT Tensor/ORT；不要求同时实现所有后端。增加后端只新增 backend 实现和 factory 注册；增加模型只新增 adapter/manifest及必要 parser。

完成标准：至少一个第二后端能使用同一 RegionRequest/BlockResult 契约；含外部处理/内部处理的样本均正确；主循环无需修改。若模型输出协议不同，可增加 adapter profile，不能把差异散落到主循环。

## 4. 建议配置

以下是早期结构简例，不是已验证的运行配置；其中默认识别 profile 已明确为 Ovis。更完整且优先采用的配置在 `configs/pipeline.yaml`，决策语义见第 04 文档。

```yaml
schema_version: 1
pipeline:
  mode: layout_vlm
  on_region_error: keep_image_and_continue
  inline_formula_policy: parent_owns_inline
  pre_recognition_merge: false
  cross_page_merge: false
page:
  pdf_dpi: 200                  # 开发基线，可调整；不是模型硬约束
  canonical_color: rgb8
  processors: []               # 页级可选增强
layout:
  profile: doclayout_v3_mnn_local
recognition:
  default_profile: ovis_ocr2_mnn
  session_policy: fresh_per_region
routes:
  text: {task: text, profile: ovis_ocr2_mnn}
  title: {task: text, profile: ovis_ocr2_mnn}
  display_formula: {task: formula, profile: ovis_ocr2_mnn}
  table: {task: table, profile: ovis_ocr2_mnn}
  image: {task: asset}
  chart: {task: asset}
  unknown: {task: asset, warning: true}
execution:
  requested_device: cpu
  allow_device_fallback: false
  vlm_instances: 1
  max_inflight_pages: 1
  max_region_retries: 1
exports:
  formats: [markdown, json]
  preserve_raw: true
  include_assets: true
  debug_artifacts: false
```

token 数、最大页像素、超时、切片数、内存上限为必需部署参数，依据设备与模型清单填入，缺失时启动校验提示；它们不能隐藏在源码常量中。示例为简明省略这些值，不意味着可无限制运行。

## 5. DocumentIR 示例

以下仅展示关键字段；最终需提供 versioned JSON Schema 和迁移规则。

```json
{
  "schema_version": "1.0",
  "document_id": "doc-example",
  "status": "ok",
  "source": {"type": "image", "name": "example.png"},
  "pages": [{
    "page_id": "p0001",
    "page_index": 0,
    "raster_size": [1200, 1600],
    "transforms": [],
    "reading_order": ["b0001", "b0002"],
    "blocks": [{
      "id": "b0001",
      "type": "text",
      "source_region_ids": ["r0001"],
      "bbox": [100, 200, 900, 400],
      "coordinate_space": "raster_page",
      "geometry_granularity": "region",
      "status": "ok",
      "content": {"format": "markdown", "text": "设 $x>0$，求函数的最小值。"},
      "confidence": null,
      "reading_order_source": "layout_model",
      "provenance": {"model_profile": "ovis_ocr2_mnn", "request_id": "req001"},
      "edits": []
    }, {
      "id": "b0002",
      "type": "formula",
      "source_region_ids": ["r0002"],
      "bbox": [200, 450, 800, 550],
      "coordinate_space": "raster_page",
      "geometry_granularity": "region",
      "status": "ok",
      "content": {"format": "latex", "text": "f(x)=x+\\frac{1}{x}", "display": true},
      "confidence": null,
      "provenance": {"model_profile": "ovis_ocr2_mnn", "request_id": "req002"},
      "edits": []
    }],
    "relations": [],
    "diagnostics": []
  }]
}
```

配套 Markdown：

```markdown
设 $x>0$，求函数的最小值。

$$
f(x)=x+\frac{1}{x}
$$
```

schema 应约束：同页 ID 唯一、reading_order 引用存在、关系端点存在、坐标单位可解释、模型来源可回溯、失败块有错误。几何范围的跨字段检查通过业务 validator 完成，不能全部依赖基础 JSON Schema。

## 6. 验收用例矩阵

| ID | 场景 | 必须观察到的结果 |
|---|---|---|
| AT-01 | 输入无需前处理、输出无需特殊解析 | Identity 路径生效，仍校验契约并包装结果 |
| AT-02 | graph 内 normalize + adapter 又 normalize | PlanBuilder 拒绝重复执行 |
| AT-03 | 必需后处理被关闭，raw tensor 无法成为内容 | ContractMismatch，而非空成功 |
| AT-04 | runtime 自带图像处理/tokenizer | 外层不重复 resize/模板/tokenize |
| AT-05 | BGR/RGB、RGBA、非连续行 | 色彩测试图与基线一致，alpha 背景处理可追踪 |
| AT-06 | 裁剪→缩放→padding | 文档中的回映例返回页面点 (200,300) |
| AT-07 | 90°旋转/透视/非线性去弯曲 | 保留对应映射类型，标注不漂移 |
| AT-08 | 文本包含行内公式 | VLM 默认父块拥有，只导出一次 |
| AT-09 | 表格内文字和公式 | 不作为表外重复正文输出 |
| AT-10 | det+rec 遮罩（P1） | 原图 hash 不变，det 用 mask、rec 用原图 |
| AT-11 | 多个公式横穿一行（P1） | 文本切分覆盖正确，无重复和漏掉相邻字 |
| AT-12 | 双栏、跨栏标题、图注 | 阅读序列符合标注，非简单 y 排序 |
| AT-13 | 多区域以乱序完成 | 导出顺序仍确定 |
| AT-14 | VLM A→B 与独立 B | 无 A 内容污染；固定配置下满足隔离判据 |
| AT-15 | max tokens / 循环输出 / 坏表格 | partial/error，保留 raw 与原图 |
| AT-16 | VLM 无 confidence | JSON null，不填 0.95/1.0 |
| AT-17 | 识别文字含 $、反斜杠、HTML、中文 | 不全局误删，导出转义保持原义 |
| AT-18 | 图片/资源导出 | MD 引用存在、相对路径有效、无跨页命名冲突 |
| AT-19 | 某页损坏/某块失败 | 其他页完成，失败位置明确 |
| AT-20 | 空白页 vs 模型无输出 | 区分 blank 与识别失败，不静默等同 |
| AT-21 | 取消/timeout 后下一任务 | 无残留状态，不释放仍运行中的对象 |
| AT-22 | GPU 请求未满足 | 失败或按策略降级，actual_device 可见 |
| AT-23 | JSON→MD 重新导出 | 不依赖运行模型，资源与内容一致 |
| AT-24 | JNI DirectBuffer 容量不足/坏 stride | InvalidInput，无越界 |
| AT-25 | Android URI、后台调用、Close 竞争 | 无 UI 卡死、无 UAF、资源释放确定 |
| AT-26 | UTF-8 含 emoji/罕见汉字 | 经 JNI 往返不损坏 |
| AT-27 | 缺 tokenizer/vision 权重/字典错误 | 初始化失败，指出缺失项 |
| AT-28 | 替换后端并保留业务请求 | OcrPipeline 不修改，适配能力差异可解释 |

AT-01～09、12～23、27 是 P0；AT-10/11 属 det+rec 阶段，AT-24～26 属 Android 阶段，AT-28 属第二后端阶段。不要把后续阶段完成作为 P0 前置条件。

## 7. 质量与性能评测

建议初始人工回归集至少覆盖 30 页：单栏中英文 6、双栏/论文 5、行内公式密集 5、独立公式 3、复杂表格 5、扫描/旋转/低对比 4、空白/异常 2。这个数量是工程建议，不能代替大规模精度结论。

将转换对齐集、调参集和最终评测集分开。文本使用 CER/归一化编辑距离；表格使用结构指标与单元格内容指标；公式同时检查表示一致性与渲染/语义可比指标；阅读顺序按块 ID 序列评估。记录未识别和失败样本，不能只评成功输出。

OmniDocBench 可用作后续综合评测，锁定数据版本、评测脚本与评测配置，不能将模型卡上的分数当作本系统实测。首版先建立 float/原框架基线，再量化或启用加速。

验收阈值分两类：功能不变量要求全部通过；精度/性能容忍度由 T0 基线和目标设备确定并冻结。报告包含 CER 差异、公式/表格指标、失败率、单页 p50/p95、冷启动、峰值内存、模型体积与实际设备；尚未获得硬件要求时不要写“每页 1 秒”等承诺。

## 8. RapidDoc 源码映射

已取得并阅读 RapidAI/RapidDoc 的源码；本次克隆的 commit 为 `60cd038d424e0e839462ba4bd96345e0279290fe`。以下永久链接指向该快照。前期对 main 的单文件读取也用于定位，实施应以固定快照和回归样本为准。

| RapidDoc 文件/符号 | 已观察到的职责 | C++ 对应设计 |
|---|---|---|
| [pipeline_analyze.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/pipeline_analyze.py)：doc_analyze / batch_image_analyze | PDF/图片批次组织与模型调度 | PageLoader + OcrPipeline |
| [model_init.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/model_init.py)：AtomModelSingleton | 模型构建/复用、自定义模型入口 | Registry + 有界 ModelPool |
| [batch_analyze.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/batch_analyze.py)：BatchAnalyze | Layout、公式、custom/traditional OCR、表格分支 | 固定主流程 + 可配置 route |
| [backend/utils/utils.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/utils/utils.py)：filter_overlap_boxes | 按识别模式处理重叠区域 | RegionPlanner 的归属/去重策略 |
| [analyze_utils.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/analyze_utils.py)：_apply_mask_boxes_to_image / _run_ocr_det_batch | 原图与遮罩图分离、分辨率分组、文字框修正 | TextDetRecEngine 内部 DAG |
| [ocr_utils.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/utils/ocr_utils.py)：update_det_boxes / get_ocr_result_list | 公式相交文字框处理与识别片段构造 | 几何切分 + Span 构造 |
| [rapid_layout.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/layout/rapid_layout.py) | 类别映射、original_order 保留 | LayoutAdapter + canonical label map |
| [CustomBaseModel](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/custom/__init__.py) | 以 batch_predict 抽象识别模型 | IRecognitionEngine；新增错误、geometry、来源契约 |
| [paddleocr_vl.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/custom/paddleocr_vl/paddleocr_vl.py) | 按 text/formula/table 提示、表格格式转换 | PromptProfile + ModelDecoder |
| [model_json_to_middle_json.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/model_json_to_middle_json.py) | span 过滤/填入块、公式编号、中间结构 | DocumentAssembler + IR |
| [pipeline_middle_json_mkcontent.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/backend/pipeline/pipeline_middle_json_mkcontent.py) | 块转 Markdown、段落与图片/公式导出 | MarkdownExporter |

本项目是参考算法与编排进行 C++ 设计，不承诺 Python 实现可以直接链接复用。优先复用类别表、算法思想与输入/输出样例；OpenCV 图像操作可等价重写；模型运行时、所有权、线程模型和 JNI 需要原生实现。若复制具体源码，应保留适用许可证和归属信息。

差异需明确：RapidDoc 中可见 VLM span 默认置信度和部分全局字符替换等处理，本设计改为未知置信度与结构化清理；这些是本项目的设计选择，不是对上游逐行复刻。笔记提到的 CropByBoxes/merge_blocks 不假定是上述快照的同名核心接口，以实际调用链为准。

## 9. 外部参考与核验状态

| 来源 | 核验情况 | 对设计的影响 |
|---|---|---|
| [RapidDoc 仓库](https://github.com/RapidAI/RapidDoc) | README、主要编排/后处理源码已读，本地快照已固定 | 采用区域识别→中间结构→导出的组织 |
| [PP-StructureV3 文档](https://www.paddleocr.ai/main/version3.x/pipeline_usage/PP-StructureV3.html) | 已访问 | 模块化 pipeline 参考，不能当成固定端侧配置 |
| [PaddleX 配置](https://github.com/PaddlePaddle/PaddleX/blob/release/3.7/paddlex/configs/pipelines/PP-StructureV3.yaml) | 已访问 | 模块开关和组合配置参考 |
| [PaddleOCR TextRecognition](https://www.paddleocr.ai/v3.6.0/version3.x/module_usage/text_recognition.html) | 已核对 rec_text/rec_score | 校正 det 与 rec 职责 |
| [MNN llm.hpp](https://github.com/alibaba/MNN/blob/master/transformers/llm/engine/include/llm/llm.hpp) | 已读取头文件，未编译执行 | 生成与多模态接口、reset、状态封装 |
| [MNN Session](https://mnn-docs.readthedocs.io/en/latest/inference/session.html) / [LLM](https://mnn-docs.readthedocs.io/en/latest/transformers/llm.html) | 已访问官方文档 | 张量/LLM 两类后端适配与构建 |
| [LiteRT C++](https://developers.google.com/edge/litert/next/cpp) | 已访问官方文档 | Tensor 后端扩展 |
| [LiteRT-LM C++](https://developers.google.cn/edge/litert-lm/cpp) / [Conversation 头文件](https://github.com/google-ai-edge/LiteRT-LM/blob/main/runtime/conversation/conversation.h) | 已访问接口资料 | Engine/Conversation 分离、独立区域会话 |
| [Android JNI](https://developer.android.com/ndk/guides/jni-tips) | 已读取官方建议 | JNI 线程、引用、编码和边界设计 |
| [GLM-OCR 配置](https://github.com/zai-org/GLM-OCR/blob/main/glmocr/config.yaml) | 已读取 | 模型任务提示词与类别映射参考 |
| [PP-DocLayoutV3-MNN](https://modelscope.cn/models/dr3334/PP-DocLayoutV3-mnn) | 网页工具失败后已通过 API 核对文件清单及 README；未运行权重 | 已选定；具体图 I/O/精度仍需探测 |
| [OvisOCR2 MNN 包](https://modelscope.cn/models/dr3334/ovrics-ocrv2_mnn) | 已通过 API 核对文件清单、README、config/llm_config/export_args | 已选定；配置与归一化线索明确，实际运行待验证 |
| [GLM-OCR ModelScope](https://modelscope.cn/models/ZhipuAI/GLM-OCR/summary) | 网页工具无法读取；官方 GitHub资料可读 | 不能认定此链接是已转换 MNN 包 |
| [PaddleOCR-VL LiteRT 包](https://huggingface.co/litert-community/PaddleOCR-VL-1.6) | 已读取模型卡，未运行权重 | 静态尺寸、任务协议和独立会话仅按该包解释 |
| [hf-to-litertlm/paddleocr_work](https://github.com/john-rocky/hf-to-litertlm/tree/main/paddleocr_work) | 本次页面读取失败，未核对转换源码 | 混合拆分仅列实验，未声称方案验证成功 |

链接中的 main/master 内容会变化。除 RapidDoc 外，其余 SDK 与模型在实现 T0 中还需固定 revision/hash。模型卡声称的性能与精度属于发布方报告，不是本项目测试结果。

保留原调研中的补充资源：[PaddleOCR 中文说明](https://github.com/PaddlePaddle/PaddleOCR/blob/main/readme/README_cn.md)、[chineseocr_lite](https://github.com/DayBreak-u/chineseocr_lite)、[Paddle Lite opt 文档](https://www.paddlepaddle.org.cn/lite/v2.12/user_guides/opt/opt_bin.html)、[ERNIE 权重检索线索](https://www.modelscope.cn/search?search=ERNIE-4.5-0.3B-PT)。本轮未据此建立首版运行时或性能结论。

## 10. 风险与下一次实施所需信息

| 项目 | 当前状态 | 下一动作 |
|---|---|---|
| 已有引擎代码/接口 | 尚未提供 | 获得工程后做适配映射，避免重复开发 |
| 默认 VLM | 已确定 OvisOCR2-MNN | T0 验证运行与区域任务表现，不再重复选型 |
| 模型分辨率与量化 | 已读取 Ovis 配置与 4-bit 导出参数 | 继续 probe 实际动态图与数值；Layout I/O 仍需核验 |
| Windows/Android 硬件预算 | 未指定 | 实施前选参考设备，记录内存/延迟目标 |
| Android minSdk/ABI | 本设计建议 arm64，版本未定 | 依据 SDK 与设备确定 |
| 首批业务文档 | 未提供 | 提供脱敏题目/论文/表格，建立优先回归集 |
| UV 接口含义 | 未明确 | 当前保留一般输入处理扩展，YUV 为条件性设计 |
| 模型许可与依赖清单 | 未逐包核验 | 随模型清单与发行包登记 |

以上不阻碍需求与架构文档交付；它们是后续真实接入与性能验收的输入。最终发布不可把 pending 的模型/设备标为已支持。


---

# 确定模型、能力管理与前后处理决策

版本 1.1，2026-09-26。本补充将首版模型与配置语义具体化；与早期“候选模型”描述冲突时以本节为准。

## 1. 已明确的两个模型

| 用途 | 仓库 | 已核对的证据 |
|---|---|---|
| 页面版面检测 | [dr3334/PP-DocLayoutV3-mnn](https://modelscope.cn/models/dr3334/PP-DocLayoutV3-mnn) | 包含 `PP-DocLayoutV3.mnn`，大小 130,568,736 bytes；文件 revision `c67c1a858d5f6c855172d4cfdf931798dafa2edd` |
| 区域识别 | [dr3334/ovrics-ocrv2_mnn](https://modelscope.cn/models/dr3334/ovrics-ocrv2_mnn) | README 明确为 ATH-MaaS/OvisOCR2 的 MNN 导出；有 LLM/vision 图与权重、tokenizer、配置；核心文件 revision `20f12e49d846941e67829a7a7c3645693e485942` |

Ovis 仓库名称就是 `ovrics-ocrv2_mnn`，不擅自按产品拼写改 URL。README 后续更新 revision 为 `07c6809c5492628aa8cfcc6588e6fd5ee333d3f9`，引用文档与权重版本要区分。

上轮网页读取失败，本轮改用 ModelScope 文件 API 后已取得资料；不能再把两个仓库统称为“无法核对”。本轮只下载小型说明/配置文件，未运行权重，未验证整页/裁剪区域识别质量。

Ovis 包的重要文件：`config.json`、`llm_config.json`、`llm.mnn`、`llm.mnn.weight`、`visual.mnn`、`visual.mnn.weight`、`tokenizer.mtok`、`export_args.json`。文件存在说明包结构具备必要组成，不等于 SDK 兼容性、精度和性能已通过。

## 2. 实际配置能确认什么

从 [Ovis llm_config.json](https://modelscope.cn/models/dr3334/ovrics-ocrv2_mnn/resolve/master/llm_config.json)读取：

| 字段 | 实际值 | 设计解释 |
|---|---|---|
| model_type | qwen3_5 | MNN 模型适配路径线索 |
| is_visual / is_mrope | true / true | 需要视觉处理与正确位置编码 |
| image_mean | [127.5, 127.5, 127.5] | 像素预处理参数，禁止外层重复应用 |
| image_norm | [0.00784313725490196, ...] | 每通道约 1/127.5 |
| image_size | 420 | 配置值，不独立证明固定 420×420 |
| image_size_unit | 32 | 对齐声明，仍需结合实际视觉图/运行时 |
| image_min_pixels | 65536 | 配置中的最小像素预算 |
| image_max_pixels | 16777216 | 配置中的最大像素预算，不是移动端资源承诺 |
| max_position_embeddings | 262144 | 结构配置，不承诺设备可分配如此大的上下文 |
| jinja / eos | 内置模板 / `<\|im_end\|>` | 使用包内协议，不重复套模板 |

从 [export_args.json](https://modelscope.cn/models/dr3334/ovrics-ocrv2_mnn/resolve/master/export_args.json)确认语言模型导出 `quant_bit=4`、`quant_block=128`，visual 量化选项为 null；不由此断言视觉权重也是同一种 4-bit 量化。

从 [config.json](https://modelscope.cn/models/dr3334/ovrics-ocrv2_mnn/resolve/master/config.json)确认 CPU/4 线程、mixed sampling、temperature 0.8 等包默认值。工程建议另建可复现 OCR profile（例如 greedy），通过 SDK 对应选项映射并实测；不直接覆盖原始包文件，记录 override 与实际生效参数。

读取的 [MNN omni.cpp](https://github.com/alibaba/MNN/blob/master/transformers/llm/engine/src/omni.cpp)存在依据视觉图输入和像素预算进行动态 resize、归一化与 patch 构造的路径。因此 `image_size=420`不等于“必须外层 resize 到 420×420”。最终使用的分支、颜色输入约定和 shape 支持必须对锁定 SDK + 该模型包联合验证。

Layout 仓库的 README 是通用说明，`configuration.json`只有任务标签，不含完整预处理协议。RapidDoc 的 V3 参考实现是 resize 到 800×800、float32、乘 1/255、NCHW、携带 scale_factor；这是**待迁移的参考 profile**。需要确认该 MNN 图是否已包含其中某些操作、输出框是否已回到原图坐标、阅读顺序的输出方式。

参考源码：[V3 handler](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/layout/rapid_layout_self/model_handler/pp_doclayout/main.py)、[pre_process.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/layout/rapid_layout_self/model_handler/pp_doclayout/pre_process.py)、[post_process.py](https://github.com/RapidAI/RapidDoc/blob/60cd038d424e0e839462ba4bd96345e0279290fe/rapid_doc/model/layout/rapid_layout_self/model_handler/pp_doclayout/post_process.py)。

## 3. 配置管理什么

“技能”在这里是流水线能力，如 layout.detect、ocr.transcribe、formula.normalize、table.parse、document.assemble，不是 Codex 的 SKILL.md。

配置分四层管理，初期可以放在一个 YAML，规模增长后拆文件：

1. **模型包**：仓库、revision、hash、文件、I/O、包内前后处理、模板。
2. **能力绑定**：哪种任务由哪个 model/adapter/backend 提供，输入输出类型是什么。
3. **流水线**：节点依赖、任务路由、处理开关、条件、阈值、失败策略。
4. **平台 profile**：Windows/Android 设备、线程数、内存、并发、token/像素预算。

配置引用 `processor_id`、`adapter_id`和 `engine_id`，由 C++ Registry 找到实现。配置可以组合已实现能力，不能凭填写一个字符串创造新算子或让一个不支持表格的模型自动支持表格。

每项能力分开记录 `declared`（包声称支持）、`contract_verified`（I/O/实现核对）、`quality_validated`（数据验证）、`enabled`（当前流水线启用）。enabled 不等于 verified。生产启动要求必要的契约验证完成；研发模式允许带 pending 质量标记实验，但仍不得绕过 I/O 和所有权校验。

## 4. 首版能力绑定

| 业务能力 | 模型执行 | 处理方式 |
|---|---|---|
| layout.detect | PP-DocLayoutV3-MNN Tensor | LayoutAdapter 准备张量并解码 |
| text.recognize | OvisOCR2-MNN Generation | 统一转写 prompt → Markdown/text parser |
| formula.recognize | 同一 Ovis 实例池 | 相同 prompt 基线 → 公式结果规范化 |
| table.recognize | 同一 Ovis 实例池 | 相同 prompt 基线 → HTML/表格结构校验 |
| image.preserve | 无模型 | 从原图导出资源 |
| document.assemble | 无模型 | 归属、阅读顺序、段落与关系 |

这三种识别能力可以是同一个底层 `ocr.transcribe`能力的业务视图。模型卡/包 README 使用统一 Markdown 提取指令，不需要为了三个标签加载三份 Ovis 权重，也不默认复制 GLM 的 task prompts。

用户给出的 DEFAULT_PROMPT 与 [Ovis 官方模型卡](https://huggingface.co/ATH-MaaS/OvisOCR2)一致。首版采用包 README 的简化转写 prompt 基线，图/图表由 Layout 路由导出；官方带 bbox 的 profile 作为可选项。若启用 bbox profile，其坐标以当前输入子图为参照，再经 TransformChain 回映，不直接信任模型生成的资源文件名。

如果公式任务输出一段混合 Markdown，parser 应返回结构不符合预期或保留混合片段，不能无条件把整段当裸 LaTeX。表格未出现合法 table 时保留 raw、返回 partial，并按配置保留图片。裁剪区域是本项目组合方式，质量需单独验证，不能引用整页模型分数代替。

## 5. 如何决定执行前后处理

决策分为启动期与运行期，不能让 LLM 临时猜测是否 resize。

### 5.1 启动期：生成执行计划

```text
读取 YAML → Schema 校验 → 解析版本/路径/模型文件
  → ModelManifest + EngineCapabilities + Registry 类型契约
  → 校验任务可路由、DAG 无环、输入输出类型连接
  → 每个必需处理步骤确定唯一 owner
  → 校验模型约束与资源预算相容
  → 输出不可变 ExecutionPlan + effective_config + config_hash
```

mandatory 是模型/类型协议要求，enable 是使用策略，两者不是一个开关。模型 resize 即使每次是 identity，也必须经过契约节点确认。

| 情况 | 决策 |
|---|---|
| 模型必须 normalize，graph/runtime 无此能力 | adapter 执行；禁用则启动失败 |
| 模型必须 normalize，runtime 已执行 | adapter 不执行，trace 标记 delegated/runtime |
| graph 已融合 normalize | adapter/runtime 不再执行，owner=graph |
| 输入已经是契约要求的 tensor | 验证 provenance/spec 后 Identity；不是看 shape 相同就跳过 |
| 可选去倾斜关闭 | 跳过，不影响必需的模型 resize |
| 可选处理设 auto，且有 detector 与阈值 | 运行期评估确定性条件 |
| 配置 auto 但没有可用的条件检测器 | 启动失败；不静默假装自动判断 |
| 原始输出是 tensor，后处理为空 | 类型不连通，启动失败 |
| 输出已是标准 Markdown fragment | 额外内容转换可为空；状态/格式/来源校验仍执行 |

### 5.2 运行期：按输入元数据决定可选步骤

顺序：检查 enabled → 检查步骤是否适用于当前 task/region → 执行条件探测 → 满足阈值则处理 → 更新类型和几何 → 记录原因。

条件使用注册的 predicate 与类型化参数，例如 `abs(skew_angle)>threshold && confidence>=threshold`，不在 YAML 中执行任意 Python/C++ 表达式。阈值是可配置策略，默认值需在样本集验证。

每节点产出 `executed / skipped_disabled / skipped_predicate / delegated_runtime / provided_by_graph / identity_validated / failed`。trace 包含输入尺寸、输出尺寸、owner、条件测量、参数 hash 和变换 ID，才能解释一页为什么被处理而另一页没有。

## 6. 对当前两个模型的具体决策

| 步骤 | 当前决策 | 依据 |
|---|---|---|
| PDF 渲染/EXIF/颜色解码 | 按文件元数据执行 | 输入规范，非识别模型开关 |
| 页面去倾斜/去弯曲/二值化/锐化 | 首版默认关闭 | 避免未经测量改变文档像素 |
| Layout resize/scale/NCHW | 必需 profile；先验证 MNN 图再启用 | RapidDoc 参考协议，MNN 包未给完整 I/O |
| Layout 输出解析/类别映射 | 必需 | tensor 不能直接成为 Region |
| Layout NMS/坐标恢复 | 根据已核验输出协议启用 | 防止双 NMS、双反变换 |
| 区域裁剪 | 必需，边界钳制与映射 | Ovis 接收每个区域原图 |
| 检测图涂白/公式切框 | 此 VLM 档关闭 | 属于传统 det+rec 内部流程 |
| Ovis resize/normalize/patch/tokenizer/template | 首版设计委托 MNN 多模态路径 | adapter 只封装符合该 API 的图像与 prompt；运行时能力必须验证 |
| Ovis 外层额外 normalize | 关闭 | 避免重复应用包内 mean/norm |
| Ovis 新会话/reset | 每区域必需 | 内容隔离 |
| Ovis 文本/公式/表格规范化 | 按 route 的期望内容类型执行 | 同一个模型输出需要不同检查 |
| 文档组装、资源导出 | 必需 | 模型输出片段不是完整文档结构 |

关于颜色：核心 RGB8 与某个 MNN 图片接口的 BGR 约定可能不同。Backend adapter 负责边界转换；delegate 图像处理不表示所有接口都直接接受 RGB8。锁定 SDK 后使用彩色测试图验证通道顺序。

## 7. 配置示例与加载约束

完整示例见 [configs/pipeline.yaml](configs/pipeline.yaml)。这是本项目拟议 schema，不是 MNN 原生 config.json，也不是已经实现的配置加载器。`contract_status: pending_probe`会阻止 production 执行，避免示例中的参考预处理被误认为已验证。

配置合并规则：模型包提供硬约束；model profile 提供默认协议；pipeline 提供路由与可选处理；platform 提供设备预算；单次请求仅能覆盖 allowlist（页范围、导出格式等）。哈希/revision、必需处理、输出语义不允许任意 request 覆盖。数组默认整体替换，依 ID 合并须显式定义；未知字段默认拒绝。

修改 pipeline 或 platform 后生成新的 immutable plan。已运行任务继续使用旧快照，新任务使用新 plan；卸载模型等操作等待实例空闲。相同 profile 的 text/formula/table 共享权重池，但不共享可变会话状态。

## 8. 下一步实现应补的模块

在原设计上明确增加 `ConfigLoader`、`ConfigValidator`、`CapabilityRegistry`、`PlanBuilder`、`ConditionEvaluator`、`ProcessingTrace`。它们使 YAML 真正成为可检查的能力编排入口，而不是散落 bool 开关的存储文件。

新增验收：未知 processor 失败；能力未绑定失败；重复 normalize 失败；required 被关闭失败；auto 缺 detector 失败；相同配置同输入元数据生成同一计划；每个跳过步骤有原因；热更不影响正在运行任务；Ovis 三条路由只占一个模型池配置；生产拒绝未验证 Layout I/O。

