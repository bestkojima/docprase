#include "layout_preprocess.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) return 2;
    if (argc == 6 && std::string(argv[5]) != "letterbox" && std::string(argv[5]) != "area" &&
        std::string(argv[5]) != "lanczos") return 2;
    dococr::Image image;
    image.width = std::atoi(argv[1]);
    image.height = std::atoi(argv[2]);
    if (image.width < 1 || image.height < 1 || uint64_t(image.width)*image.height > 64000000) return 3;
    image.rgb.resize(size_t(image.width)*image.height*3);
    std::ifstream source(argv[3], std::ios::binary);
    source.read(reinterpret_cast<char*>(image.rgb.data()), image.rgb.size());
    if (!source || source.peek() != EOF) return 4;
    auto tensor = argc == 6 ?
        dococr::prepare_layout_page(image, true, std::string(argv[5]) == "area" ? dococr::LayoutResample::Area :
            std::string(argv[5]) == "lanczos" ? dococr::LayoutResample::Lanczos : dococr::LayoutResample::Bilinear).tensor :
        dococr::layout_image_tensor(image);
    std::ofstream output(argv[4], std::ios::binary);
    output.write(reinterpret_cast<const char*>(tensor.data.data()), tensor.data.size());
    return output ? 0 : 5;
}
