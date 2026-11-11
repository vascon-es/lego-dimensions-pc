#pragma once
// 3.0: os icones das conquistas saem do trophy00.trp do PROPRIO jogo do jogador (sce_sys\trophy), nao vem no download.
// No .trp do PS4 os PNG ficam abertos; so os ESFM (textos) sao cifrados, e esses o port nao usa (os nomes estao em
// trophy_defs.h). Header sem dependencias: usado pelo port (trophy_manager.cpp) e pelo launcher.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace trophy_trp {

inline std::uint32_t Be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

inline std::uint64_t Be64(const unsigned char* p) {
    return (std::uint64_t(Be32(p)) << 32) | Be32(p + 4);
}

// Copia os PNG do .trp para `out_dir` (os que ja existem com o mesmo tamanho ficam). Devolve quantos PNG o arquivo
// tem, ou -1 se nao e um .trp.
inline int ExtractIcons(const std::filesystem::path& trp, const std::filesystem::path& out_dir) {
    std::ifstream file(trp, std::ios::binary);
    unsigned char header[0x60];
    if (!file || !file.read(reinterpret_cast<char*>(header), sizeof(header)) || Be32(header) != 0xDCA24D00u) {
        return -1;
    }
    const std::uint32_t count = Be32(header + 0x10), entry_size = Be32(header + 0x14);
    if (entry_size < 0x40 || entry_size > 0x400 || count == 0 || count > 4096) {
        return -1;
    }
    std::vector<unsigned char> entries(std::size_t(count) * entry_size);
    if (!file.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size()))) {
        return -1;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    int pngs = 0;
    std::vector<char> data;
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = entries.data() + std::size_t(i) * entry_size;
        std::string name(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 32));
        if (name.size() < 5 || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) {
            continue;
        }
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) {
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        const std::uint64_t pos = Be64(e + 0x20), len = Be64(e + 0x28);
        if (ext != ".PNG" || len < 8 || len > (16u << 20)) {
            continue;
        }
        ++pngs;
        const std::filesystem::path dst = out_dir / name;
        if (std::filesystem::file_size(dst, ec) == len && !ec) {
            continue;
        }
        data.resize(static_cast<std::size_t>(len));
        file.clear();
        file.seekg(static_cast<std::streamoff>(pos));
        if (!file.read(data.data(), static_cast<std::streamsize>(len)) || std::memcmp(data.data(), "\x89PNG", 4) != 0) {
            continue;
        }
        const std::filesystem::path tmp = dst.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(data.data(), static_cast<std::streamsize>(len))) {
                continue;
            }
        }
        std::filesystem::rename(tmp, dst, ec);
    }
    return pngs;
}

// Procura o trophy00.trp na atualizacao (traz as conquistas das DLCs) e depois no jogo; extrai se faltar o icone
// de exemplo. Devolve true se os icones estao na pasta.
inline bool EnsureIcons(const std::filesystem::path& out_dir, const std::vector<std::filesystem::path>& game_dirs) {
    std::error_code ec;
    if (std::filesystem::exists(out_dir / "TROP001.PNG", ec)) {
        return true;
    }
    for (const auto& dir : game_dirs) {
        if (dir.empty()) {
            continue;
        }
        const std::filesystem::path trp = dir / "sce_sys" / "trophy" / "trophy00.trp";
        if (std::filesystem::exists(trp, ec) && ExtractIcons(trp, out_dir) > 0) {
            return std::filesystem::exists(out_dir / "TROP001.PNG", ec);
        }
    }
    return false;
}

} // namespace trophy_trp
