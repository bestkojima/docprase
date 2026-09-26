#include "config.hpp"
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dococr {
namespace {
uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
constexpr std::array<uint32_t, 64> k = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
class Hasher {
public:
    void update(const char* bytes, size_t size) {
        length_ += uint64_t(size);
        for (size_t i = 0; i < size; ++i) {
            block_[used_++] = uint8_t(bytes[i]);
            if (used_ == block_.size()) { compress(); used_ = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = length_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < 64) block_[used_++] = 0;
            compress(); used_ = 0;
        }
        while (used_ < 56) block_[used_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) block_[used_++] = uint8_t(bits >> shift);
        compress();
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint32_t word : h_) out << std::setw(8) << word;
        return out.str();
    }
private:
    std::array<uint8_t, 64> block_{};
    size_t used_ = 0;
    uint64_t length_ = 0;
    uint32_t h_[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    void compress() {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            size_t p = size_t(i) * 4;
            w[i] = (uint32_t(block_[p]) << 24) | (uint32_t(block_[p+1]) << 16) |
                   (uint32_t(block_[p+2]) << 8) | uint32_t(block_[p+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotate(w[i-15],7) ^ rotate(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotate(w[i-2],17) ^ rotate(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],q=h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
            uint32_t choice=(e&f)^(~e&g);
            uint32_t t1=q+s1+choice+k[i]+w[i];
            uint32_t s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
            uint32_t majority=(a&b)^(a&c)^(b&c);
            uint32_t t2=s0+majority;
            q=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=q;
    }
};
} // namespace
std::string sha256(const std::string& input) {
    Hasher hasher;
    hasher.update(input.data(), input.size());
    return hasher.finish();
}
std::string sha256_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw ConfigError("artifact_missing", path);
    Hasher hasher;
    char block[65536];
    while (file) {
        file.read(block, sizeof(block));
        hasher.update(block, size_t(file.gcount()));
    }
    if (!file.eof()) throw ConfigError("artifact_read_error", path);
    return hasher.finish();
}
} // namespace dococr
