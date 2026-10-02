# Issue #29：水印误检诊断与显式复核

本票新增按原页和候选证据绑定的复核策略。默认保留；仅对人工/维护者已确认的原始 `image`（class 14）候选显式 skip。未加入通用水印分类器，不依据尺寸、位置、颜色、重叠或缺少 GT 自动删除内容。

## 真实诊断

[odb-03 原页](evidence/odb-03-original.jpg) 的 [c34 裁图](evidence/odb-03-c34.png) 是浅蓝色重复背景标志的一部分。页面右侧、底部重复同形标志，c34 内没有坐标轴、曲线、题目文字或选项标签，属于**检测误检**。原模型 class 14、score 0.395996958、rank 105、mask row 34、mask 非零 36；原框 `[124.252678,418.795685,213.631577,459.802155]`、裁图 `[124,418,214,460]`。它不是 NMS 重复框，不是选项排序错误，也不是 Ovis 转写错误。

- c34 修复前有独立 LayoutBlock/Region/image 资源；应用[本页明确复核记录](evidence/odb-03-review.json)后从选中候选中跳过，不再创建 Region 或调用 Ovis。原始候选、原图、裁图、[mask](evidence/odb-03-c34-mask.png)与原因均保留。
- 同页八幅有效选项图、八个短标签与页眉标志保留。页眉标志与背景标志同源，说明仅凭视觉相似或颜色不能授权删除。
- 目视检查固定 20 页中保留的 49 个 class 14 候选，未确认第二个可单独删除的误检。odb-07 两个人物肖像、odb-08 彩色展开图及立体图都是有效教材内容。odb-13 c6/c7 虽然像重复纹理，原页题干明确引用它们表示沸腾前后的气泡，必须保留。
- 同页另有可确认的重复背景标志，但没有独立选中候选；不能伪造 LayoutBlock/Region，也不能因正文含水印而删除整块正文。[逐例诊断](evidence/visual-cases.json)区分独立误检、混合内容、未形成候选的背景及有效内容，保留判断依据。

所有页面都是既有开发/评测集，不作为新增留出集。没有使用“缺少 GT”来确认水印。

## 策略和配置

1. 在原配置的 `execution` 中加入 `"layout_candidate_reviews": []`，通过公共 CLI 或 C ABI 运行。空数组只收集证据，输出 DocumentIR 1.12；不配置时沿用现有行为及版本。
2. 查看 `layout_diagnostics.candidate_reviews.source_asset` 对应的完整解码原页，结合候选、裁图与 mask 复核。PDF 的诊断在各页 `layout_diagnostics` 下。
3. 从该作业生成一条复核记录：

```bash
python3 scripts/layout_candidate_review.py --job output/review-job \
  --candidate 34 --decision confirmed_watermark \
  --reason '原页重复背景标志片段，裁图不含题目、曲线或选项标签' \
  --out output/review-c34.json
```

4. 把生成的 JSON 对象放入原配置 `execution.layout_candidate_reviews` 数组，再通过相同公共作业入口运行。

复核绑定内容包括：解码 RGB 页面的尺寸和全部像素 SHA-256、candidate ID、实际裁图范围，以及原始 float32 候选行和完整 int32 mask 行的 SHA-256。模型输出、mask、页面像素或裁图映射变化即失效；不会模糊匹配到其他页面或插图。当前张量制品按项目支持平台的 little-endian 格式计算。

`decision` 支持 `confirmed_watermark / confirmed_decoration / suspected / keep`。前两种只有在原始 class 14 且证据匹配时才跳过；`suspected / keep` 保留。页眉图、页脚图、图表、印章、文字、公式、表格和未知标签均受保护。所有记录必须提供非空理由；重复或非法记录在配置阶段拒绝。

每条记录的 `outcome` 区分 `skipped / retained / protected_content / page_mismatch / candidate_mismatch / already_filtered`。规则在现有检测后处理之后、内容归属和阅读顺序之前执行：不会改变 NMS 的原因，也不把已执行的 OCR 失败改记为 skip。跳过候选不会加入 Ovis 成败统计；图片资源及内容归属继续独立记账。默认 Markdown 的页眉展示策略保持既有行为，此处“保留页眉”指不新增检测删除，并保留其 IR/资源。

空数组收集或原页匹配时保存完整解码原页 PNG，并计入现有输出字节预算；原页不匹配时 `source_asset` 为 null，不额外保存无关原页。过小预算正常报错。即使全部候选被跳过，也不伪称完成了 OCR 识别。该策略处理“已确认误检的可追溯修订”，后续新页面仍需复核，不能宣称自动识别了所有水印。

## 验证与复现

[verification.json](evidence/verification.json)记录源代码、二进制、动态库、原图和前后产物哈希。`before/after` 的 JSON、Markdown、运行清单、执行计划及模型原始输出已压缩归档，完整本地作业路径和逐文件清单可追溯。原页和复核裁图也能随公共重新导出保留。

- 公共 CLI 与直接 C ABI：确认水印/装饰 skip、疑似/保留、页面/候选/裁图变化失效、其他标签保护、配置拒绝、证据缺失拒绝、图片和多页 PDF 重新导出。
- 4 个真实原页（odb-03/07/08/13）重新调用生产 MNN Layout 作前后对照；c34 外所有输出内容、裁图像素、阅读顺序及选项组一致。
- 20 页历史 Layout 张量/mask 与 Ovis 文本经公共 fixture 作业完整重放，对照本次修改前后：除 c34 外内容、状态、原始输出、裁图、阅读顺序、选项绑定与唯一内容归属一致。此项隔离结构影响，**不是新一轮 Ovis 识别质量测量**。#26 的受控结构和 Region 映射测试也重跑。
- C++ Release 编译、CTest（含 KaTeX）、Python unittest 与脚本语法检查见 [验证记录](validation.md)。

```bash
cmake -S . -B build/issue29 -DCMAKE_BUILD_TYPE=Release -DDOCOCR_TEST_KATEX=ON
cmake --build build/issue29 -j 6
ctest --test-dir build/issue29 --output-on-failure
python3 tests/issue29_reviews.py build/issue29/dococr_cli_fixture \
  build/issue29/dococr_cli build/issue29/libdococr_c_test.so
python3 scripts/issue29_verify.py --cli build/issue29/dococr_cli \
  --fixture-cli build/issue29/dococr_cli_fixture \
  --baseline-cli output/issue29/baseline-bin/dococr_cli \
  --baseline-fixture-cli output/issue29/baseline-bin/dococr_cli_fixture \
  --out output/issue29/new-verification --archive output/issue29/new-evidence
```

基线为任务开始时 `235bc9f` 加已有工作区修改编译所得，并保存了既有差异摘要哈希；基线/候选动态库由各自目录的 `LD_LIBRARY_PATH` 显式选定。既有未提交文件不属于本次提交。复现真实和历史重放需要本地冻结模型及原数据，脚本核对来源并在缺失时失败，不把历史结果冒充新运行。
