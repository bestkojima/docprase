# Issue #29 验证记录

基线：任务开始时 `235bc9f` 与已有工作区修改；后者未并入本次提交。使用 C++ Release 构建完成类型/链接检查，Python 脚本通过 `py_compile`。新增公共测试先在旧版本因 `unknown_field: layout_candidate_reviews` 失败，再通过实现。

- 首轮完整 CTest 32/34：新测试直接 C ABI 部分未切换仓库根目录导致相对模型路径失败，修正后通过；原有 `contract` 的 `running` 轮询在并发负载下超时，串行重跑通过。保留首次日志，不伪称首次全绿。
- 最终串行 CTest **34/34 通过**（包含实际 KaTeX 与 #26 结构/Region 回归，173.17 秒）；新增 mask 变化保护场景的公共测试单独复测通过。真实验证成绩由 `evidence/verification.json` 记录。
- 系统 Python 首次 unittest 因缺少 numpy/cv2 无法运行旧测试；使用已有 `.scratch/issue27-test-env/bin/python` 后 **46/46 通过**。未为了本票改动全局环境或依赖文件。
- 新测试覆盖 CLI、直接 C ABI、JSON 重新导出、PDF 多页、确认水印/装饰、疑似保留、有效标签保护、过期页面/候选/裁图、非法配置和证据缺失。后处理原有原因与跳过记账分开。
- 真实测试区分新执行 MNN Layout 与历史输出 fixture 重放；不声称执行了新的 Ovis 全量识别。较早中间运行发现无关大页额外保存原页会超预算，已通过仅对收集/匹配页保存原页解决；最终证据另行完整重跑。

完整本地执行日志在 `output/issue29/`，便携日志在本目录 evidence 下。代码审查按 Standards / Spec 两个独立轴执行，结果见 `code-review.md`。

最终归档核验：4/4 真实 Layout 对照、20/20 历史输出公共重放通过；源代码、候选及基线二进制/动态库哈希与执行时记录完全一致。单一明确 skip 为 odb-03 c34。
