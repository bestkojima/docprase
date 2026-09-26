# Issue #4：单页文档作业

## 构建与运行

要求 CMake ≥ 3.20、C++17 编译器。依赖的 stb 头文件已固定在 `third_party/stb/`，不需要 MNN、模型权重或网络。测试额外需要 Python 3 标准库。

```sh
cmake -S . -B /tmp/dococr-build -DDOCOCR_BUILD_TESTS=ON
cmake --build /tmp/dococr-build -j2
ctest --test-dir /tmp/dococr-build --output-on-failure

cmake -S . -B /tmp/dococr-production -DDOCOCR_BUILD_TESTS=OFF
cmake --build /tmp/dococr-production -j2
/tmp/dococr-production/dococr_cli --backend none --input page.png --out output
```

最后一条命令目前会明确返回 `Unsupported`：本任务仅建立无模型编排和导出契约，尚未接入生产推理后端。仅在 `DOCOCR_BUILD_TESTS=ON` 的构建中存在 `dococr_cli_fixture` 和 `dococr_c_test`，可执行固定结果演示：

```sh
/tmp/dococr-build/dococr_cli_fixture --backend fixture:sample --input page.png --out output
```

输出目录含 `document.json`、`document.md` 和 `assets/*.png`。`fixture:sample` 的文字是固定测试内容，不能据此声明真实 OCR、Layout 或业务质量通过。生产 `dococr_c` 不接受 `fixture:*` 配置。

## 公共接口

公开 C ABI 位于 `include/dococr/dococr.h`。`DocOcrHandle` 和 `DocOcrJob` 是单调注册表 ID，不是裸指针。入口文字用 `{data,size}` 明确 UTF-8 长度；图像输入带 `struct_size`、格式、字节数及原始 RGB8/GRAY8 的尺寸与行跨度。编码输入支持 8 位无 alpha PNG/JPEG，`width`、`height`、`row_stride` 必须为零；原始输入支持 RGB8/GRAY8。内部转换成自有 RGB8 缓冲，调用期间不得修改或释放输入缓冲；返回后调用方可任意复用原缓冲。

每个作业只运行一次。`dococr_job_run` 同步执行；调用方可在另一线程调用 `dococr_job_cancel`。运行中的作业和引擎销毁返回 `DOCOCR_BUSY`；有作业挂在引擎下时引擎销毁也返回 `DOCOCR_BUSY`。无效或已销毁 ID 返回 `DOCOCR_INVALID_HANDLE`。空白页和区域局部失败的作业返回 `DOCOCR_OK`，具体状态见 DocumentIR；损坏图像返回 `DOCOCR_INPUT_ERROR` 且无结果。`dococr_job_poll_events` 返回当前状态及作业级错误。

`dococr_job_result` 返回 JSON 与 Markdown 的库分配副本，`dococr_job_asset` 返回资源名与 PNG 的库分配副本。所有 `DocOcrBytes` 都必须调用 `dococr_bytes_free`；释放后原结构清零，第二次释放返回 `DOCOCR_INVALID_ARGUMENT`。分配身份 `allocation_id` 还能防止旧结构副本在堆地址复用后误释放新结果。调用方创建的图像缓冲仍由调用方释放。资源名由核心生成，不采信模型给出的路径。

## DocumentIR 1.0

版本化结构见 [`document-ir-1.0.schema.json`](document-ir-1.0.schema.json)。页面、LayoutBlock、Region、内容块和资源分别有自己的 ID。当前单页 ID 为 `p0001`；块 `b0001`、区域 `r0001`、版面块 `l0001` 在稳定阅读顺序中编号。候选 `rank` 保留为版面元数据，可能重复，绝不充当 ID。所有坐标都是源栅格页的左上原点、整数像素、半开区间 `[x0,y0,x1,y1]`；区域裁剪没有重采样。当前几何粒度仅为 region，识别置信度未知时保持 `null`，不生成字符或单元格坐标。`provenance` 保存后端 profile、请求 ID 和原始文本输出。

当前 `TensorRequest`/`TensorOutput` 在 `IInferenceEngine` 处使用带名称、dtype、layout、shape 和自有字节的张量，`GenerationRequest`/`GenerationOutput` 独立。上层从候选张量解码版面块，再规划单块区域、识别与组装。若后端文字、原始输出或错误含非法 UTF-8，块标记失败，原始字节分别保存在 `text_base64`、`raw_output_base64`、`error_base64`，JSON 仍保持合法 UTF-8。测试 fixture 产生固定张量和生成结果；后续真实模型适配不得把 fixture 作为协议或质量证据。

此 1.0 结构只覆盖单页、单块对应单区域、简单几何阅读顺序及基本文字/独立公式/HTML 表格/图片。复杂列序、内容归属、图注关系、多页 PDF、JSON 再导出及真实推理均由后续任务处理。新增可选字段可在 1.x 版本发布；改变已有字段语义、坐标空间或 ID 规则时必须发布新的主版本及显式迁移器。读取器必须检查 `schema_version`，不能静默按 1.0 解释未知主版本。基本 Schema 不能表达跨字段关系；读取器还须校验 ID 唯一、reading_order 和 source 引用存在、bbox 在页面范围内、资源路径在输出目录内。
