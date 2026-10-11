/*
 * 页面流水线学习入口
 *
 * 一、先看整个处理过程
 *   调用者（abi.cpp / pdf_job.cpp）
 *     -> run_page：建立页面身份，解码输入，检查输入预算，选择处理路径
 *     -> PP 路径：版面推理/解码 -> 候选筛选 -> 可选复核 -> 冻结页面计划
 *                -> 创建区域并识别 -> 汇总资源/页面状态 -> export_page
 *     -> 旧接口路径：版面推理/解码/排序 -> 逐候选执行 -> export_page
 *     -> export_page：生成 DocumentIR 和 Markdown，检查输出预算，提交输出
 *
 * 当前生产识别使用 PP-DocLayoutV3 + 区域 VLM，也支持只输出 PP 版面。
 * generic 是旧 layout_candidates 接口的兼容路径，主要供测试使用；
 * 它不表示 PP-Structure，也不表示另一条已实现的生产 VLM 流水线。
 * 本文件处理一页图像；PDF 的拆页、渲染及多页汇总在 pdf_job.cpp。
 *
 * 二、建议阅读顺序
 *   先读文件末尾的 run_page，再读 run_doclayout_pipeline，最后读 export_page。
 *   run_generic_pipeline 用于比较旧接口的处理差异。
 *   elapsed 和 append_assets 是这几个函数共用的计时/资源搬运工具。
 *
 * 三、理解本文件中几种数据的区别
 *   context：借用本次调用的后端、原图、身份、配置、取消标志和进度回调。
 *            它不拥有这些对象，阶段函数不能把其中的引用保存到调用结束以后。
 *   audit：RunResult 运行记录，保存阶段标记、耗时、错误、最终返回码及输出。
 *          did_layout / did_export 等只说明相应步骤走过，不等于整页成功。
 *   records：全部 PP 候选及其筛选/处理诊断；selected 标记哪些候选参与规划。
 *   page_plan：识别前页面计划，固定内容归属、裁框、阅读顺序及识别执行顺序。
 *              阅读顺序组织文档，识别执行顺序安排模型调用，两者可以不同。
 *   execution：区域执行结果，含块、裁图/mask、区域进度以及更新后的 audit。
 *              completed 表示执行没有提前退出，不表示每个块都识别成功。
 *   output：暂存的 JSON、Markdown 和资源；输出预算通过后才赋给 audit.output。
 *
 * 四、修改时先找对应实现
 *   输入/裁图/叠加图：image_resources.cpp；版面预处理：layout_preprocess.cpp。
 *   模型请求：layout_inference.cpp；张量契约/坐标回映：layout_output.cpp。
 *   筛选：layout_filter.cpp；人工复核绑定：candidate_review.cpp。
 *   内容归属/裁框/顺序：page_plan.cpp、region_structure.cpp。
 *   区域执行/取消：page_execution.cpp；会话/重试：region_recognition.cpp。
 *   内容校验/结果规范化：block_result.cpp、content_validation.cpp。
 *   DocumentIR/Markdown：document_export.cpp；身份命名：page_identity.cpp。
 *   本文件负责组合阶段、页面状态、预算和最终提交；改阶段算法时继续到对应模块。
 *
 * 五、重要约定
 *   1. 先规划再识别；模型返回的内容不能反过来改变已经确定的归属或顺序。
 *   2. 区域执行取消时不发布暂存输出；PP 版面推理早退可保留已有输入诊断资源。
 *   3. 超出输出预算时，不提交新生成的文档及暂存资源。
 *   4. 进度通知表示阶段进展；export_completed 之后仍可能因输出预算而失败。
 *   5. 本文件没有统一的异常捕获；阶段可转换已知错误，其余异常交调用者兜底。
 *      不能为了继续流程而把异常改成空候选，否则会把处理失败误判成空白页。
 *
 * 修改控制流程后可从 tests/page_pipeline.cpp 看取消、预算、异常与页面身份的验证，
 * 再运行页面计划、区域结果、导出及 PDF 等集成回归。
 */
#include "dococr/inference.hpp"
#include "page_identity.hpp"
#include "page_pipeline_context.hpp"
#include "layout_inference.hpp"
#include "layout_filter.hpp"
#include "candidate_review.hpp"
#include "page_plan.hpp"
#include "page_execution.hpp"
#include "document_export.hpp"
#include "image_resources.hpp"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace dococr {
namespace {
// steady_clock 不受系统时间调整影响，适合测量处理持续了多久。
// 匿名 namespace 中的工具和分支函数只在本文件内使用；公共入口在文件末尾。
using Clock = std::chrono::steady_clock;

// 从传入的起点算到现在，转换为整数毫秒；不足 1 毫秒的部分被舍去。
// 它只计算耗时，不累加 audit，也不修改任何执行状态。
uint64_t elapsed(Clock::time_point start) {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

// 将一批资源按原顺序追加到 target，不在这里排序、去重或检查输出预算。
// source 按值接收；调用方传 std::move(...) 时可移交原 vector 的缓冲区。
// move_iterator 使每个 Asset 的字符串/字节数组被移动，避免复制大图像数据。
// std::move 本身只是允许移动，真正的搬运发生在参数构造或后面的 insert 中。
void append_assets(std::vector<Asset>& target, std::vector<Asset> source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

/*
 * 两条路径共用的收尾：把执行结果变成文档，再决定是否提交给调用者。
 *
 * context 提供页面信息和配置；execution 提供已经处理的块及进度。
 * state 是调用分支算出的 blank / partial / ok；本函数不重新判断块状态。
 * layout 有值时导出版面候选、计划和诊断证据，旧接口传 nullptr 保留旧格式。
 * output 带入暂存资源；audit 带入阶段记录，两者按值接收以接管本次收尾的数据。
 * export_document_page 实现具体序列化，本函数管理进度、预算和最终返回码。
 */
RunResult export_page(const PagePipelineContext& context, const PageExecutionResult& execution,
    const std::string& state, const PageLayoutExport* layout, JobOutput output, RunResult audit) {
    // 1. 通知导出开始，使用执行阶段已统计的区域完成数/总数。
    const auto start = Clock::now();
    if (context.progress) context.progress("export_started", context.identity.progress_page(), "",
                                          execution.region_done, execution.region_total);
    // 2. 同一份块数据同时生成 DocumentIR JSON 和 Markdown，避免两个导出各自拼装。
    // document_id 从原图像素和尺寸计算，页面身份则区分单图与 PDF 页号。
    // 传入的块及 layout 被同步读取；导出不再推理，也不重算识别前页面计划。
    auto document = export_document_page({{context.image.width, context.image.height,
        context.identity.source_page(), document_id(context.image)},
        execution.blocks, state, context.backend.profile(), layout});
    output.json = std::move(document.json);
    output.markdown = std::move(document.markdown);
    // 3. 此时只说明序列化完成，还没有提交 output。回调之后仍要检查输出预算。
    // export_ms 从本函数起点开始，包含这里的进度回调，不是模型推理耗时。
    audit.did_export = true;
    if (context.progress) context.progress("export_completed", context.identity.progress_page(), "",
                                          execution.region_done, execution.region_total);
    audit.export_ms = elapsed(start);
    // 4. 按实际载荷字节计算：JSON + Markdown + 每份资源的字节数组。
    // Asset::png 是历史字段名，资源也可能是 f32/i32/RLE，全部计入预算。
    // 这是生成后的大小检查，并非生成前的内存预估；不要把二者混为一谈。
    uint64_t bytes = output.json.size() + output.markdown.size();
    for (const auto& asset : output.assets) bytes += asset.png.size();
    if (context.plan && bytes > context.plan->max_output_bytes) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "output_bytes";
        // output 是本函数的局部暂存数据，直接返回 audit 不会提交它。
        // audit 中仍保留已经执行的阶段标记和耗时，包括 did_export=true。
        return audit;
    }
    // 5. 预算通过：把页面状态映射为公共返回码，并一次性提交完整输出。
    // 这是两条路径的新文档和暂存资源进入最终 RunResult 的位置。
    audit.code = state == "blank" ? RunCode::Blank : state == "partial" ? RunCode::Partial : RunCode::Ok;
    audit.output = std::move(output);
    return audit;
}

/*
 * PP-DocLayoutV3 路径：保留版面证据，先规划页面，再执行区域识别。
 *
 * context.plan 是模型/预算等执行配置；下面的 page_plan 是这张图的页面计划。
 * 两者职责不同：配置决定允许怎样执行，页面计划决定哪些内容由哪些区域负责。
 * infer_doclayout 和 execute_planned_regions 可提前返回；此时沿用阶段的 audit，
 * 不继续制造文档。整个执行完成后，部分块失败/跳过仍可导出 partial 文档。
 */
RunResult run_doclayout_pipeline(const PagePipelineContext& context, RunResult audit) {
    const auto* plan = context.plan;
    // 1. 决定版面分析之后是否转写内容。组合后端和 printed_page 测试支持转写，
    // 纯 PP 版面后端不转写。后端名称沿用既有契约；Android 可适配 Ovis/GLM。
    const bool transcribe = plan && (plan->backend == "mnn:pp-doclayout-v3+ovisocr2" ||
        plan->backend.rfind("fixture:printed_page", 0) == 0);
    // 2. 执行能力检查、预处理、模型请求、输出契约检查及坐标回映。
    // 模型看到的可能是缩放/补边后的图，解码模块将检测框换算回原图坐标，
    // 后续页面计划和裁图才能使用同一套原图几何，不能直接裁模型输入的坐标。
    // 返回值拥有原始输出张量、预处理变换和预览图，后续 mask/叠加图仍要用它们。
    // 没有值意味着阶段已经写好失败/取消/不支持的 audit，不能当作“零候选”。
    auto layout = infer_doclayout(context, transcribe, audit);
    if (!layout) return audit;
    // 3. raster 只借用原始张量及坐标变换，用于把模型 mask 映射到原图。
    // 下面只移走 candidates，原始张量仍归 layout->output 所有。
    // layout 必须活到区域执行和导出结束；提前销毁它会使 raster 中的引用失效。
    const image_resources::LayoutRasterSource raster{
        context.image.width, context.image.height, layout->output, layout->transform};
    // 4. 处理重复、包含和重叠等筛选规则。records 保留被排除候选及原因，
    // 后续通过 selected 取出参与规划的候选。候选 ID 后续会用于索引 records，
    // 不可随意删除或重排 records 中的条目，否则会破坏 mask、归属和区域的对应。
    auto records = filter_layout_candidates(context.image.width, context.image.height,
                                           std::move(layout->output.candidates));
    // 5. 可选的显式复核：绑定当前页面像素、候选与 mask，核验复核决定并产生证据。
    // 复核可以改变候选的 selected/处理原因，所以一定要放在 plan_page 之前。
    // 未开启时 review 保持空值，不产生额外复核资源。
    CandidateReviewResult review;
    if (plan && plan->collect_candidate_reviews) {
        review = review_layout_candidates(plan->layout_candidate_reviews,
            context.image, raster, context.identity, records);
        append_assets(audit.output.assets, std::move(review.assets));
    }
    // 6. PP 的 layout_completed 在解码、筛选和复核完成后发出。
    // 此时尚未冻结计划或执行识别；回调若触发取消，后续执行阶段会检查取消标志。
    audit.did_layout = true;
    if (context.progress) context.progress("layout_completed", context.identity.progress_page(), "", 0, 0);
    // 7. 保存可追溯的模型输出：候选行 f32、有效数量 i32、mask 的 RLE 字节。
    // 版面输入的三个诊断张量已由 infer_doclayout 写入 audit.output.assets。
    // RLE 是 mask 的游程编码，避免把整个原始 mask 张量直接作为资源输出。
    for (const auto& tensor : layout->output.tensors()) {
        if (tensor.name == "fetch_name_0")
            audit.output.assets.push_back({context.identity.page_asset("fetch_name_0.f32"), tensor.data});
        else if (tensor.name == "fetch_name_1")
            audit.output.assets.push_back({context.identity.page_asset("fetch_name_1.i32"), tensor.data});
    }
    audit.output.assets.push_back({context.identity.page_asset("fetch_name_2.rle"), layout->output.masks_rle()});
    // 8. 只把 selected 候选交给页面规划，其他候选留在 records 中供诊断导出。
    // 这里的 transcribe 对应 PagePlanInput::resolve_content_ownership：
    // 内容识别时解析归属合并；纯版面输出关闭这项合并，不代表计划模块调用了 VLM。
    PagePlanInput planning_input{context.image.width, context.image.height,
                                context.identity.source_page(), transcribe, {}};
    for (const auto& candidate : records)
        if (candidate.selected) planning_input.candidates.push_back(candidate);
    // const 明确计划冻结：之后按计划创建区域和识别，不能按识别结果再修改顺序。
    // 归属、扩展裁框、图片分组和阅读/执行顺序的实现继续看 page_plan.cpp。
    const PreRecognitionPagePlan page_plan = plan_page(planning_input);
    // 9. 开始区域执行之前，把已有诊断资源移入本地 output，作为待提交资源。
    // 之后执行早退时返回的是 execution.audit，不会发布 output 中的资源。
    // 这与第 2 步版面推理失败可返回已有输入诊断资源的规则不同，不能统一清空。
    JobOutput output;
    output.assets = std::move(audit.output.assets);
    // 10. 执行模块先创建块、mask 和裁图，再按 recognition_order 调用识别模块。
    // records 会更新资源引用/归属原因/实际裁框诊断；page_plan 保持不变。
    // 识别的会话重置和重试在 region_recognition，内容处理在 block_result。
    auto execution = execute_planned_regions(context, page_plan, records, raster, transcribe, std::move(audit));
    // completed=false 表示取消等原因导致提前退出；audit 已包含错误优先级的判定。
    // 例如先有区域失败再取消，返回码可能为 Failed，不必一定是 Cancelled。
    if (!execution.completed) return std::move(execution.audit);
    // 11. 执行遍历结束：取回更新后的运行记录，追加该阶段生成的 mask/区域裁图。
    // completed=true 仍允许块状态为 failed、partial 或 skipped；
    // 模型返回的 truncated 是生成结束原因，经结果处理后可变成块的 partial。
    audit = std::move(execution.audit);
    append_assets(output.assets, std::move(execution.assets));
    // 12. 在区域诊断更新之后生成版面叠加图，资源名和导出引用使用同一个页面身份。
    // 智能缩放路径会使用 layout->preview，具体像素绘制在 image_resources。
    const auto overlay_name = context.identity.page_asset("layout-overlay.png");
    output.assets.push_back({overlay_name,
        image_resources::layout_overlay_png(context.image, records, raster, layout->preview)});
    // 13. 汇总整页状态。任意块不是 ok，都标记为不完整，不只检查 failed。
    // records 真正为空 -> blank；有候选但全部筛除/复核跳过 -> partial；
    // 有块且全部 ok -> ok；其余 -> partial。不能用 blocks.empty() 直接判空白页。
    const bool incomplete = std::any_of(execution.blocks.begin(), execution.blocks.end(),
        [](const DocumentBlock& block) { return block.result.status != "ok"; });
    const std::string state = records.empty() ? "blank" :
        (execution.blocks.empty() || incomplete) ? "partial" : "ok";
    // 14. 汇总 PP 专有导出证据，包括完整候选、冻结计划、变换、标签策略和复核。
    // layout_export 借用局部变量，同步 export_page 返回之前这些变量都必须有效。
    // 将来若改异步导出，需要重新设计所有权，不能直接把这些引用交给后台任务。
    const LabelExportPolicy label_export = plan ? plan->label_export : LabelExportPolicy{};
    const PageLayoutExport layout_export{records, page_plan, layout->transform, label_export,
        overlay_name, plan ? plan->layout_score_threshold : .5, review.evidence};
    return export_page(context, execution, state, &layout_export, std::move(output), std::move(audit));
}

/*
 * 旧 layout_candidates 接口的兼容流程，主要服务测试。
 * 与 PP 路径的输出契约、错误处理和导出格式不同，因此保留单独的组合函数。
 * 此路径没有 PP 的 mask、候选复核或识别前页面计划，不能套用 PP 类别映射。
 */
RunResult run_generic_pipeline(const PagePipelineContext& context, RunResult audit) {
    // 1. 请求布局，检查所有候选的类别和整页裁框，再按几何顺序排序。
    // 非法输出保留旧的 fresh Failed 行为，会重置运行记录；布局异常向调用者传播。
    // 旧接口 layout_completed 在最终候选校验之前通知，时机也不同于 PP 路径。
    auto candidates = infer_generic_layout(context, audit);
    if (!candidates) return audit;
    // 2. 按已排序候选逐个创建和执行区域；图片保存资源，文字/公式/表格请求识别。
    // 保留单次 reset/execute 及旧错误规则，不直接套用 PP 的重试流程。
    auto execution = execute_generic_regions(context, *candidates, std::move(audit));
    if (!execution.completed) return std::move(execution.audit);
    // 3. 旧路径空候选就是 blank；非空候选中任意块不是 ok，整页即 partial。
    // PP 的“有候选但全部筛除”情形不能移用到这里，因为这里没有该筛选阶段。
    const bool partial = std::any_of(execution.blocks.begin(), execution.blocks.end(),
        [](const DocumentBlock& block) { return block.result.status != "ok"; });
    const std::string state = candidates->empty() ? "blank" : partial ? "partial" : "ok";
    // 4. 取出暂存资源和运行记录，交共用导出函数检查预算并提交。
    // nullptr 表示没有 PP 版面证据，导出模块据此选择兼容的文档结构。
    JobOutput output;
    output.assets = std::move(execution.assets);
    audit = std::move(execution.audit);
    return export_page(context, execution, state, nullptr, std::move(output), std::move(audit));
}
} // namespace

/*
 * 单页公共入口，声明在 include/dococr/inference.hpp；直接由 pipeline 实现。
 *
 * backend：调用者已经创建/加载的推理后端，本函数不加载或销毁模型。
 * input：借用输入字节、格式、尺寸和步长，decode 将其变成拥有 RGB 数据的 Image。
 * cancelled：调用者可设置的原子取消标志，传给后续阶段及模型执行。
 * plan：只读执行配置，允许 nullptr；没有配置时沿用旧接口路径及默认解码上限。
 * source_page：0 表示单张图片，PDF 页号从 1 开始；影响块/资源/请求的命名。
 * progress：可选同步阶段回调，由调用者转换为 App 或 C ABI 作业进度事件。
 *
 * 返回 RunResult：既包含执行状态与运行记录，也可能包含成功提交的输出，
 * 或 PP 布局早退留下的输入诊断资源。仅有某个 did_* 标记不能判断最终成功。
 */
RunResult run_page(IInferenceEngine* backend, InputView input, std::atomic_bool& cancelled,
    const ExecutionPlan* plan, uint32_t source_page, const ProgressCallback& progress) {
    // 1. 页面身份按本次调用创建，避免全局页号导致嵌套/并行调用互相污染。
    // 初始状态是 Failed，后续只有明确走完对应阶段才设置其他返回码。
    const PageIdentity identity(source_page);
    RunResult audit{RunCode::Failed, {}};
    const auto start = Clock::now();
    // 2. 按配置选择契约，同时选择输入解码硬上限：PP 为 6400 万像素，旧接口为
    // 1600 万像素。PP 允许较大原图，再在版面预处理阶段按配置缩放。
    // 这一步选择路径，不是按图像内容猜测模型；扩展后端时需核对 uses_doclayout。
    const bool layout_job = plan && plan->uses_doclayout();
    auto decoded_image = image_resources::decode(input,
        layout_job ? max_layout_source_pixels : max_standard_page_pixels);
    // decode 同时检查输入格式、步长、尺寸和解码硬上限。
    // 解码返回空值统一映射为 InputError；这里不会区分成配置预算错误。
    if (!decoded_image) return {RunCode::InputError, {}};
    // 3. 解码成功后记录像素数和进度。先转成 uint64_t 再乘，避免 32 位乘法溢出。
    // decode_ms 从入口计时起点算起，包含上面的路径准备及 decode_completed 回调。
    audit.did_decode = true;
    if (progress) progress("decode_completed", identity.progress_page(), "", 0, 0);
    audit.page_pixels = uint64_t(decoded_image->width) * decoded_image->height;
    audit.decode_ms = elapsed(start);
    // 4. 再检查本作业的配置预算；它与解码硬上限不同，错误为 BudgetExceeded。
    // decode_completed 只表示解码完成，所以即使发出该事件，仍可能在这里早退。
    if (plan && audit.page_pixels > plan->max_page_pixels) {
        audit.code = RunCode::BudgetExceeded;
        audit.budget_stage = "input_pixels";
        return audit;
    }
    // 5. 解码和预算检查之后才检查后端。返回一个新 Unsupported 结果，沿用原规则；
    // 不会把上面的 did_decode 等标记复制到这个新结果中。
    if (!backend) return {RunCode::Unsupported, {}};
    // 6. context 借用本函数局部的图像/身份，以及调用者的后端/配置/取消标志/回调。
    // 两条路径都是同步执行，局部对象会活到它们返回，引用在此期间有效。
    const PagePipelineContext context{*backend, *decoded_image, identity, plan, cancelled, progress};
    // 7. 分支函数直接返回最终结果，入口不再额外转发或修改页面状态。
    // 页开始/结束事件、PDF 多页处理、模型加载恢复等由外层作业管理负责。
    return layout_job ? run_doclayout_pipeline(context, std::move(audit)) :
                        run_generic_pipeline(context, std::move(audit));
}
} // namespace dococr
