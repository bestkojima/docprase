#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc,char**argv) {
    if (argc != 3) return 2;
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    int w=0,h=0,n=0;
    unsigned char* rgb=stbi_load_from_memory(bytes.data(),int(bytes.size()),&w,&h,&n,3);
    if (!rgb) return 3;
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<char*>(rgb),size_t(w)*h*3);
    stbi_image_free(rgb);
    return output ? 0 : 4;
}
