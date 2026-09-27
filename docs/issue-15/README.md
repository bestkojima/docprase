# #15：试卷教材 Linux 综合验收

## 结论和边界

Linux CPU 上已用生产 PP-DocLayoutV3 + OvisOCR2 对 6 份输入、7 个真实页执行公共作业；旧样本曾用于对齐或开发，`en_two_column` 是本轮新写、未用于调参的项目自制页。这是小规模固定回归集，不是约 30 页的独立公开 benchmark。每份作业的原始 JSON、Markdown、清单和关键日志在 [`evidence/`](evidence/)；归档含 124 份轻量文件的 SHA 校验及首轮 275 项、修复重跑 31 项资产的 SHA 索引。完整导出资源在本机 `output/issue-15/`，可按下文重建。样本 SHA、来源和角色见 [`samples.json`](samples.json)，比较与容差在首轮运行前冻结于 [`rules.md`](rules.md) 和 [`measurement-addendum.md`](measurement-addendum.md)。

**显著质量失败：** HiLEx JEE 640×640 英文试卷页的 7/7 文字块均 `partial/token_limit`，raw 反复生成 `## 1`、`## 2` 等编号；人工可见的四个锚点 `JEE (Advanced) 2023`、`Q.12`、`Q.13`、`Time (h)` 全部缺失。两处图像块保留，但这页英文 OCR 不通过。现有配置没有对该质量失败自动回退或修复，不因 CLI 返回 0、资源存在而声称其文字正确。完整 raw、状态与框见 [`en_jee/document.json`](evidence/en_jee/document.json)、[`en_jee/run-manifest.json`](evidence/en_jee/run-manifest.json)。

新双栏页首轮可判定顺序对仅 72/105；检测框已含页眉和分节标题，是确定性几何回退的产品缺陷。先在公共 CLI fixture 边界写出失败测试并修复通用分节判断，再用**同一张原图、同一模型和同一冻结指标**重跑：顺序 105/105，15/16 锚点识别，缺图注仍 `partial/token_limit`。首轮与修复后证据并存，详见 [`order-fix.md`](order-fix.md)。逐块 raw、状态、框未变；31 个资产在首轮、修复作业与再次导出间字节一致。

## 八项验收定位

| #15 条件 | 实际证据及判定 |
| --- | --- |
| 1. 业务回归集 | 3 张 OmniDocBench 中文教材图、1 张 HiLEx JEE 英文试卷图、项目自制 2 页中文 PDF、项目自制 1 张双栏英文试卷图，共 7 页；含行内/独立公式、普通/合并表、图、单/双栏、跨栏/分节标题、多页。加密 PDF、坏图及故障另计案例。未扩写为 30 页。 |
| 2. 来源、人工参考、角色、冻结 | `samples.json` 列来源、SHA、对齐/开发/本轮未调参角色；`rules.md` 定文本 CER、LaTeX 仅忽略空白、表结构与格文字分开、人工锚点成对顺序。旧 OmniDocBench/HiLEx 不能作为留出集。全部 raw、partial 和缺锚点保留于 `quality.json` 与作业 JSON。 |
| 3. 公开入口、不变量、定位、再导出 | 6 份输入均走生产 CLI，完成后用 Schema、页/块/区域/归属引用、页内 bbox、原图裁剪、资源路径、诊断 tensor/mask/overlay、Markdown 引用及生产 `--reexport` 字节比较。初轮全部硬性不变量通过；PDF 页内 34 项资产经补充复核。生产 C ABI 同引擎连续作业另有原始结果。 |
| 4. Linux 实际覆盖 | PDF 两页 200 DPI、公式页、两张表页、JEE 与双栏阅读顺序、中文路径（#14 同基线原证据）、同引擎连续两次与真实取消恢复。Linux、g++、glibc、MNN、Poppler、模型 SHA 在 `evidence/`。固定推理 fixture 验确定性编排/失败边界，不能代表真实识别质量。 |
| 5. 质量、时延、内存 | 下表与 `quality.json`、`performance.json`：文本、公式、表格、顺序分别给分母及错误；`/usr/bin/time -v`、清单阶段、PDF 逐页时延和公共引擎创建时间分开。单次本机冒烟无速度承诺。 |
| 6. 契约状态 | 生产配置的 9 个工件 SHA 与运行清单比对；9 个拒绝案例覆盖未知处理器、重复/禁用/责任错误处理、哈希错误、未验证状态、超预算 token、加密 PDF、坏图。运行清单保存实际 `MNN/3.6.1` CPU 后端、单线程、处理责任、每区域 stop_reason。`contract_verified` 不等于业务质量通过。 |
| 7. 复现/通过/失败/限制 | 本文命令与各原始证据可复核。功能不变量和重导出通过；JEE 英文、图注及部分公式/表格/细小文字质量失败明确保留。手写、复杂拍照、Android、第二后端、性能优化未纳入通过条件。 |
| 8. 完成范围 | Linux 实际证据与上述功能验收给总控审核；父 #1 原始 Windows 文字未修改、未关闭。 |

## 质量明细

| 输入 | 原始文档状态 | 冻结比较结果 | 明确失败或边界 |
| --- | --- | --- | --- |
| `zh_text_table` 中文教材 | `partial`，14 `ok` / 5 `partial` | 人工正文摘录 1/1 精确；普通表结构 5×3、15 格一致，格文字 14/15 精确 | 4 块 token 截断；1 公式语法不完整；首格 `序号`/`序 号` 差空格；非整页 CER。 |
| `zh_formula` 中文教材 | `partial`，18 `ok` / 4 `partial` | 17 条行内归属；人工独立公式 1/3 在仅忽略空白下精确 | 一条把 `\alpha` 误为 `a`；一条混合公式 `invalid_formula_syntax`；另有 2 块 token 截断、1 文字块行内公式语法不完整。 |
| `zh_merged_table` 中文教材 | `partial`，7 `ok` / 5 `partial` | 主表结构 10×4、26 格一致，文字 8/26 精确；次表结构 2×4、8 格一致，文字 6/8 精确 | 主要为数学 TeX 表记差异；3 块 token 截断、2 块行内公式语法不完整。 |
| `en_jee` 英文试卷 | `partial`，7 `partial` / 2 图像 `skipped` | 可见锚点 0/4，成对顺序无可判定对 | 7 文字块全 token 截断且 raw 重复编号；实际识别失败，图像仅保留裁剪。 |
| `zh_pdf` 自制两页 | `ok`，10/10 块 `ok` | 冻结 raw 规则 8/10 行精确，编辑距离 6/225 字符，CER 2.67% | 两标题各多 `## ` Markdown 前缀；按既有展示文字剥离标题格式可得 10/10，但不能替代冻结 raw 指标。 |
| `en_two_column` 自制英文试卷 | `partial`，10 `ok` / 1 `partial` / 1 图像 `skipped` | 首轮 15/16 锚点、72/105 顺序对；排序修复后仍 15/16、105/105 | `Figure 1. Recorded readings.` 未识别，图注 raw 达 token 上限；图块存在且 `caption_of` 关系存在。首轮阅读顺序失败单独留档。 |

总计 7 页，人工参考只涵盖固定摘录、指定表格/公式、PDF 逐行及英文锚点；没有对其余全部文字虚构真值。`quality.json` 逐块列 25 个非 `ok` 状态和 stop_reason，并列公式、表格及锚点的原文差异；所有 raw 在相应 `document.json` 中。不能由单个摘录正确推断整页 OCR 精度。新双栏页整页 raw 串联的补充诊断编辑距离为 791/493，插入的重复图注使 CER 可大于 100%；锚点与成对顺序是更可解释的局部指标。

原图裁剪对 PNG 做逐像素精确比较；对 JPEG 按运行前补充规则允许 Pillow 与生产 stb 解码的通道差 `<=1`。本轮 `zh_text_table` 与 `en_jee` 逐块实际最大绝对差均为 1；其他 PNG 无差。逐作业 `jpeg_crop_max_abs` 和所有硬性失败清单在 `evidence/summary.json`。

## 性能与版本

以下为首轮六次新 CLI 进程，均未清理 OS 页缓存。外层时间含进程启动、模型加载、作业和导出；不是独立模型加载时间。最大 RSS 是 GNU `/usr/bin/time -v` 的 `wait4` 值，单位 KiB；对 PDF 命令可包含被等待的 Poppler 子进程中的最大值，**不是**并发进程树 RSS 求和。清单中的内存字段仍是 `unavailable`，PDF 页前/后的 `VmRSS` 只是父进程快照。

| 输入 | 外层墙钟 s | 最大 RSS KiB | decode / layout / recognition / export ms |
| --- | ---: | ---: | --- |
| `zh_text_table` | 98.65 | 2,418,812 | 12 / 1,322 / 84,243 / 6 |
| `zh_formula` | 84.01 | 2,389,788 | 31 / 1,092 / 72,386 / 8 |
| `zh_merged_table` | 77.33 | 2,463,028 | 7 / 1,087 / 65,765 / 4 |
| `en_jee` | 73.17 | 2,339,380 | 1 / 1,183 / 62,350 / 1 |
| `zh_pdf`（两页） | 28.98 | 2,390,620 | 13 / 2,160 / 20,424 / 16 |
| `en_two_column` 首轮 | 38.37 | 2,353,140 | 2 / 1,031 / 28,085 / 4 |

PDF 第 1、2 页清单内 `render/pipeline/total` 分别为 `132/12429/12573 ms` 和 `132/10706/10850 ms`；第 2 页不另启动模型。双栏排序修复后同页外层为 40.01 s、最大 RSS 2,353,028 KiB；这两次不能用作性能变化因果判断。公共 `dococr_create` 单独调用 5.779 s，包含本轮配置/哈希校验与模型加载，但不含 Python 解释器启动，也没有清理 OS 页缓存；同引擎后续两次真实作业分别 94.185 s、87.280 s。不同计时边界不可直接相减归因。这些值仅是一次本机冒烟。

机器为 Linux 7.0.0-31-generic x86_64、g++ 13.3.0、CMake 4.4.3、glibc 2.39、Poppler 24.02.0；MNN 3.6.1，源码提交 `baaa5a62e9cc6d5b3660e37f8a2608a2d585adc6`。`Release`、`DOCOCR_REQUIRE_MNN/LLM=ON`、CPU 1 线程；九模型工件实际 SHA 在各作业 `run-manifest.json`，与 [`printed-page.example.json`](../../configs/printed-page.example.json) 相符。Layout 包修订 `c67c1a858d5f6c855172d4cfdf931798dafa2edd`，Ovis 包修订 `20f12e49d846941e67829a7a7c3645693e485942`；模型权重不纳入仓库。实际动态依赖见 `evidence/cli-ldd.txt` 和 `abi-ldd.txt`。

## 作业、错误与测试结果

生产 C ABI 同引擎连续两次真实教材页，分别返回 0，19 区域，终态引擎均 `engine_ready=true`；两个固定正文 raw SHA 都是 `e1cf9ab07816f67591c568dacd5addd6789fa23e2770c8f4bc67544056c00546`，两个 DocumentIR SHA 都是 `c947ab4108bf48aba23818aa46434ff90cc59a42ad4292477a88f1dfcc17e628`。真实取消在 `layout_started` 后发出，返回 7/`cancelled`，含 `recovery_completed` 而无 `page_completed`；同引擎随后处理新页，86.670 秒，固定 raw SHA 仍同基线。取消等待当前不可中断 MNN 调用的安全边界，未宣称即时强停。原始摘要和每次清单见 [`evidence/abi-continuous/`](evidence/abi-continuous/)。

生产入口负例 9/9 按预期拒绝，均退出 3：未知处理器、重复 normalize、必需 decode 被关闭、normalize 责任错误、工件哈希错误、`pending_probe` 状态、超限 `max_new_tokens`、加密 PDF、坏图。逐项原始 stderr 与判定在 [`evidence/negative/`](evidence/negative/)；该部分不需要用 fixture 冒充真实后端。固定公共测试另覆盖超时、OOM、后端失败、Poppler 中断、并发 Busy 和安全恢复，见完整 CTest 日志。

最终 `cmake --build build/linux-current -j4` 退出 0，[`final-build.log`](evidence/final-build.log)；`ctest --test-dir build/linux-current --output-on-failure` 退出 0、15/15，[`final-ctest.log`](evidence/final-ctest.log)；`litert-env` 的 `unittest discover -s tests -v` 退出 0、14/14，[`final-unittest.log`](evidence/final-unittest.log)。评分器新增的两个边界用例包含在这 14 项中。首次完整 unittest 曾因旧运行环境没有 `jsonschema`、评分器顶层导入 #11 脚本而失败；已消除该依赖，失败摘录、复现和修复记录见 [`unittest-initial-failure.md`](unittest-initial-failure.md)。

## 复现

从仓库根目录运行。先按 [`linux-current.md`](../linux-current.md) 准备固定 MNN、模型和 Poppler，并构建 `build/linux-current`。`output/` 被 Git 忽略；以下输出路径须不存在，或换成新的目录名。评估脚本使用系统 Python 的 Pillow、`jsonschema`；旧探针 unittest 用仓库既有的 `litert-env` Python。

```sh
python3 scripts/issue15_acceptance.py --cli build/linux-current/dococr_cli --out output/issue-15/real-new
python3 scripts/issue15_quality.py output/issue-15/real-new
python3 scripts/issue15_performance.py output/issue-15/real-new
python3 scripts/issue15_negative.py build/linux-current/dococr_cli output/issue-15/negative-new
python3 tests/job_control_real.py build/linux-current/libdococr_c.so output/issue-15/abi-new
python3 tests/job_control_real_cancel.py build/linux-current/libdococr_c.so output/issue-15/abi-new output/issue-15/abi-new/summary.json
ctest --test-dir build/linux-current --output-on-failure
/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v
```

排序修复后的同页复现仍用生产 CLI `--config configs/printed-page.example.json --input tests/fixtures/issue15/printed_science_2col.png --out 新目录`，随后用生产 `--reexport` 到另一新目录。首轮已存原始文件不可覆盖。质量失败直接见相应 `document.json` 的 `provenance.raw_output`、`status/error` 和 `run-manifest.json` 的 `stop_reason`；诊断资源 SHA 在 `evidence/asset-sha256.json`。固定 fixture 的单栏、双栏、跨栏标题、图文关系、异常及恢复测试作为契约覆盖，与上述真实模型质量分开报告。

本次固定目录的排序影响复核由 `python3 scripts/issue15_order_audit.py` 执行：其他五份输入共六页没有新增分段边界，双栏页新增页眉与两个分节标题边界，按 ID 的块数据和所有资产保持相同。输出在 [`evidence/order-audit.json`](evidence/order-audit.json)。换新输出目录时用 `--initial`、`--fixed`、`--reexport` 和 `--out` 指定对应路径。
