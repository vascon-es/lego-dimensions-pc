#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace cutscene_patch {
// Adapted from JonyGOi commit 0aeb8b9. Scan only file-backed executable
// segments supplied by the loader. Missing/ambiguous matches leave all bytes
// unchanged; a fixed 48 MiB scan could read unmapped/non-code regions.
inline bool Apply(std::span<const std::span<uint8_t>> segments) {
    constexpr std::array<uint8_t, 8> original{0x8A,0x83,0xE8,0,0,0,0x24,1};
    constexpr std::array<uint8_t, 8> patched{0xB0,1,0x90,0x90,0x90,0x90,0x90,0x90};
    uint8_t* found = nullptr;
    for (const auto code : segments) {
        for (size_t offset = 0; offset <= code.size() && original.size() <= code.size() - offset; ++offset) {
            if (std::memcmp(code.data() + offset, original.data(), original.size())) continue;
            if (found) return false;
            found = code.data() + offset;
        }
    }
    if (!found) return false;
    std::memcpy(found, patched.data(), patched.size());
    return true;
}
} // namespace cutscene_patch
