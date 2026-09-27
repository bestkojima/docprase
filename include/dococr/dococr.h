#ifndef DOCOCR_DOCOCR_H
#define DOCOCR_DOCOCR_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DOCOCR_SHARED)
#if defined(DOCOCR_BUILDING)
#define DOCOCR_API __declspec(dllexport)
#else
#define DOCOCR_API __declspec(dllimport)
#endif
#else
#define DOCOCR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DOCOCR_ABI_VERSION 1u
typedef uint64_t DocOcrHandle;
typedef uint64_t DocOcrJob;
typedef struct { const char* data; size_t size; } DocOcrStringView;
typedef struct { uint8_t* data; size_t size; uint64_t allocation_id; } DocOcrBytes;

typedef enum {
    DOCOCR_OK = 0,
    DOCOCR_INVALID_ARGUMENT = 1,
    DOCOCR_INVALID_HANDLE = 2,
    DOCOCR_BUSY = 3,
    DOCOCR_UNSUPPORTED = 4,
    DOCOCR_INPUT_ERROR = 5,
    DOCOCR_FAILED = 6,
    DOCOCR_CANCELLED = 7,
    DOCOCR_NO_RESULT = 8,
    DOCOCR_CONFIG_ERROR = 9,
    DOCOCR_BUDGET_EXCEEDED = 10,
    DOCOCR_TIMEOUT = 11
} DocOcrStatus;

typedef enum {
    DOCOCR_IMAGE_PNG = 1,
    DOCOCR_IMAGE_JPEG = 2,
    DOCOCR_IMAGE_RGB8 = 3,
    DOCOCR_IMAGE_GRAY8 = 4,
    DOCOCR_DOCUMENT_PDF = 5
} DocOcrImageFormat;

typedef struct {
    uint32_t struct_size;
    const uint8_t* data;
    size_t size;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    size_t row_stride;
    /* 仅 PDF 使用。页号从 1 开始且含终点；0 表示默认（首页/末页）。 */
    uint32_t first_page;
    uint32_t last_page;
    uint32_t dpi;
    uint64_t max_page_pixels;
    /* 0 表示不设作业截止时间；超时只请求在安全边界停止。 */
    uint32_t timeout_ms;
} DocOcrInput;

typedef struct {
    uint32_t struct_size;
    DocOcrBytes json;
    DocOcrBytes markdown;
} DocOcrResult;

DOCOCR_API uint32_t dococr_abi_version(void);
/* config 使用明确长度的 UTF-8；接受旧后端名或 schema 1.0 JSON。 */
DOCOCR_API DocOcrStatus dococr_create(DocOcrStringView config, DocOcrHandle* out);
DOCOCR_API DocOcrStatus dococr_reconfigure(DocOcrHandle engine, DocOcrStringView config);
DOCOCR_API DocOcrStatus dococr_last_error(DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_execution_plan(DocOcrHandle engine, DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_capabilities(DocOcrHandle engine, DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_job_create(DocOcrHandle engine, DocOcrJob* out);
/* 同步运行；推理前复制输入图像。调用方须保持缓冲区有效直至函数返回。 */
DOCOCR_API DocOcrStatus dococr_job_run(DocOcrJob job, const DocOcrInput* input);
DOCOCR_API DocOcrStatus dococr_job_result(DocOcrJob job, DocOcrResult* out);
DOCOCR_API DocOcrStatus dococr_job_manifest(DocOcrJob job, DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_job_asset_count(DocOcrJob job, size_t* out_count);
DOCOCR_API DocOcrStatus dococr_job_asset(DocOcrJob job, size_t index,
                                         DocOcrBytes* out_name, DocOcrBytes* out_data);
DOCOCR_API DocOcrStatus dococr_job_cancel(DocOcrJob job);
DOCOCR_API DocOcrStatus dococr_job_poll_events(DocOcrJob job, DocOcrBytes* out_json);
/* 状态快照；terminal 为真才表示运行线程及后端清理均已结束。 */
DOCOCR_API DocOcrStatus dococr_job_status(DocOcrJob job, DocOcrBytes* out_json);
/* 取走最早事件；空队列返回 NO_RESULT。队列最多保留 64 个事件。 */
DOCOCR_API DocOcrStatus dococr_job_next_event(DocOcrJob job, DocOcrBytes* out_json);
/* 等待终态；等待超时返回 BUSY，0 表示只查询。 */
DOCOCR_API DocOcrStatus dococr_job_wait(DocOcrJob job, uint32_t timeout_ms);
DOCOCR_API DocOcrStatus dococr_job_destroy(DocOcrJob job);
DOCOCR_API DocOcrStatus dococr_destroy(DocOcrHandle engine);
/* 只能释放本 ABI 返回的字节；重复释放返回 INVALID_ARGUMENT。 */
DOCOCR_API DocOcrStatus dococr_bytes_free(DocOcrBytes* bytes);

#ifdef __cplusplus
}
#endif
#endif
