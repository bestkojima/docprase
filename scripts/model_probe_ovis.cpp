#include "llm/llm.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using MNN::Transformer::Llm;
using MNN::Transformer::LlmStatus;

static const char* statusName(LlmStatus status) {
    switch (status) {
        case LlmStatus::NORMAL_FINISHED: return "NORMAL_FINISHED";
        case LlmStatus::MAX_TOKENS_FINISHED: return "MAX_TOKENS_FINISHED";
        case LlmStatus::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case LlmStatus::TIMEOUT: return "TIMEOUT";
        case LlmStatus::USER_CANCEL: return "USER_CANCEL";
        case LlmStatus::RUNNING: return "RUNNING";
        case LlmStatus::NOT_LOADED: return "NOT_LOADED";
    }
    return "UNKNOWN";
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: model_probe_ovis CONFIG IMAGE PROMPT MAX_TOKENS RAW_OUTPUT\n";
        return 2;
    }
    const int maxTokens = std::stoi(argv[4]);
    if (maxTokens < 1) return 2;
    std::ofstream raw(argv[5], std::ios::binary);
    if (!raw) return 2;
    std::unique_ptr<Llm, void (*)(Llm*)> llm(Llm::createLLM(argv[1]), Llm::destroy);
    if (!llm || !llm->load()) {
        std::cerr << "模型加载失败\n";
        return 3;
    }
    const std::string query = std::string("<img>") + argv[2] + "</img>" + argv[3];
    llm->response(query, &raw, nullptr, maxTokens);
    raw.flush();
    const auto* context = llm->getContext();
    std::cout << "OVIS_PROBE {\"status\":\"" << statusName(context->status)
              << "\",\"tokens\":" << context->gen_seq_len
              << ",\"vision_us\":" << context->vision_us
              << ",\"pixels_mp\":" << context->pixels_mp << "}" << std::endl;
    return context->status == LlmStatus::INTERNAL_ERROR ? 4 : 0;
}
