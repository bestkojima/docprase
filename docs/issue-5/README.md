# Issue #5：配置执行计划与运行清单

## 当前可执行配置

公共 `dococr_create` 接受 UTF-8 JSON 文本；CLI 使用 `--config <文件>`。格式见 [`configs/fixture-plan.example.json`](../../configs/fixture-plan.example.json)。配置分为模型包 `models`、能力绑定 `skills`、流程 `flow` 与 `processing`、平台 `platform` 和开发预算 `execution`。字段严格校验，重复 JSON 键、未知字段、未知处理器、缺失能力、无效依赖、类型不连通、必需处理关闭和重复 normalize 均在加载前失败。`dococr_last_error` 返回机器可读的错误代码及详情；CLI 返回 3。输入或输出超预算返回 `DOCOCR_BUDGET_EXCEEDED`，CLI 返回 5。

`configs/pipeline.yaml` 是较早的拟议 1.1 配置，不作为本版运行输入。它包含尚待验证的真实模型协议、平台草案和更多处理选项。将它传给当前 CLI 会被拒绝；不能把 `pending_probe`、参考 resize/interpolation 或声明的 owner 当作真实后端能力。当前 JSON 1.0 只注册已能在公共单页路径执行的处理和测试专用 `fixture:*` 后端；真实 PP-DocLayoutV3 与 OvisOCR2 的契约由后续任务接入。生产库不包含 fixture，不能通过修改 `mode` 或清单状态启用它。测试夹具的 `verified_fixture` 只表示夹具协议可运行，不表示模型、OCR 质量或 MNN 已验证。

## 工件和责任

每个模型列出包根目录、相对工件路径和 SHA-256。加载时解析规范路径，拒绝越界路径、文件缺失、非普通文件和哈希不符；哈希按块计算。经校验的工件随 `BackendLoadSpec` 传给后端，后端须回报实际加载工件；公共入口逐项比对。夹具再次读取并核验文件，工件哈希前缀进入后端 profile 和文档来源记录，使作业结果能证明使用了所核验的工件。测试后端的责任由已注册实现确定：`fixture:normalized` 由 adapter 把 RGB8 转成 Float32 `[0,1]` 并通过固定像素值验收；`fixture:runtime` 在测试运行时执行相同变换；`fixture:graph` 模拟图内处理并产生不同的可见结果。配置与注册责任不一致即失败。`decode`、`crop`、`session_reset`、`rgb_identity` 和关闭的 `deskew` 分别记录实际执行、委托、合法 identity 或关闭原因。真实图内/运行时处理仍需后续模型契约证据。

## 计划、作业和预算

计划在配置验证后生成，包含规范化配置的 SHA-256；`dococr_execution_plan` 返回计划。`dococr_job_create` 固定当时的不可变计划；`dococr_reconfigure` 只允许同一后端、同一工件集合安装新计划，活动作业保留旧快照，后续作业取得新快照。更换工件须新建引擎。运行结果仍使用 DocumentIR 1.0；`dococr_job_manifest` 独立返回工件、配置哈希、实际测试后端/CPU、有效预算、处理记录、阶段毫秒耗时和内存指标不可用原因。超预算时也保留清单，未执行阶段标为 `not_run`；CLI 导出计划与清单并返回 5。成功时 CLI 另写 `document.json`、`document.md` 和资源。单页像素、输出总字节和测试生成 token 上限是可调开发预算；仅 `threads=1` 可执行，其他值启动失败。测试 token 按 UTF-8 字符模拟，不代表真实 tokenizer。运行清单不宣称固定速度或内存指标。

```sh
cmake -S . -B /tmp/dococr-issue5 -DDOCOCR_BUILD_TESTS=ON
cmake --build /tmp/dococr-issue5 -j2
ctest --test-dir /tmp/dococr-issue5 --output-on-failure
/tmp/dococr-issue5/dococr_cli_fixture --config configs/fixture-plan.example.json --input tests/fixtures/rgb2x2.png --out /tmp/dococr-issue5-output
```

生产目标 `dococr_cli` 不包含测试后端。它保留旧 `--backend none` 的明确 `Unsupported` 行为；配置路径对未核验的真实模型保持拒绝。示例命令只验证编排和导出，不是模型运行证据。
