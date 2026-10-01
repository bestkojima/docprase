# #25：冻结候选的带标注质量验收

任务：[Issue #25](https://github.com/bestkojima/docprase/issues/25)。本次使用 #23 封存的 12 页完整原图和对应上游参考，新跑 #24 的冻结生产候选。**产品质量未达标**：正文严格 CER 62.64%，九项门槛通过 2 项，#24 工程关仍未通过。最终成绩、逐页状态和门槛差距见 [results.md](results.md)，完整计分行与证据绑定见 [report.json](report.json)。

## 材料、候选与评分

材料版本为 `issue23-annotated-evaluation-2`，OmniDocBench revision 为 `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec`。数学 7 页，物理、化学、生物、语文、英语各 1 页；沿用全部封存页面，没有按识别成绩筛选。#23 的用户修订取消来源身份、同卷同册隔离及开发曝光准入要求，原记录保存在本次冻结副本中。因此本交付称“带标注评测”，不声称证明了严格独立留出的泛化能力。原业务 12/8 分组不变，缺全文参考的业务 8 页不计作本次真值。

使用 `output/issue-24/candidate/` 的原二进制、项目动态库、模型和有效配置，候选身份 SHA 为 `776b9f2b68fc6648568e04e744a651efe0080fd07be648046065ddf5cc999823`，生产源码基点为 `88183cd0fb4acd3e72e78a40d7bc5140fefa8d7e`。当前生产源码逐文件 SHA 与该候选一致；本任务仅新增评测工具、测试及证据。冻结模型依旧使用已核对字节的硬链接，不能抵抗原模型文件被就地修改；运行前后及各页执行前后重新核对哈希。

质量门槛逐字节沿用 #24，没有根据本次结果修改。正文折叠 Unicode 空白后按全部参考字符计算严格 CER，失败、回退、未执行与缺匹配计完整删除；行内与独立公式分别计算精确率；表格核对结构、行列、跨度、header 和文字；顺序使用全部参考对，缺匹配对计错误。回退和漏匹配保留完整正文/独立公式/表格参考分母。全集零分母为不可评，不能自动通过。全部分项同时通过仍须满足工程关与真实证据校验。

计分调用冻结的 #24 评分实现，保存其全部直接依赖 SHA 和源码副本；本次报告工具也在首次识别前保存。正文原始转写 CER 仅作诊断，不能代替严格流程成绩；图片分项表示定位和保存，不表示语义正确。空间重复计数与原始输出重复疑点分开，不能据空间重复为零宣称没有语义重复。多 GT/单 Region 的漏匹配仍需要 #26 内容核验，不冒称每项均是确认漏字。

## 证据与复现

`evidence/` 保存运行前冻结副本、运行与哈希检查、每页公共 CLI 命令、原始 DocumentIR/Markdown/运行清单/状态/事件/失败日志，以及原始产物压缩包的 SHA 与逐文件索引。完整包含图片、裁图和版面张量的无损 ZIP 保存在 `output/issue-25/archives/`；通过包 SHA、每个原始文件 SHA、解压资源核验和生产重新导出核对内容，原始字节未改写。原图与参考沿用 #23 的材料包。

本机磁盘空间不足，CLI 的最终新作业目录位于 `/dev/shm/docprase-issue25-final-jobs/`，完成后压缩到持久输出目录。临时内存目录不作为长期证据。Git 保存文本与资源索引，ZIP 和模型不提交。模型推理未配置随机种子；这次独立进程运行可按固定输入与参数重跑，但不保证逐字节确定性，不将运行耗时当成性能验收。

首轮使用两个独立进程，同时执行完整自动化测试，语文 `candidate-11` 被 signal 9 终止，CLI 返回 137；内核日志读取受限，内存竞争是可能原因，未声称已确认 OOM。该轮 12 页原始包、失败分母、资源核验及未通过报告保存在 `output/issue-25-parallel-attempt/`，Git 文本证据保存在 `evidence/parallel-attempt/`。没有补写缺失的 DocumentIR，也没有丢弃失败页。最终另开目录，在自动化测试结束后以一个进程重跑全部 12 页，不把首轮成功页拼接进最终成绩。

在仓库根目录执行，重跑必须换新的 `--out` 和 `--jobs`，避免混合候选：

```sh
TMPDIR=/dev/shm python3.12 scripts/issue25_run.py \
  --candidate-run output/issue-24 --out output/issue-25-new \
  --jobs /dev/shm/docprase-issue25-new --workers 1
TMPDIR=/dev/shm python3.12 tests/issue25_real.py output/issue-25-new
TMPDIR=/dev/shm python3.12 scripts/issue25_report.py \
  --run output/issue-25-new --out output/issue-25-new/report.json
```

依赖现有冻结候选、#23 原图与参考、Pillow 和 jsonschema。报告退出码 0 为质量通过，2 为已生成完整未通过报告，1 为冻结/输入证据非法。未执行页和丢失产物保留参考分母；冻结 SHA 不符不能作为有效验收。生产重新导出不再次调用模型。

本次已执行工具版本为 `8ddebe1`。代码审查后加强了工具自身的版本校验，当前版本会拒绝这轮旧工具的冻结记录；原始冻结副本和成绩没有改写。对本次旧轮进行复核，使用固定提交的独立工作树；新轮使用当前修复后的工具冻结并执行。

```sh
sha256sum -c docs/issue-25/seal.sha256
python3.12 -m unittest discover -s tests -p test_issue25_acceptance.py -v
git worktree add --detach /dev/shm/docprase-issue25-replay-new 8ddebe1
TMPDIR=/dev/shm python3.12 /dev/shm/docprase-issue25-replay-new/tests/issue25_real.py \
  /home/dr/project/docprase/output/issue-25
TMPDIR=/dev/shm python3.12 /dev/shm/docprase-issue25-replay-new/scripts/issue25_report.py \
  --run /home/dr/project/docprase/output/issue-25 \
  --out /home/dr/project/docprase/output/issue-25/rechecked-report.json
```

自动化与真实生产验证分别记录在 [verification.json](evidence/verification.json)；两轴审查见 [code-review.md](code-review.md)。#24 工程关的未通过项继续阻止产品质量声明，包括 #26 结构计划、图文关系与粒度核验。未通过的门槛不因 CLI 退出成功或任务完成而改变。

## 后续使用失败案例

[exposure.json](exposure.json) 记录本次读取输出用于质量验收，没有据此修改生产策略或调参。后续若使用这些失败案例修复或调参，应新增曝光记录，冻结新候选并重新验收，保留原版成绩及门槛。当前已取消的来源隔离资格不重新加入；若以后需要作严格独立验收声明，须另补未用于该候选修复的独立带标注材料，不能把本轮已曝光页面继续称为未见样本。
