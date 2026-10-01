# Issue #26：原错误图片关闭前复验

2026-10-01 23:58（北京时间），**复验通过，满足用户“先验证当时的错误图片，如果通过，则此 Issue 关闭”的条件**。本轮核验已完成的实现，没有修改生产源码。实现提交为 `4b1cf084de80e188a67eaf254d7c77413e11b556`，核验目标为 `95804817b2be5f02f74c472726138c965b6e848d`。

## 原错误图片与新生产运行

从核验目标重新构建真实 MNN/Ovis CLI，保持完整原图、CPU 单线程及 SmartResize Lanczos/0.3，重新识别 odb-03/07/08/13，四个公共 CLI 作业全部 exit=0，逐页候选哈希通过。

| 原反例 | 本次结果 |
|---|---|
| odb-03 第17题 | 图片顺序 c8、c5、c6、c13，即 A→B→C→D；四个真实 OCR 标签分别绑定 c32、c27、c36、c33，图片与标签相邻 |
| odb-03 第18题 | 图片顺序 c4、c1、c0、c2，即 A→B→C→D；四个真实 OCR 标签分别绑定 c26、c31、c25、c30，图片与标签相邻 |
| odb-07 | c9 斐波那契图片与 c10 人物说明绑定；图注只调度和输出一次 |
| odb-08 | c20 图片与 c42“第8题”绑定；第11题三行及12个标签关系保留 |
| odb-13 | 两组、四个标签和双栏关系保留，无新增错序 |

第17/18题 C、D 图片与原页逐图核对，标签来自真实 OCR，保存的原样文字是 Markdown 标题 `## A\n` 至 `## D\n`，没有补造标签。c34 水印仍保留为误检证据，没有混入选项组。新运行的第17/18题对照 PNG 及 odb-07/08 阅读顺序 PNG 已逐张查看。

四页共101个 Region/资源、7个图组、26个图注绑定。对原基线固定比较1286个共同 GT 顺序对，保留137对修复，新增错序0；与已验收的1.9相比，阅读顺序、OCR内容、OCR状态及绑定均一致。四页首次 Markdown 与生产 `--reexport` 逐字节等价，JSON与资源保持等价。

odb-03 仍是 partial，保留原有公式校验回退；其他三页为 ok。这些既有 OCR/语法问题没有被隐藏，不影响用户指定的图文结构关闭条件。

- [四页完整核验](evidence/closure-focused-acceptance.json)
- [本次源码、模型、数据、运行时冻结](evidence/closure-focused-freeze.json)
- [四页新生产运行命令与完成记录](evidence/closure-focused-run-summary.json)
- 本地完整作业：`/home/dr/project/docprase/output/issue-26/closure-reverify-0fbc`
- [第17题新运行对照 PNG](/home/dr/project/docprase/output/issue-26/closure-reverify-0fbc-png/odb-03/q17-comparison.png)、[第18题新运行对照 PNG](/home/dr/project/docprase/output/issue-26/closure-reverify-0fbc-png/odb-03/q18-comparison.png)

## 旧20页验收证据复核

本轮没有重复20页模型推理。此前完整20页新生产运行继续作为原完整验收，本轮对它执行了独立产物核验：43个冻结源码逐文件匹配实现提交；模型、实际运行库、配置、原图、标注、20页1532个产物和8张已提交 PNG 哈希通过。PNG 来源文档与真实作业产物的绑定全部通过。

使用本轮新构建的生产 CLI 对旧20页重新导出，20/20 成功，Markdown 逐字节一致，JSON及裁图资源等价。该项是旧产物审计和新执行的重新导出，不冒称本轮20页新推理。

- [旧20页复核及重新导出记录](evidence/closure-retained-audit.json)
- [实际执行的独立核验脚本](evidence/closure-audit.py)；脚本固定本次路径，产物保存在 `/home/dr/project/docprase/output/issue-26/closure-evidence-0fbc-pass`
- [总凭据与证据哈希](evidence/closure-verification.json)

## 构建、回归与两轴复核

从核验目标独立配置并完整构建 C++17 Release，真实 MNN/LLM 为必需依赖。两项 #26 公共回归先单独通过，随后一次完整 CTest 25/25 通过，耗时71.84秒；六个 #26 Python模块编译检查通过。见 [本次完整 CTest 日志](evidence/closure-ctest.log)。

按 implement 调用 code-review，两名独立审查代理分别执行 Standards 和 Spec，固定基点为 `d5cd4beeb6b2cee17ab907b08fa4436718206edd`。Standards 没有硬性违规，只有一项不阻断的 P3 版本判断维护性提示；Spec 没有确认缺口、范围扩张或实现错误。完整结果见 [两轴复核](code-review-closure.md)。

## 关闭范围

按用户本次指令，原错误图片复验通过即可关闭 [Issue #26](https://github.com/bestkojima/docprase/issues/26)。[PR #30](https://github.com/bestkojima/docprase/pull/30) 的合并是独立状态；核验时 PR 为 draft、未合并。关闭 Issue 不表示代码已经进入 main，也不表示整体 OCR 质量通过。后续 #27/#24 的质量验收、#28 后处理审计和 #29 水印/装饰策略继续按原范围跟踪。
