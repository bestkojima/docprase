# Issue #12 验证记录

## 固定故障与公共边界

`tests/job_control.py` 仅通过公共 C ABI 观察运行、事件、状态、清单、结果和句柄；fixture 的文件闸门固定后端推理停留位置。它覆盖单区域运行中取消、PDF 第二页排队取消、协作超时与 Busy、OOM、后端局部失败、后端重建失败、持续失败时最多一次显式重试、64 条事件队列溢出、终态事件留存以及多次作业的结果和内存释放。另有测试专用 Poppler wrapper 控制子进程停留在渲染期间，确认取消后仍等待子进程结束才释放作业，清单保留中断页及渲染时间；生产代码仍用 argv 启动真实 Poppler。`tests/cli_job_control.py` 从外部进程向 CLI 发送 SIGINT，并以固定闸门检查 `--timeout-ms` 的实时事件、状态文件及退出码。断言针对公共行为、已知原文和稳定状态，不依赖推理内部调用次数或运行快慢。

真实双模型脚本 `tests/job_control_real.py` 在一个公共引擎上连续处理两次固定教材 JPG，要求两次匹配同一已知正文与原始模型输出，并检查每次终态、区域清单和后端可用状态。摘要保存在 `output/issue-12/real-continuous/summary.json`。原始模型输出两次须完全一致；与既有参考文本比较时只按原 #7 的首尾空白处理规则 `strip()`。`tests/job_control_real_cancel.py` 在公共 `layout_started` 事件后取消真实作业，核对重建完成，再于同一公共引擎处理固定页并与连续请求的 raw SHA-256 对比。这些输出目录不纳入提交。

## 命令与结果

在 `/home/dr/project/docprase` 执行：

| 命令 | 退出码与结果 | 证据 |
| --- | --- | --- |
| `cmake -S . -B /tmp/dococr-issue12 -DCMAKE_BUILD_TYPE=Release -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN` | 0 | `/tmp/dococr-issue12-configure.log` |
| `cmake --build /tmp/dococr-issue12 -j4` | 0 | `/tmp/dococr-issue12-build.log` |
| `ctest --test-dir /tmp/dococr-issue12 --output-on-failure` | 0，14/14 通过，15.27 秒 | `/tmp/dococr-issue12-ctest.log`；包含公共接口、CLI、PDF 和既有功能回归 |
| `python3 tests/job_control_real.py /tmp/dococr-issue12/libdococr_c.so output/issue-12/real-continuous` | 0 | `/tmp/dococr-issue12-real.stdout.log`、`/tmp/dococr-issue12-real.stderr.log`、`output/issue-12/real-continuous/summary.json` 与逐次原始文件 |
| `python3 tests/job_control_real_cancel.py /tmp/dococr-issue12/libdococr_c.so output/issue-12/real-continuous output/issue-12/real-continuous/summary.json` | 0（取消进度修复前运行） | `/tmp/dococr-issue12-real-cancel.stdout.log`、`/tmp/dococr-issue12-real-cancel.stderr.log`、`output/issue-12/real-continuous/cancel-and-next.json` |

两次真实连续请求均返回 `DOCOCR_OK`，文档状态 `partial`，每次 19 个区域，分别耗时 87.868 秒和 88.964 秒；两份 DocumentIR SHA-256 同为 `c947ab4108bf48aba23818aa46434ff90cc59a42ad4292477a88f1dfcc17e628`。已核对的正文块两次原始模型输出完全相同，SHA-256 均为 `e1cf9ab07816f67591c568dacd5addd6789fa23e2770c8f4bc67544056c00546`。它比既有真值多一个末尾换行，按 #7 的 `strip()` 规则与真值一致。脚本初版曾错误地把 raw 与去掉首尾空白的参考文本直接全等比较，因该换行退出 1；修复比较规则后重新运行退出 0，原始模型输出没有改写。每次的 `document.json`、`manifest.json`、`status.json` 和 `matched-raw.txt` 均已保存到同一输出目录。

真实取消在公共 `layout_started` 事件之后发起，`job_run` 返回 `DOCOCR_CANCELLED=7`，终态为 `cancelled`，恢复事件包含 `recovery_started` 与 `recovery_completed`。同一公共引擎再跑该页耗时 90.623 秒，已核对正文 raw SHA-256 与上述两次基线相同。原始 `cancel-and-next.json` 还记录了修复前一个进度缺陷：取消已被接受后，图像作业仍发出 `page_completed`。该原始日志保留不修改。随后生产代码要求 `!cancelled` 才能发出页面完成事件，并在版面后端因取消而抛异常时返回 `Cancelled` 同时保留异常代码；受控版面闸门测试经公共 ABI 验证 `page_completed=0`、无页面完成事件、清单保留 `layout_interrupted_error`。修复后没有为这一进度断言重跑 90 秒真实整页，因此真实取消日志只证明取消、重建及新作业原文隔离；页面进度修复以固定后端回归为证据。

真实验证在 Linux CPU 上完成。MNN 和 Poppler 的中途强制停止未被验证；超时上限作用于停止请求，后端调用及恢复可能继续占用时间和内存。Windows 构建与运行由 #14 验收。
