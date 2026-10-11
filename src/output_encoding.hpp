#pragma once
#include <cstdint>
#include <string>

namespace dococr::output_encoding {
// 保留原始字节；无效 UTF-8 通过 Base64 记录，不进入 JSON 文本字段。
inline bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
        if (!n || i + static_cast<size_t>(n) > s.size()) return false;
        unsigned char c1 = static_cast<unsigned char>(s[i+1]);
        if ((c1 & 0xC0) != 0x80 || (c == 0xE0 && c1 < 0xA0) ||
            (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) ||
            (c == 0xF4 && c1 >= 0x90)) return false;
        for (int j = 2; j < n; ++j)
            if ((static_cast<unsigned char>(s[i+j]) & 0xC0) != 0x80) return false;
        i += static_cast<size_t>(n);
    }
    return true;
}

inline std::string base64(const std::string& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t a = static_cast<uint8_t>(bytes[i]);
        uint32_t b = i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i+1]) : 0;
        uint32_t c = i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i+2]) : 0;
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return out;
}

}
