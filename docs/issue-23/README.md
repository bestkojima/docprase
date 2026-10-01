# #23：带标注评测材料封存

任务：[Issue #23](https://github.com/bestkojima/docprase/issues/23)。2026-10-01，用户明确取消来源身份、同卷同册隔离及开发曝光的资格要求，保留“必须有对应参考标注”；原话及修订范围见 [requirements.json](evidence/requirements.json)。这项修订优先于原 Issue 中的严格留出要求。

**12 页带标注评测材料已就绪。** 固定版本 `issue23-annotated-evaluation-2` 已保存原图、对应上游参考、逐页清单、覆盖与 SHA。来源隔离不再是 #23 的提交条件；当前材料称为“带标注评测集”，不声称已证明严格独立留出。正式质量评测等待 #24 冻结候选方案与数值门槛，本次没有运行这些页面的识别。

## 对应“答案”是什么

OmniDocBench 提供页面的参考标注：文字转写、公式 LaTeX、表格 HTML/LaTeX、版面坐标与阅读顺序。它们用于比较“原图应被识别成什么”；试卷上印刷的答案或解析也作为页面原文识别，不要求系统求解试题。字段含义见 [上游固定版本说明](https://huggingface.co/datasets/opendatalab/OmniDocBench/blob/aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec/README.md)。

本地已有对应的 `OmniDocBench.json`。新增 12 页均与固定上游记录逐字段相同，逐页标注 SHA 一致，适用正文、独立公式和表格字段没有缺失。既有 20 页 OmniDocBench 开发集的原始参考也核对一致；原业务 8 页缺全文参考，继续不计入本次带标注评测集。无需补标原业务 8 页，也无需另找材料。[verification.json](evidence/verification.json) 记录本次复核。

使用上游原始参考即可满足当前标注要求，不新增逐字人工校订的提交门槛。以后发现标注争议时单独记录勘误，保留原始参考。

## 冻结内容

来源：`opendatalab/OmniDocBench`，revision `aa1ee96d106dbe53d0ae59474d75c6e6d9b53fec`。沿用原先在下载前选定的 12 页，没有按模型成绩选样；原图未裁剪、拉伸或去水印。完整逐页信息见 [manifest.json](manifest.json)。

| 学科 | 页数 | 学段 |
| --- | ---: | --- |
| 数学 | 7 | 初中、高中；代数、几何、函数、概率统计 |
| 物理、化学、生物、语文、英语 | 各 1 | 初中 |
| 合计 | 12 | 初中 8、高中 4 |

有教材、教辅和印刷解析，保留上游的扫描模糊、水印等标签。各非数学科目只有 1 页，不能据此作逐科泛化结论。原业务 20 页的 12/8 分组保留。此前的来源与曝光核查作为历史记录保留，不参与当前材料准入。

- 原图与原始参考：`output/issue-23/annotated-evaluation-v2/`，共 12 张原图、1 份子集 JSON、1 份上游说明。
- 便携包：`output/issue-23/issue23-annotated-evaluation-2.zip`，大小、SHA 及逐文件索引见 [package.json](package.json)。Git 保存清单与记录，原图和完整参考保存在本地便携包。
- 文件级 SHA：[materials.sha256](materials.sha256)；文档与记录 SHA：[seal.sha256](seal.sha256)。包的 SHA 保存在包外，避免循环哈希。

在仓库根目录复核：

```bash
sha256sum -c docs/issue-23/seal.sha256
sha256sum -c docs/issue-23/materials.sha256
```

文件缺失或 SHA 不符时先恢复材料。SHA 证明字节一致，不能证明标注完全正确。便携包解压到仓库根目录后，也可运行这两份外部 SHA 清单。

## 跨机恢复

[download-command.json](evidence/download-command.json) 给出通过 `uvx`、`huggingface_hub` 及镜像下载的固定 revision、12 张图片、完整 JSON 和 README。执行该命令后，在仓库根目录生成完全相同的子集：

```python
import hashlib
import json
import shutil
from pathlib import Path

manifest = json.loads(Path('docs/issue-23/manifest.json').read_text(encoding='utf-8'))
downloaded = Path('output/issue-23/upstream')
raw = (downloaded / 'OmniDocBench.json').read_bytes()
assert hashlib.sha256(raw).hexdigest() == manifest['upstream_annotation']['sha256']
upstream = json.loads(raw)
sealed = Path('output/issue-23/annotated-evaluation-v2')
(sealed / 'images').mkdir(parents=True, exist_ok=True)
records = [upstream[page['upstream_index_0based']] for page in manifest['pages']]
(sealed / 'OmniDocBench.json').write_text(
    json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
for page in manifest['pages']:
    shutil.copyfile(downloaded / page['image_path'], sealed / page['image_path'])
shutil.copyfile(downloaded / 'README.md', sealed / 'README.upstream.md')
```

随后运行 `materials.sha256` 校验，不调用模型。逐页标注 SHA 使用 UTF-8、`ensure_ascii=False`、`sort_keys=True`、`separators=(',', ':')` 的规范 JSON 序列化；整份子集文件另有字节 SHA。[restore-check.json](evidence/restore-check.json) 记录空目录恢复检查。

## 使用条件与历史

材料条件已满足；正式识别的门禁仍为 `closed`，仅等待 #24 冻结代码、模型、运行时、配置及正文、公式、表格、顺序、回退的数值质量门槛和分母。来源核查与历史开发曝光不再阻塞。原图、参考或清单变化时建立下一冻结版本，保留本版。

原冻结版本 `issue23-candidates-1` 的 20 份文档逐字节归档到 [history/issue23-candidates-1](history/issue23-candidates-1/README.md)，原便携包未改动；见 [history-verification.json](evidence/history-verification.json)。历史文件保留原路径与原结论，代表修订前状态。复核旧版时，在独立目录解压旧包或检出提交 `dcfd5af` 后运行旧 SHA 清单，避免以旧清单检查当前新文档。

本次仅修订材料记录，运行时代码未变化。此前同一代码的完整构建、CTest 23/23、Python unittest 25/25 均通过，日志保留在历史版 `evidence/`。本次重新核对当前原图、参考、文档 SHA、便携包和恢复流程；两轴审查见 [code-review.md](code-review.md)。
