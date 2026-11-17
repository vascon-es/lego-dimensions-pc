#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// CUSA00935 discovery adapted from JonyGOi/lego-dimensions-pc, commit 0aeb8b9.
// Read metadata rather than guessing the region from a renamed folder.
namespace game_region {
inline std::string TitleId(const std::filesystem::path& game) {
    std::ifstream file(game / "sce_sys" / "param.sfo", std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size < 20 || size > 1024 * 1024) return {};
    std::vector<unsigned char> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), size) || std::memcmp(data.data(), "\0PSF", 4)) return {};
    const auto u32 = [&](size_t at) { uint32_t value; std::memcpy(&value, data.data() + at, 4); return value; };
    const auto u16 = [&](size_t at) { uint16_t value; std::memcpy(&value, data.data() + at, 2); return value; };
    const size_t keys = u32(8), values = u32(12), count = u32(16);
    if (u32(4) != 0x101 || !count || count > 512 || 20 + 16 * count > keys || keys > values || values > data.size()) return {};
    std::string result;
    for (size_t i = 0; i < count; ++i) {
        const size_t row = 20 + 16 * i, key = keys + u16(row), length = u32(row + 4), capacity = u32(row + 8), offset = u32(row + 12);
        if (key >= values || length > capacity || offset > data.size() - values || capacity > data.size() - values - offset) return {};
        const auto end = static_cast<const unsigned char*>(std::memchr(data.data() + key, 0, values - key));
        if (!end) return {};
        if (std::string(reinterpret_cast<const char*>(data.data() + key), end - data.data() - key) != "TITLE_ID") continue;
        if (!result.empty() || u16(row + 2) != 0x204 || length != 10 || data[values + offset + 9]) return {};
        result.assign(reinterpret_cast<const char*>(data.data() + values + offset), 9);
        if (result != "CUSA01176" && result != "CUSA00935") return {};
    }
    return result;
}

inline std::filesystem::path FindPatch(const std::filesystem::path& game) {
    const auto tid = TitleId(game);
    if (tid.empty()) return {};
    const auto name = game.filename().wstring() + L"-patch";
    for (const auto& path : {game / name, game.parent_path() / name, game / (tid + "-patch"), game.parent_path() / (tid + "-patch")})
        if (TitleId(path) == tid) return path;
    return {};
}

inline std::filesystem::path Addcont(const std::filesystem::path& game, const std::filesystem::path& appdata) {
    const auto tid = TitleId(game);
    if (tid.empty() || appdata.empty()) return {};
    const auto path = appdata / "shadPS4" / "addcont" / tid;
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) ? path : std::filesystem::path{};
}

// ---- pacotes de DLC instalados ----------------------------------------------------------------------------------
// 4.5 (multiplayer): o jogo do convidado pode nao ter o mundo da DLC liberado no savedata0 dele. O que decide se o
// pacote existe e o addcont (o mesmo que o HLE de libSceAppContent monta em /addcontN), nao o save.
struct AddcontPack {
    std::string label; // rotulo do conteudo: os 16 caracteres depois do product-id do CONTENT_ID
    std::filesystem::path folder;
};

// Valor de texto de uma chave do param.sfo (formato PSF). Vazio = arquivo invalido ou chave ausente.
inline std::string SfoString(const std::filesystem::path& file, const std::string& wanted) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size < 20 || size > 1024 * 1024) return {};
    std::vector<unsigned char> data(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), size) || std::memcmp(data.data(), "\0PSF", 4)) return {};
    const auto u32 = [&](size_t at) { uint32_t value = 0; std::memcpy(&value, data.data() + at, 4); return value; };
    const auto u16 = [&](size_t at) { uint16_t value = 0; std::memcpy(&value, data.data() + at, 2); return value; };
    if (u32(4) != 0x101) return {};
    const size_t keys = u32(8), values = u32(12), count = u32(16);
    if (!count || count > 512 || 20 + 16 * count > keys || keys > values || values > data.size()) return {};
    for (size_t i = 0; i < count; ++i) {
        const size_t row = 20 + 16 * i, key = keys + u16(row), length = u32(row + 4), offset = u32(row + 12);
        if (key >= values || offset > data.size() - values || length > data.size() - values - offset) continue;
        const auto end = static_cast<const unsigned char*>(std::memchr(data.data() + key, 0, values - key));
        if (!end) continue;
        if (std::string(reinterpret_cast<const char*>(data.data() + key), end - data.data() - key) != wanted) continue;
        std::string value(reinterpret_cast<const char*>(data.data() + values + offset), length);
        value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
        return value;
    }
    return {};
}

// Pastas de addcont com conteudo de add-on (CATEGORY "ac..."), como o libSceAppContent do port aceita.
inline std::vector<AddcontPack> AddcontPacks(const std::filesystem::path& addcont_root) {
    std::vector<AddcontPack> packs;
    std::error_code ec;
    if (addcont_root.empty() || !std::filesystem::is_directory(addcont_root, ec)) return packs;
    for (std::filesystem::directory_iterator it(addcont_root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code item_ec;
        if (!it->is_directory(item_ec)) continue;
        const auto sfo = it->path() / "sce_sys" / "param.sfo";
        const std::string category = SfoString(sfo, "CATEGORY");
        const std::string content = SfoString(sfo, "CONTENT_ID");
        if (category.compare(0, 2, "ac") != 0 || content.size() <= 20) continue;
        packs.push_back({content.substr(20, 16), it->path()});
    }
    std::sort(packs.begin(), packs.end(), [](const AddcontPack& a, const AddcontPack& b) { return a.folder < b.folder; });
    return packs;
}

// Ha DLC instalado para este jogo? (a pasta do shadPS4 ou a pasta do port, como o main.cpp procura)
inline bool HasAddcontContent(const std::filesystem::path& game, const std::filesystem::path& appdata) {
    if (!AddcontPacks(Addcont(game, appdata)).empty()) return true;
    return !AddcontPacks(game / "assets" / "addcont").empty() ||
           !AddcontPacks(game.parent_path() / "assets" / "addcont").empty();
}
} // namespace game_region
