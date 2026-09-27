#include "layout_preprocess.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    dococr::Image image;
    image.width = std::atoi(argv[1]);
    image.height = std::atoi(argv[2]);
    if (image.width < 1 || image.height < 1 || image.width > 4000 || image.height > 4000) return 3;
    image.rgb.resize(size_t(image.width)*image.height*3);
    std::ifstream source(argv[3], std::ios::binary);
    source.read(reinterpret_cast<char*>(image.rgb.data()), image.rgb.size());
    if (!source || source.peek() != EOF) return 4;
    auto tensor = dococr::layout_image_tensor(image);
    std::ofstream output(argv[4], std::ios::binary);
    output.write(reinterpret_cast<const char*>(tensor.data.data()), tensor.data.size());
    return output ? 0 : 5;
}
