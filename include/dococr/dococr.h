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
    DOCOCR_NO_RESULT = 8
} DocOcrStatus;

typedef enum {
    DOCOCR_IMAGE_PNG = 1,
    DOCOCR_IMAGE_JPEG = 2,
    DOCOCR_IMAGE_RGB8 = 3,
    DOCOCR_IMAGE_GRAY8 = 4
} DocOcrImageFormat;

typedef struct {
    uint32_t struct_size;
    const uint8_t* data;
    size_t size;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    size_t row_stride;
} DocOcrInput;

typedef struct {
    uint32_t struct_size;
    DocOcrBytes json;
    DocOcrBytes markdown;
} DocOcrResult;

DOCOCR_API uint32_t dococr_abi_version(void);
/* config 使用明确长度的 UTF-8；本阶段唯一生产配置为 "none"。 */
DOCOCR_API DocOcrStatus dococr_create(DocOcrStringView config, DocOcrHandle* out);
DOCOCR_API DocOcrStatus dococr_capabilities(DocOcrHandle engine, DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_job_create(DocOcrHandle engine, DocOcrJob* out);
/* 同步运行；推理前复制输入图像。调用方须保持缓冲区有效直至函数返回。 */
DOCOCR_API DocOcrStatus dococr_job_run(DocOcrJob job, const DocOcrInput* input);
DOCOCR_API DocOcrStatus dococr_job_result(DocOcrJob job, DocOcrResult* out);
DOCOCR_API DocOcrStatus dococr_job_asset_count(DocOcrJob job, size_t* out_count);
DOCOCR_API DocOcrStatus dococr_job_asset(DocOcrJob job, size_t index,
                                         DocOcrBytes* out_name, DocOcrBytes* out_data);
DOCOCR_API DocOcrStatus dococr_job_cancel(DocOcrJob job);
DOCOCR_API DocOcrStatus dococr_job_poll_events(DocOcrJob job, DocOcrBytes* out_json);
DOCOCR_API DocOcrStatus dococr_job_destroy(DocOcrJob job);
DOCOCR_API DocOcrStatus dococr_destroy(DocOcrHandle engine);
/* 只能释放本 ABI 返回的字节；重复释放返回 INVALID_ARGUMENT。 */
DOCOCR_API DocOcrStatus dococr_bytes_free(DocOcrBytes* bytes);

#ifdef __cplusplus
}
#endif
#endif
