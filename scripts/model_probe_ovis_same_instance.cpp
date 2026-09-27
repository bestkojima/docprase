#include "llm/llm.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

using MNN::Transformer::Llm;
using MNN::Transformer::LlmStatus;

static const char* status_name(LlmStatus status) {
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
    if (argc != 7) {
        std::cerr << "用法：model_probe_ovis_same_instance CONFIG PROMPT OUTPUT_DIR B A BAD\n";
        return 2;
    }
    std::ifstream prompt_file(argv[2], std::ios::binary);
    std::string prompt((std::istreambuf_iterator<char>(prompt_file)), {});
    while (!prompt.empty() && prompt.back() == '\n') prompt.pop_back();
    std::unique_ptr<Llm, void(*)(Llm*)> llm(Llm::createLLM(argv[1]), Llm::destroy);
    if (!llm || !llm->load()) return 3;
    std::ofstream loaded_config(std::string(argv[3]) + "/loaded-config.json", std::ios::binary);
    loaded_config << llm->dump_config(); loaded_config.close();
    if (!loaded_config) return 4;
    const char* images[] = {argv[4], argv[5], argv[4], argv[6], argv[4]};
    for (int i = 0; i < 5; ++i) {
        llm->reset();
        std::ostringstream raw;
        llm->response(std::string("<img>") + images[i] + "</img>" + prompt, &raw, nullptr, 512);
        std::ofstream output(std::string(argv[3]) + "/raw-" + std::to_string(i) + ".txt", std::ios::binary);
        output << raw.str(); output.close();
        const auto* state = llm->getContext();
        std::cout << "OVIS_SAME_INSTANCE {\"index\":" << i << ",\"status\":\""
                  << status_name(state->status) << "\",\"tokens\":" << state->gen_seq_len
                  << ",\"vision_us\":" << state->vision_us
                  << ",\"pixels_mp\":" << state->pixels_mp << "}" << std::endl;
        if (!output) return 4;
    }
    return 0;
}
