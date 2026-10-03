# #31 全量差异与 Paddle 候选归因

先运行公共 CLI，再核对候选/输入、原始响应与导出，最后参照 Paddle 选择有证据的对照。下列结论限定于保存的样本；不把检测或计分错误都称为模型能力上限。

## 英语高 CER 与拆分覆盖

`candidate-12` 有 20 个正文 GT、13 个父 text，805 个参考字符。多 GT/单 Region 将同一输出的 586 次编辑降为 49 次，排除了原一对一漏配的 7 项；原图、GT、Block/Region、原始响应及实际 Markdown 见 [逐页记录](evidence/pages/evaluation-candidate-12.json)。这证明该页原高 CER 主要受配对粒度影响，未证明逐字、顺序全部正确。

`candidate-03` 和 `candidate-04` 存在反向情形：一个较大 GT 被拆成多个预测 text。当前父区域评分只有多对一匹配，未配对的额外输出分别计入 763 和 399 个插入字符，两个页面合计 CER 分别为 2337/3089＝75.66%、815/735＝110.88%（均已包含插入）。其中包含覆盖配对差异，不能直接解释为模型生成了同等数量的错误内容。完整参考和实际输出均保留，待补一对多覆盖核验，不删 GT、不放宽门槛。见 [candidate-03](evidence/pages/evaluation-candidate-03.json)、[candidate-04](evidence/pages/evaluation-candidate-04.json)。

## 12 页全集的表格

4 个表均成功匹配，行列和跨度结构全部正确；严格计分仅 1/4 整表、28/36 单元格精确。检查完整原始 HTML、解析单元格和裁图后，8 个不相等单元格的可见差异为：

| 页面/区域 | 严格单元格 | 差异 | 归因 |
| --- | ---: | --- | --- |
| candidate-01 / b0011 | 2/6 | `：`/`:`、`，`/`,`，句点前空格 | 原始响应已有标点表示差异，HTML 解析保留文字；未发现行列、定义或数字被导出删除 |
| candidate-03 / b0027 | 6/8 | `（cm）`/`(cm)`、`（元/株）`/`(元/株)` | 括号表示差异；区间端点、开闭括号和 4/6/8 均与裁图一致 |
| candidate-06 / b0017 | 8/10 | `（人）`/`(人)`、`（小时）`/`(小时)` | 括号表示差异；表中 3/17/13/7 及 7/8/9/10 与裁图一致 |
| candidate-06 / b0023 | 12/12 | 无严格差异 | 此表原参考精确通过 |

这些局部内容核对没有修改整表/单元格成绩，也不证明对应整页正文全部正确。原图无法唯一决定字符编码的全半角形式，不能用后处理猜选 GT 拼写。[candidate-01](evidence/pages/evaluation-candidate-01.json)、[candidate-03](evidence/pages/evaluation-candidate-03.json)、[candidate-06](evidence/pages/evaluation-candidate-06.json) 保留完整对照。

开发集情况不同：`odb-11` 一表、`odb-17` 两表为 `partial`，原始响应和异常均保留，3 次表格回退计入分母；另有非精确单元格，不因原始 HTML 可读就声明通过。`odb-04` 的同一失败正文覆盖两个参考，均计完整删除。[逐页证据索引](evidence/summary.json) 不将失败改为 skip。

## 独立公式的任务与内容

12 页全集有 8 项原独立公式参考，严格通过 1 项。

- `candidate-02` 的逆时针旋转参考 anno 23 被预测为 `text` 的 b0016，完整箭头、角度及坐标已存在原始响应；其 GT 独立公式漏匹配是检测类别/任务与评分范围差异，不能直接说内容漏识别。顺时针的 b0023 是 formula，主要差异为 `\left`、`\right`、`^\prime` 与撇号表示。独立分母仍保留 2 项，不按成功响应改任务或 GT。[原始记录](evidence/pages/evaluation-candidate-02.json)
- `candidate-07` 的 b0006 将两处 `64` 转写为 `54`，原始响应已经错误；生产导出没有改变指数。b0004、b0013、b0015、b0019 还存在间距命令、括号尺寸、字体或数字空格表示差异；b0017 原预算严格通过。这些差异保持逐项记录，没有自动转换成等价或通过。[原始记录](evidence/pages/evaluation-candidate-07.json)

## Paddle 去边：生产实跑否决

固定参照 PaddleX `ffb64904d23708863ff5b8da312a5cbd52a7f462` 的 [公式输入准备](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/pipelines/paddleocr_vl/pipeline.py) 及 [crop_margin](https://github.com/PaddlePaddle/PaddleX/blob/ffb64904d23708863ff5b8da312a5cbd52a7f462/paddlex/inference/pipelines/paddleocr_vl/uilts.py#L1076)。其灰度归一化、阈值 200 取内容包围框只作为本地输入实验；Ovis 的提示词、模型和默认视觉预算保持不变。

先前六个单区域探针修正了 b0006 的指数，因此建立了带原资源/实际输入坐标记录的显式生产候选 `5e36fea`，串行新跑数列页、旧公式页、旧合并表页。数列页仍修正 `54→64`，但旧公式页出现两个实际内容退化：

| 页面/区域 | 默认原始响应 | 去边原始响应 | 原图核对 |
| --- | --- | --- | --- |
| zh_formula / b0005 | `e^{\alpha}x…` | `e^{a}x…` | 原图为 `α`，新响应字母错误 |
| zh_formula / b0009 | `e^{\alpha}x…` | `x…` | 原图有指数因子，新响应漏内容 |

另外，数列页 b0017 从带句点的独立显示公式变为 `## $q = -1/3$`，属于表示变化；合并表原始响应未改变。三个作业皆返回 0，不能据此认定内容通过。前后全部原始响应、真实输入框、命令和身份见 [生产对照](evidence/margin-production-comparison.json)、[候选身份](evidence/withdrawn-margin-candidate.json)；完整裁图、二进制、配置和源码在 `withdrawn-margin-candidate.zip`，其 SHA 见 [索引](evidence/artifacts.json)。

**去边候选已用 `0dc4559` 完整撤回。** 单页改善不能抵消旧样本新的错误；没有生产启用、按 GT 选裁图或纠正模型字符。此前统一增大公式预算的退化证据也继续保留于 [输入归因](input-attribution.md)。不能从 Paddle 的成功流程直接推断另一模型适用。

## 未核验边界

全部 20/12 页都有自动化差异及可追溯产物，局部图片和表格已人工核对；没有完成全部行内公式、正文及内部顺序的人工裁决。未核验项保留未通过。上述表示差异也没有改写原指标或授予质量豁免。
