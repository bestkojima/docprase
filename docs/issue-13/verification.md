# Issue #13 验证记录

## 构建与测试

在 `/home/dr/project/docprase` 执行，生产入口使用无 MNN/LLM 的构建：

| 命令 | 结果 | 日志 |
| --- | --- | --- |
| `cmake -S . -B /tmp/dococr-issue13 -DCMAKE_BUILD_TYPE=Release -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/tmp/dococr-no-mnn` | 退出码 0；MNN/LLM include 与 library 均为 `NOTFOUND` | `/tmp/dococr-issue13-configure-review.log`、`/tmp/dococr-issue13/CMakeCache.txt` |
| `cmake --build /tmp/dococr-issue13 --parallel 4` | 退出码 0 | `/tmp/dococr-issue13-build-safe.log` |
| `ctest --test-dir /tmp/dococr-issue13 -R '^reexport_integration$' --output-on-failure` | 退出码 0；固定往返、负例及无模型目录运行 | 定向终端输出 |
| `ctest --test-dir /tmp/dococr-issue13 --output-on-failure` | 退出码 0；15/15 通过，17.14 秒 | `/tmp/dococr-issue13-links-final-ctest.log` |
| `/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v` | 退出码 0；12/12 通过，37.15 秒 | `/tmp/dococr-issue13-links2-unittest.log` |
| `ldd /tmp/dococr-issue13/dococr_cli` | 退出码 0；无 MNN/LLM 动态依赖 | 终端输出 |
| `cmake -S . -B /tmp/dococr-issue13-production -DCMAKE_BUILD_TYPE=Release -DDOCOCR_BUILD_TESTS=OFF -DDOCOCR_MNN_ROOT=/tmp/dococr-no-mnn` 及 `cmake --build /tmp/dococr-issue13-production --parallel 4` | 均退出码 0；无测试专用 fixture 的生产构建 | `/tmp/dococr-issue13-production-review-configure.log`、`/tmp/dococr-issue13-production-links2-build.log` |
| `/tmp/dococr-issue13-production/dococr_cli --reexport /tmp/dococr-issue13-saved/document.json --asset-root /tmp/dococr-issue13-saved --out /tmp/dococr-issue13-copied-review`，随后 `cmp` JSON/Markdown/PNG | 从 `/tmp` 执行，均退出码 0；独立生产可执行文件完成往返 | `/tmp/dococr-issue13-production-review-run.log` |

系统 `python3 -m unittest discover -s tests -v` 曾因环境缺少 `numpy` 退出 1；同一套测试在仓库既有的 `litert-env` Python 中 12/12 通过。此项不影响生产 CLI 或 CTest 无模型构建。

## 六条验收定位

1. 生产 `dococr_cli --reexport` 从保存的 JSON 工作；测试在临时无模型目录调用它，构建与动态依赖核查见上表。
2. 读取器按已发布 1.0～1.4 Schema 和跨引用规则检查页面、内容、关系、资源及深层诊断资产；未知版本、非法字段、缺失/越界资产和关系断链均以退出码 3 明确失败。
3. 固定夹具覆盖文字、独立/行内公式、结构化表格、图片和阅读顺序；另有从历史真实 #8/#9 作业提取的 1.1/1.2 小夹具。Markdown 与已存期望逐字节比较。原文 HTML、普通/引用式链接和图片语法另经 MarkdownIt 解析核对，不产生未声明的活跃链接或图片。
4. JSON 按原始输入字节复制；稳定 ID、原页、bbox/源区域、partial/failed、`confidence:null`、关联及 raw provenance 不做识别或改写。
5. 保存路径下的全部资产在新目录逐字节复制；诊断 overlay/tensor/mask 也逐项验证，缺失资产不生成目标目录。
6. CTest 将公共往返测试纳入完整无模型套件，覆盖旧 1.0 及后续 1.1～1.4 语义；后续版本应在此测试增加对应固定作业与版本规则。

总控另对历史真实输出 1.0、1.1、1.2、1.3 和 1.4 共 9 例复核了 JSON、Markdown、引用资源字节一致性；当前链接安全修复后的记录由总控保存在 `output/implementation-control/issue-13-links-history/`，不纳入本项提交。
