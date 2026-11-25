#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <vector>

// Atlas BC3 e tabela de glifos do arquivo .ft2 do jogo.
namespace nu_font {

struct NuFont {
    struct Glyph {
        float x, y, w;
    };
    std::vector<Glyph> glyphs;
    std::map<wchar_t, int> chars;
    float line = 0;
    int width = 0, height = 0;
    size_t blocks = 0;
};

inline uint32_t Be32(const std::span<const uint8_t>& d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3];
}
inline float BeFloat(const std::span<const uint8_t>& d, size_t at) {
    const uint32_t bits = Be32(d, at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
inline uint32_t Le32(const std::span<const uint8_t>& d, size_t at) {
    return d[at] | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}

inline bool Parse(std::span<const uint8_t> d, NuFont* font) {
    if (d.size() < 0x60 || std::memcmp(d.data() + 8, "TNFN", 4) != 0) {
        return false;
    }
    size_t r1 = 0x10;
    while (r1 + 8 <= d.size() && std::memcmp(d.data() + r1, "ROTV", 4) != 0) ++r1;
    if (r1 + 8 > d.size() || r1 < 0x24) {
        return false;
    }
    const uint32_t n1 = Be32(d, r1 + 4);
    const size_t r2 = r1 + 8 + size_t(n1) * 12;
    if (n1 == 0 || n1 > 65536 || r2 + 8 > d.size() || std::memcmp(d.data() + r2, "ROTV", 4) != 0) {
        return false;
    }
    const uint32_t n2 = Be32(d, r2 + 4);
    const size_t dds = 4 + size_t(Be32(d, 0));
    if (r2 + 8 + size_t(n2) * 4 > d.size() || dds + 128 > d.size() || std::memcmp(d.data() + dds, "DDS ", 4) != 0 ||
        std::memcmp(d.data() + dds + 84, "DXT5", 4) != 0) {
        return false;
    }
    font->line = BeFloat(d, r1 - 0x14);
    font->height = int(Le32(d, dds + 12));
    font->width = int(Le32(d, dds + 16));
    if (!(font->line > 4 && font->line < 1024) || font->width < 4 || font->height < 4 || font->width > 8192 || font->height > 8192 ||
        (font->width % 4 != 0) || (font->height % 4 != 0) ||
        dds + 128 + size_t(font->width) * font->height > d.size()) {
        return false;
    }
    for (uint32_t i = 0; i < n1; ++i) {
        const NuFont::Glyph glyph{BeFloat(d, r1 + 8 + i * 12), BeFloat(d, r1 + 12 + i * 12), BeFloat(d, r1 + 16 + i * 12)};
        if (!(glyph.x >= 0 && glyph.y >= 0 && glyph.w >= 0 && glyph.x + glyph.w <= font->width &&
              glyph.y + font->line <= font->height)) return false;
        font->glyphs.push_back(glyph);
    }
    for (uint32_t i = 0; i < n2; ++i) {
        const wchar_t c = wchar_t((d[r2 + 8 + i * 4] << 8) | d[r2 + 9 + i * 4]);
        const int g = (d[r2 + 10 + i * 4] << 8) | d[r2 + 11 + i * 4];
        if (c > 32 && g < int(n1)) font->chars[c] = g;
    }
    font->blocks = dds + 128;
    return true;
}

} // namespace nu_font
