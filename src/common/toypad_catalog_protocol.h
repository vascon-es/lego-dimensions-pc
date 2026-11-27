#pragma once

#include <array>
#include <cstdint>

namespace toypad_catalog {

constexpr int kSlots = 7;
constexpr int kPort = 9191;
constexpr std::uint8_t kMove = 3, kSnapshot = 5, kSelect = 6;
constexpr wchar_t kWindowClass[] = L"LegoDimensionsToypadCatalog";
constexpr wchar_t kGameWindowClass[] = L"LegoDimensionsPcWindow";
constexpr wchar_t kInputOwnerProperty[] = L"LegoDimensionsToypadCatalog.InputTarget";
constexpr unsigned kShowMessage = 0x8000 + 73;
constexpr unsigned kOpenRequest = 0x8000 + 74;

using Header = std::array<std::uint8_t, 5>;
using Snapshot = std::array<std::uint8_t, 2 + kSlots * 5>;

inline std::uint32_t ReadId(const std::uint8_t* data) {
    return data[0] | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

inline void WriteId(std::uint8_t* data, std::uint32_t id) {
    for (int i = 0; i < 4; ++i) data[i] = static_cast<std::uint8_t>(id >> (i * 8));
}

} // namespace toypad_catalog
