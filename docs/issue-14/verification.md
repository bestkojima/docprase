# Issue #14：Linux 实测验收记录

执行日期：2026-09-27。输入为固定中文教材页，使用生产 `dococr_cli` 和生产 `libdococr_c.so`；所有模型工件均与 `configs/printed-page.example.json` 的 SHA-256 相符。关键原始结果、有效计划、ABI 结果及日志已保存到随提交交付的 [`evidence/`](evidence/) 并列出 [SHA-256](evidence/SHA256SUMS)；包含 45 个资产的完整工件在本机 `output/issue-14-linux/`（Git 忽略）。新检出可按 [运行说明](README.md)重建全部工件。

| 验收条件 | 实际证据与结论 |
| --- | --- |
| 1. Linux 编译链接与工具链 | `cmake -S . -B build/linux-current -DCMAKE_BUILD_TYPE=Release -DDOCOCR_BUILD_TESTS=ON -DDOCOCR_REQUIRE_MNN=ON -DDOCOCR_REQUIRE_LLM=ON -DDOCOCR_MNN_ROOT=/home/dr/project/MNN`、`cmake --build build/linux-current -j4` 均退出 0，构建日志含 `dococr_core`、`dococr_c`、`dococr_cli`。`g++` 13.3.0，CMake 4.4.3，MNN 3.6.1/提交 `baaa5a62…`；`ctest --test-dir build/linux-current --output-on-failure -j2` 为 15/15。见 `configure.log`、`build.log`、`compiler.txt`、`cmake-version.txt`、`mnn-commit.txt`、`ctest.log`。 |
| 2. 双模型真实 CPU 单页 | `python3 scripts/issue14_linux_verify.py --cli build/linux-current/dococr_cli --lib build/linux-current/libdococr_c.so --out output/issue-14-linux/real` 退出 0；脚本从生产 CLI 读取 JSON、Markdown、完整 `run-manifest.json`、有效计划与资源。教材页为 `partial`、19 块，正文 `b0003` 经 `strip()` 去除首尾空白后与真值匹配；其未经修改的原始输出 SHA-256 为 `e1cf9ab07816f67591c568dacd5addd6789fa23e2770c8f4bc67544056c00546`。`summary.json` 记录 9 个模型文件 SHA、配置哈希、CPU/线程/采样参数，`raw-reference-block.txt` 和完整 JSON 保留原始结果。 |
| 3. 中文路径、UTF-8、相对资源、依赖 | 原输入实际命名 `教材原图.jpg`，输出为 `中文教材页/job`；严格 UTF-8 解码的 JSON/Markdown 通过 DocumentIR 1.3 Schema。19 个声明资源均存在，Markdown 中 5 个本地 PNG 引用均为有效相对资源；目录实际有 45 个资产。`cli-ldd.txt`、`abi-ldd.txt`、`cli-dynamic.txt`、`abi-dynamic.txt` 记录运行库位置及构建时 `RUNPATH`，无 `not found`。 |
| 4. `.so` 公共 ABI | 独立 Python `ctypes.CDLL` 只声明 `include/dococr/dococr.h` 中的 C 类型，实际调用生产 `.so` 再次解析同一页，`job_run=0`，取回 JSON、Markdown、清单，查询资产计数为 45 并取回首项字节且配对释放；CLI 完整导出 45 个资产文件。两份文档与 CLI 逐字节 SHA 一致。ABI 版本 1；无效句柄 2、忙句柄 3、旧结果结构拒绝 1、48 字节旧输入结构进入解码并返回输入错误 5；错误 JSON 传递 `configuration_required`；`dococr_bytes_free` 配对释放后重复释放返回 1，job/engine 句柄按序销毁。见 `abi-result.json`、`abi-document.json`、`abi-document.md`、`abi-manifest.json`。 |
| 5. 可复现说明 | [README](README.md)记录固定 MNN 提交、MNN 编译开关、程序构建/运行命令、模型来源及修订、九项哈希校验、Python 验收脚本、共享库查找和迁移约束。模型权重与 MNN 库明确为外部依赖；本机构建路径只出现在实测命令/动态依赖证据中。 |
| 6. 实际完成与边界 | 上述均在本机实际 Linux 环境执行。CLI 成功不等于整页正确：4 个细小文字区域达到 token 上限，整页 `partial`；本项不声称多页或全场景质量，交由 #15。 |

本次 `document.json` 的 SHA-256 为 `c947ab4108bf48aba23818aa46434ff90cc59a42ad4292477a88f1dfcc17e628`，`document.md` 为 `9021bcdea45a2e18877e3ccc6cabe024b65dcbc8c94d35b8e9501523a02c8f66`；生产 ABI 输出同值。CLI stdout 保存在 `real/cli.stdout.log`，stderr 为空，原始导出目录保存在 `real/中文教材页/job/`。这些哈希证明本次两种入口返回同一结果，不代表其他页面的识别质量。
