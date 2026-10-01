# #23：留出材料核验与候选封存

核验日期：2026-10-01（Asia/Shanghai）。任务：[Issue #23](https://github.com/bestkojima/docprase/issues/23)，父规格为 #16；起点提交为 `490303d39a8015b9f0a7ce9dd421243f627cf911`。

**严格留出尚未就绪。** 本次冻结了既有材料的曝光核查与 12 页带原始标注的新候选，合格留出清单仍为空。候选的源卷、源册或版次证据不足，无法证明与来源不完整的开发材料隔离。材料完整性核验通过不能替代留出资格通过；#23 保持未满足资格条件的状态。

本任务为最终质量验收准备依据。开发材料用于发现问题和修复；候选代码、模型、运行时、配置与质量门槛冻结后，才允许在经过来源隔离核验的留出材料上识别验收。本次没有运行候选识别，没有生成候选分数，也没有新增系统模块或校验 CLI。

## 既有材料核查

[source-audit.json](source-audit.json) 逐页记录了 47 个既有页面及额外曝光条目。

| 材料 | 核查结论 | 后续角色 |
| --- | --- | --- |
| OmniDocBench 20 页 | 已在 #17 提交整页开发评测，解码失败的两页也不能恢复未见资格；部分源卷/源册仍未查明 | 保留开发与评测角色 |
| 原业务 20 页 | 原 12 页开发、8 页留出及来源记录全部保留；20 张原图、14 个源文件 SHA 与原清单一致 | 原 8 页是历史未标注留出，缺全文参考；当前不计为质量验收就绪 |
| 旧 7 页回归 | 已用于对齐、开发和回归；`en_two_column` 首轮结果用于排序修复，未见资格已取消 | 继续作为开发回归 |
| 既有被排除候选索引 715 | 已做选样检查，保留历史记录 | 不作为本次新增未见材料 |

原业务清单内，`document_id` 没有跨 12/8 两组；这个检查的范围仅限该清单，无法排除与未知来源的 OmniDocBench 开发页同册。原初中数学模拟卷缺发布页和完整机构身份，不能因其下载 URL 与其他材料不同就证明严格隔离。原业务清单的逐字节快照保存在 [evidence/business-manifest.json](evidence/business-manifest.json)，原文件和分组未改动。

[exposure-audit.json](exposure-audit.json) 冻结了检索命令、4,079 个本地文本记录的路径及 SHA，并列出命中。12 个候选的历史命中均来自 `output/ovis-source/OmniDocBench.json`，这是 SHA 与固定上游一致的完整标注副本；没有发现候选在其余检索记录中的引用。不能把“存在完整标注副本”解释成已运行候选识别，也不能把文件名/字节哈希阴性解释成全局未曝光。重命名、重新扫描、裁图、同册其他页面、未留日志或其他机器的曝光仍可能不被检索发现。当前未见资格因此没有获得自动提升。

## 新候选与原始参考

来源固定为 [OmniDocBench 的该版本](https://huggingface.co/datasets/opendatalab/OmniDocBench/tree/aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec)，revision `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec`。上游提供文字、公式、表格与阅读顺序标注，字段含义见其 [固定版本说明](https://huggingface.co/datasets/opendatalab/OmniDocBench/blob/aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec/README.md)。本次核对的仓库根目录列在 [upstream-tree.json](evidence/upstream-tree.json)；该版本提供图片与标注，缺少将这些候选和所有开发页映射到完整源卷/源册的逐页资料。

选样按学段、学科与可见原图内容进行，选择记录在下载前保存，见 [selection-before-download.json](evidence/selection-before-download.json)。保留原始完整页面，不裁剪、拉伸、去水印或按识别成绩挑样。新候选数学 7/12，覆盖代数、几何、函数、概率统计；其余五科各 1 页。初中 8 页、高中 4 页。候选配额没有改变原业务 20 页的 12/8 分组。

| ID | 上游索引（0 起） | 学科/学段 | 实际内容与主要来源缺口 |
| --- | --- | --- | --- |
| candidate-01 | 697 | 数学/初中 | 相似三角形教辅，缺完整书名与版次 |
| candidate-02 | 703 | 数学/高中 | 《2024新高考数学真题全刷：基础2000题》156 页，缺源册对应及与未知开发源册的排除证据 |
| candidate-03 | 720 | 数学/高中 | 高三理科开学考试卷第 2 页，缺学校与年份 |
| candidate-04 | 732 | 数学/初中 | 概率初步教材 128 页，缺书名与版次核实 |
| candidate-05 | 767 | 数学/高中 | 导数极最值教辅 32/75 页，缺完整讲义名称 |
| candidate-06 | 841 | 数学/初中 | 数据分析分类集训 97 页，缺书名与版次；上游 `exam_paper` 实为章节练习 |
| candidate-07 | 856 | 数学/高中 | 等比数列教材 63 页，缺书名与版次核实 |
| candidate-08 | 713 | 物理/初中 | 流体压强专项讲义，缺机构/年度/完整册次 |
| candidate-09 | 707 | 化学/初中 | 有道中考化学寒假班讲义 20 页，缺年度及开发源册排除证据 |
| candidate-10 | 839 | 生物/初中 | 被子植物教材 86 页，缺版次核实 |
| candidate-11 | 857 | 语文/初中 | 学而思《醉翁亭记》教辅 1 页，缺完整讲义册次/年度 |
| candidate-12 | 1554 | 英语/初中 | 上游文件名为 2016 安徽中考英语印刷解析，缺解析编者及整册出处 |

12 页均已解码、尺寸与上游标注一致，图片 SHA 唯一且与 47 个既有页面的已知 SHA 无交叉。选中条目与固定上游记录逐字段相等；适用正文、独立公式、表格所需参考字段均存在。核验记录见 [verification.json](evidence/verification.json)。这些检查证明原始参考可获取及结构可用，尚未完成全文参考质量复核。发现标注争议时另记勘误，保留原始参考。

所有候选为印刷内容，未观察到手写答案；部分有扫描模糊、透印或浅色水印。逐页原图阅读检查与上游标签原样保存在 [manifest.json](manifest.json)，候选的可读性附有具体限制，未将它们一概声明为已合格的清晰留出。印刷解析及答案属于页面原文，不作为要求模型解题的指令。物理、化学、生物、语文、英语各只有 1 页，候选覆盖不能用作逐科泛化结论。

## 封存与复核

- 冻结版本：`issue23-candidates-1`；`qualified_holdout_ids=[]`、`status=not_ready`，12 页均为 `quarantined_holdout_candidate`。
- 本地原图：`output/issue-23/sealed/images/`；完整原始子集标注：`output/issue-23/sealed/OmniDocBench.json`。
- 便携包：`output/issue-23/issue23-candidates-1.zip`，含原图、原始子集标注、清单与核验记录；具体文件、大小及 SHA 见 [package.json](package.json)。包中的候选尚未获得合格留出资格。
- 文件级 SHA：[materials.sha256](materials.sha256)；文档及核查记录 SHA：[seal.sha256](seal.sha256)。Git 保存轻量来源与核验记录，原图及完整标注留在本地材料包，延续已有集合的保存方式。

在仓库根目录使用现有工具复核：

```bash
sha256sum -c docs/issue-23/seal.sha256
sha256sum -c docs/issue-23/materials.sha256
```

第一条核对本次封存记录，第二条需本地原图与子集标注。核对失败或文件缺失时先恢复材料，不能依赖旧核验报告继续验收。SHA 只证明字节一致，不证明来源隔离或标注正确。

另一台机器可按 [download-command.json](evidence/download-command.json) 的固定版本与文件名复用 `uvx`、`huggingface_hub` 下载；恢复完整参考时同时下载 `OmniDocBench.json`，上游 SHA 应为 `a45cd84b04ad8b793e775089640e6b681209abea33ead54c1828ddca35fae496`。只取清单所列索引，原样保存选中记录：

```python
import json
import shutil
from pathlib import Path

manifest = json.loads(Path('docs/issue-23/manifest.json').read_text())
upstream = json.loads(Path('output/issue-23/upstream/OmniDocBench.json').read_text())
sealed = Path('output/issue-23/sealed')
(sealed / 'images').mkdir(parents=True, exist_ok=True)
records = [upstream[page['upstream_index_0based']] for page in manifest['pages']]
(sealed / 'OmniDocBench.json').write_text(
    json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
for page in manifest['pages']:
    shutil.copyfile(Path('output/issue-23/upstream') / page['image_path'],
                    sealed / page['image_path'])
shutil.copyfile('output/issue-23/upstream/README.md', sealed / 'README.upstream.md')
```

随后运行 `materials.sha256` 校验，不调用模型。JSON 逐页记录的标注 SHA 使用 UTF-8、`ensure_ascii=False`、`sort_keys=True`、`separators=(',', ':')` 的规范序列化；子集文件 SHA 核对实际保存的整份文件。

## 验收门禁与资格撤销

当前门禁为 `closed`：来源隔离未证实，#24 的候选方案与数值门槛也未冻结。开始最终识别前，须补齐源卷/源册身份与开发曝光核验，形成合格名单及原始参考哈希，随后冻结候选代码、模型、运行时、配置，以及正文、公式、表格、顺序、回退的质量门槛和分母。

若留出识别结果被用于修复或调参，应记录时间、用途及对应变更，撤销整份源卷/源册的未见资格；同册其他页不能用于补充。保留本冻结版本，新增来自不同源文档的材料并建立下一冻结版本。候选源册仍未知时无法执行可靠的同册撤销，所以继续隔离候选，保持未就绪。

本次完成的是有证据的核查和候选封存。正式质量验收继续等待来源证据及 #24 的冻结结果；现有资料不足时明确报告未就绪，不要求用户从零全文标注。

## 本次检查

现有 C++17 Release 构建通过；[完整 CTest](evidence/final-ctest.log) 23/23、[Python unittest](evidence/final-unittest.log) 25/25 通过。测试使用既有固定输入，没有调用新候选。材料检查核对了原图解码、原始参考一致性、已知页面哈希去重、原业务工件与原清单一致性，以及未就绪状态和关闭的验收门禁。以上结果仅支持材料完整性和既有回归检查，正式留出验收仍未执行。

审查依据是开始本任务时的固定提交与本任务新增文件，见 [两轴审查记录](code-review.md)。
