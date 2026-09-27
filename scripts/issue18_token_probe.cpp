#include "llm/llm.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>

using MNN::Transformer::Llm;
constexpr int image_pad_token = 248056;

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: issue18_token_probe CONFIG IMAGE PROMPT RAW_OUTPUT\n";
        return 2;
    }
    std::ifstream prompt_file(argv[3], std::ios::binary);
    if (!prompt_file) return 2;
    std::string prompt((std::istreambuf_iterator<char>(prompt_file)), {});
    while (!prompt.empty() && prompt.back() == '\n') prompt.pop_back();
    std::unique_ptr<Llm, void(*)(Llm*)> llm(Llm::createLLM(argv[1]), Llm::destroy);
    if (!llm || !llm->load()) return 3;
    const std::string user = std::string("<img>") + argv[2] + "</img>" + prompt;
    std::string query = llm->apply_chat_template(user);
    if (query.empty()) query = user;
    const auto ids = llm->tokenizer_encode(query);
    const auto count = std::count(ids.begin(), ids.end(), image_pad_token);
    const auto* state = llm->getContext();
    std::cout << "visual_tokens=" << count << " vision_us=" << state->vision_us
              << " pixels_mp=" << state->pixels_mp << '\n';
    if (count == 0) return 4;
    std::ostringstream raw;
    llm->response(ids, &raw, nullptr, 512);
    std::ofstream output(argv[4], std::ios::binary);
    output << raw.str();
    std::cout << "status=" << int(llm->getContext()->status) << '\n';
    return output ? 0 : 5;
}
