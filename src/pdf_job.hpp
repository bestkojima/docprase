#ifndef DOCOCR_PDF_JOB_HPP
#define DOCOCR_PDF_JOB_HPP
#include "dococr/inference.hpp"
#include <string>

namespace dococr {
struct PdfJobResult {
    RunResult run;
    std::string manifest_pdf;
};
PdfJobResult run_pdf(IInferenceEngine* backend, InputView input,
                     uint32_t first_page, uint32_t last_page, uint32_t dpi,
                     uint64_t request_max_pixels, std::atomic_bool& cancelled,
                     const ExecutionPlan* plan);
} // namespace dococr
#endif
