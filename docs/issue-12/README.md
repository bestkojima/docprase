# Issue #12：作业进度、取消与安全续用

## 公共接口

`DocOcrInput` 尾部新增 `timeout_ms`（0 表示无限时）。旧调用方的 `struct_size` 不含该字段时按 0 处理；ABI 版本和既有返回结构不变。`dococr_job_run` 仍同步执行，调用方须在函数返回前保持输入缓冲区有效。`DOCOCR_TIMEOUT=11` 表示作业截止时间触发后的最终状态。

- `dococr_job_status` 返回 JSON 快照：`state`、`running`、`terminal`、页/区域进度、错误、取消/超时请求、`engine_ready`、队列待取数与丢弃数。`terminal=true` 表示后端调用、恢复和终结处理已完成；只设置取消标志不会使其提前变为终态。
- `dococr_job_next_event` 依次取走事件；空队列返回 `DOCOCR_NO_RESULT`。队列最多保留 64 条，满时淘汰最旧事件并累加 `events_dropped`。状态快照与最后的 `terminal` 事件可在不依赖旧事件的情况下查询最终结果。原 `dococr_job_poll_events` 保持快照语义，供旧调用方使用。
- `dococr_job_wait(job, timeout_ms)` 等终态；0 为即时查询，等待超时返回 `DOCOCR_BUSY`，不能据此销毁运行中的句柄。`dococr_job_destroy` 和 `dococr_destroy` 在后端仍运行或恢复时返回 `DOCOCR_BUSY`。所有返回的 `DocOcrBytes` 仍须逐个调用 `dococr_bytes_free`；重复释放与失效句柄返回原有错误。

事件包含页、区域 `request_id`、页/区域已完成数和总数。阶段包括页面开始/完成、解码完成、版面开始/完成、区域开始/完成、导出开始/完成、取消/超时请求、恢复与终态。CLI 将实时事件写到标准输出及 `job-events.jsonl`，将最终快照写到 `job-status.json`；错误快照继续写标准错误。CLI 支持 `--timeout-ms`，收到 SIGINT 后走公共 `dococr_job_cancel`。超时退出码为 6，取消为 130。

## 取消粒度与恢复

当前 MNN 推理和 Poppler 子进程均没有已验证的中途安全停止契约。取消与超时是协作请求：正在进行的调用完成后，在下一个页/区域边界停止安排工作；期间 `state=cancelling`、`terminal=false` 且销毁返回 Busy。执行已结束而恢复尚未完成时，新的 `dococr_job_cancel` 返回 Busy，避免“取消已接受”却最终报告成功。`timeout_ms` 不是强制中断后端的时限，`dococr_job_wait` 的超时也不代表运行已经结束。运行结束后才卸载或重建后端。若取消请求与已确认的输入、预算或后端失败同时发生，最终报告实际失败并在状态中保留取消请求；后端明确返回 `stop_reason=cancelled` 时仍报告取消。后端自身的 `stop_reason=timeout` 是推理失败，不等同于作业的协作超时请求。PDF 取消清单保留已完成页与中断页的阶段、区域、页号和时间，未处理页不会伪装为成功。

取消、后端失败、受控内存不足以及带失败区域的部分结果后，库在当前运行线程中卸载并重建后端，重新执行载入和工件绑定校验，完成前保持 Busy。重建成功后可在同一公共引擎创建新作业；重建失败时 `engine_ready=false`，原作业的结果/运行清单仍可取，新作业的 `job_run` 返回 `DOCOCR_FAILED`，调用方须销毁引擎并重新创建。终结清单自身若分配失败，也进入明确 `failed`/`finalization` 状态并阻止复用该引擎。

库不自动重试取消或超时。调用方先保存本次状态、清单和可用结果并释放作业，再检查 `engine_ready`；只在实例已验证可用、错误适合重试且尚未重试时创建第二个作业。第二次仍失败就停止；若 `engine_ready=false`，销毁引擎并重新创建，不在原引擎上重试。实际可执行的有限重试、持续失败终止、OOM 后恢复及重建失败拒绝复用见 `tests/job_control.py`。

## 验证

复现命令与结果见 [verification.md](verification.md)。固定后端只编入 `dococr_c_test` 和 `dococr_cli_fixture`，生产库不载入 fixture。真实模型验证使用项目固定教材页、CPU 线程 1、已绑定模型工件；测试范围不代表复杂拍照或手写材料质量。
