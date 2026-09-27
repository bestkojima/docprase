# #15 冻结评测规则（运行前）

基线 `dba124ea2203a06c9afe822aab06948f9c97600a`；固定 CPU、1 线程、`configs/printed-page.example.json`、每区域 512 token、PDF 200 DPI。文件 `samples.json` 和本规则先提交再运行；不因输出调整样本、阈值或匹配方式。已有外部样本均在 #2～#14 用于对齐/开发，不能称为独立留出集；新自制双栏页只检验本次未调参输入，也不是公开独立 benchmark。统计母体仅七页，异常案例独立列出；约 30 页是后续扩充建议。

## 功能规则

每个真实作业由生产 CLI 或公共 C ABI 提交。成功导出时按其 `schema_version` 验证 Schema，所有 `resources[].path`、诊断 overlay、mask、raw tensor 必须是作业内存在的普通文件且无越界路径。每页块 ID 唯一，`reading_order` 是这些 ID 的无重复全排列。每个块的 `source_region_ids` 可解引用，其页级整数 `bbox` 落在原图或 PDF 栅格内；有 `content_owned_by` 的子版面框不得再作为独立内容块，且所有关系端点存在。对图片输入，用 RGB 原图裁剪与每块裁剪 PNG 做像素精确比较；PDF 用导出的栅格尺寸和页到 PDF 的仿射元数据复核。每个作业生产 `--reexport` 后，JSON 和 Markdown 原始字节及全部声明资源逐字节相同。Markdown 中引用的本地资产都必须存在；IR 诊断和正文裁剪不要求全在 Markdown 展示。

作业 `partial`、区域 `partial` 或 `failed` 不计为识别质量通过，必须列出 ID、原因、`stop_reason` 与 raw 证据。CLI 退出零仅证明作业完成与导出。加密、损坏、错误模型哈希和非法配置须有非零退出码及明确错误。真实取消仅承诺当前不可中断推理调用返回后的安全停止和恢复。

## 质量规则

所有质量数字同时报告分母、匹配与未匹配样本，不用一个已对齐正文块代表整页。文本保留 raw；只去首尾空白及将内部连续 Unicode 空白视作一个空格后计算精确匹配与 Levenshtein 字符错误率 `CER=编辑距离/参考字符数`，不翻译、不改标点。对 PDF 自制十行逐行比较；未找到的参考行按删除计入，额外输出另列。旧教材正文用原有固定摘录单块比较，其余文字块只列状态和 raw，不虚构人工真值。JEE 页人工可见锚点为 `JEE (Advanced) 2023`、`Q.12`、`Q.13`、`Time (h)`；锚点按大小写完全相同的子串核对，不用它们计算整页 CER。

新双栏页的人工参考按图像从左栏到右栏、再跨栏第二节顺序：`Printed Science Practice - Form A`；`Section 1: Measurement`；`1. Measure the length of a pencil.`；`Record the answer in centimeters.`；`2. A meter has one hundred`；`centimeters. Convert 2 m to cm.`；`3. Explain why repeated readings`；`can reduce random error.`；`4. The chart shows two readings.`；`Which reading is larger?`；`Figure 1. Recorded readings.`；`5. Write the result as a sentence.`；`Include the unit in your answer.`；`Section 2: Short answer`；`6. State one reason to check a measurement twice.`；`End of practice page.`。逐锚点报告是否在对应文字块 raw 中出现，不因模型分块而改变；缺失显式列出。阅读顺序用这些可唯一定位锚点的成对先后，报告正确对/可判定对；未识别锚点不进入可判定分母，但单列为遗漏，不能计为正确。左栏应先于右栏；跨栏标题应先于两栏，而第二节应在两栏后。插图应保留原图裁剪及 `Figure 1` 图注，关系若未建立单列失败。

公式以 OmniDocBench 已固定 `formula_book_page.manifest.json` 中人工 LaTeX 摘录为参考，定位框与参考框交叠面积 / 参考面积超过 `0.8` 才配对。只忽略空白并去掉一次外层 `$$`；`\\alpha` 与 `a`、不同命令或符号均不等价。行内公式验归属且只输出一次，独立公式同时列格式、状态和内容精确匹配；未对上的参考、局部不完整和截断逐条列出。

表格以 `manifest.json` 和 `merged_table_book_page.manifest.json` 中的人工 HTML 为参考，框交叠面积 / 参考面积超过 `0.5` 才配对。HTML 解析后分别比较行列数、每格行列坐标与 rowspan/colspan，以及单元格原文字；文本精确值只将连续空白折叠为单空格，不将 TeX 表记改写为等价。参考标注中数学 `<` 在**参考解析侧**按既有 #9 脚本转义，原文件不改。未匹配表格、格、图片单元和 partial 均列失败。顺序的固定编排夹具另外用 `reading_order_integration` 验证，不冒充真实模型检测精度。

## 性能规则

每个 CLI 新进程的 `/usr/bin/time -v` 记录从进程启动到退出的墙钟时间、User/System CPU 与 `Maximum resident set size`（KiB）；它是 Linux `wait4` 报告的**该命令及受等待子进程的最大 RSS**，不是加总的并发进程树峰值，也不证明 OS 页缓存冷。每次新进程加载模型，因此可称进程/模型冷启动；不清理页缓存。`run-manifest.json` 记录 decode/layout/recognition/export 阶段和 PDF 逐页 renderer/pipeline/total 毫秒。图片单页时延以外层墙钟为准，PDF 单页时延按清单逐页总毫秒，明示不同计时边界。一次运行是本机冒烟，不设性能门槛或优化承诺。
