#include "hle/overrides/hle_radar.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "hle/hle_log.h"
#include "render/vulkan_backend.h"

namespace hle::radar {

namespace {

struct Entry {
    u64 offset;
    u64 packed; // bytes ocupados no pacote
    std::string name;
    bool seen = false;
};

struct Pack {
    std::vector<Entry> entries; // ordenado por offset
    u64 seen = 0;
};

const auto g_epoch = std::chrono::steady_clock::now();
std::mutex g_mutex;
std::filesystem::path g_index_dir;
std::filesystem::path g_local_index_dir; // Fase 64
std::FILE* g_log = nullptr;
std::map<std::string, Pack> g_packs; // "GAME", "STREAM3"... (vazio = pacote sem indice)
u64 g_files_seen = 0;
u64 g_screen_files = 0;
u64 g_packs_without_index = 0;

// No terminal, no maximo kBurstLimit linhas por segundo: o jogo le centenas de .txt de uma vez
// no boot, e a lista completa ja esta no arquivo de log.
constexpr u32 kBurstLimit = 12;
s64 g_burst_second = -1;
u32 g_burst_count = 0;
u32 g_burst_suppressed = 0;

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count();
}

// "/app0/data/chunk1/GAME.DAT" -> "GAME"; vazio se nao for .DAT
std::string PackName(const std::string& guest_path) {
    const size_t slash = guest_path.find_last_of("/\\");
    std::string file = guest_path.substr(slash == std::string::npos ? 0 : slash + 1);
    for (char& c : file) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (file.size() > 5 && file.compare(file.size() - 5, 5, ".DAT2") == 0) {
        return file.substr(0, file.size() - 5); // Fase 47: pacote de DLC ("/addcont3/DLC29.DAT2" -> "DLC29")
    }
    if (file.size() < 5 || file.compare(file.size() - 4, 4, ".DAT") != 0) {
        return {};
    }
    return file.substr(0, file.size() - 4);
}

// nome <TAB> tamanho <TAB> empacotado <TAB> offset <TAB> metodo
Pack LoadPack(const std::string& name) {
    Pack pack;
    std::error_code ec;
    const std::filesystem::path local = g_local_index_dir / (name + ".tsv");
    std::ifstream in(!g_local_index_dir.empty() && std::filesystem::is_regular_file(local, ec) ? local : g_index_dir / (name + ".tsv"));
    std::string line;
    while (std::getline(in, line)) {
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        const size_t t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
        if (t3 == std::string::npos) {
            continue;
        }
        Entry entry;
        entry.name = line.substr(0, t1);
        entry.packed = std::strtoull(line.c_str() + t2 + 1, nullptr, 10);
        entry.offset = std::strtoull(line.c_str() + t3 + 1, nullptr, 10);
        if (entry.name.empty()) {
            entry.name = "(unnamed)";
        }
        pack.entries.push_back(std::move(entry));
    }
    std::sort(pack.entries.begin(), pack.entries.end(),
              [](const Entry& a, const Entry& b) { return a.offset < b.offset; });
    return pack;
}

} // namespace

// Fase 72: "\\levels\\...\\<area>\\<area>.txt" (tambem dentro de \\additionalcontent\\...): o jogo comecou a carregar
// esta area. Devolve o nome, ou "".
std::string AreaName(const std::string& lower_name) {
    if (lower_name.find("\\levels\\") == std::string::npos || lower_name.size() < 5 || lower_name.compare(lower_name.size() - 4, 4, ".txt") != 0) {
        return {};
    }
    const size_t slash = lower_name.rfind('\\');
    const size_t parent = slash == std::string::npos || slash == 0 ? std::string::npos : lower_name.rfind('\\', slash - 1);
    if (parent == std::string::npos) {
        return {};
    }
    const std::string stem = lower_name.substr(slash + 1, lower_name.size() - slash - 5);
    const std::string dir = lower_name.substr(parent + 1, slash - parent - 1);
    return stem == dir && dir != "levels" ? stem : std::string(); // levels\levels.txt e a lista de fases, nao uma area
}

bool IsScreenFile(const std::string& lower_name) {
    for (const char* needle : {".gsc", ".txt", ".csv", ".lip", "screen"}) {
        if (lower_name.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void Configure(const std::filesystem::path& index_dir, const std::filesystem::path& log_file) {
    std::lock_guard lock(g_mutex);
    g_index_dir = index_dir;
    g_packs.clear();
    g_files_seen = g_screen_files = g_packs_without_index = 0;
    g_burst_second = -1;
    g_burst_count = g_burst_suppressed = 0;
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (!log_file.empty()) {
        g_log = _wfopen(log_file.c_str(), L"w");
    }
}

void SetLocalIndexDir(const std::filesystem::path& dir) {
    std::lock_guard lock(g_mutex);
    g_local_index_dir = dir;
    g_packs.clear();
}

void OnRead(const std::string& guest_path, s64 offset, u64 size) {
    if (!g_log) {
        return;
    }
    const std::string name = PackName(guest_path);
    if (name.empty() || offset < 0 || !size) {
        return;
    }
    std::vector<std::string> screen_lines;
    std::vector<std::string> areas; // Fase 72
    {
        std::lock_guard lock(g_mutex);
        if (g_index_dir.empty()) {
            return;
        }
        auto it = g_packs.find(name);
        if (it == g_packs.end()) {
            it = g_packs.emplace(name, LoadPack(name)).first;
            if (it->second.entries.empty()) {
                ++g_packs_without_index;
            }
        }
        Pack& pack = it->second;
        const u64 begin = static_cast<u64>(offset);
        const u64 end = begin + size;
        // primeira entrada que pode conter `begin`: a ultima com offset <= begin
        auto entry = std::upper_bound(pack.entries.begin(), pack.entries.end(), begin,
                                      [](u64 value, const Entry& e) { return value < e.offset; });
        if (entry != pack.entries.begin()) {
            --entry;
        }
        const double now = Seconds();
        for (; entry != pack.entries.end() && entry->offset < end; ++entry) {
            if (entry->seen || entry->offset + std::max<u64>(entry->packed, 1) <= begin) {
                continue;
            }
            entry->seen = true;
            ++pack.seen;
            ++g_files_seen;
            std::string lower = entry->name;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool screen = IsScreenFile(lower);
            if (std::string area = AreaName(lower); !area.empty()) {
                areas.push_back(std::move(area)); // Fase 72
            }
            if (g_log) {
                std::fprintf(g_log, "%8.2f\t%s\t%s\t%s\n", now, screen ? "TELA" : "-", name.c_str(), entry->name.c_str());
            }
            if (screen) {
                ++g_screen_files;
                const s64 second = static_cast<s64>(now);
                if (second != g_burst_second) {
                    if (g_burst_suppressed) {
                        screen_lines.push_back("... and more " + std::to_string(g_burst_suppressed) +
                                               " file(s) in second " + std::to_string(g_burst_second) +
                                               " (full list in radar log)");
                    }
                    g_burst_second = second;
                    g_burst_count = g_burst_suppressed = 0;
                }
                if (++g_burst_count > kBurstLimit) {
                    ++g_burst_suppressed;
                    continue;
                }
                screen_lines.push_back(entry->name + "  (" + name + ".DAT, t=" + std::to_string(static_cast<int>(now)) + "s)");
            }
        }
        if (g_log && !screen_lines.empty()) {
            std::fflush(g_log);
        }
    }
    for (const std::string& line : screen_lines) {
        log::Raw(">>> [SCREEN RADAR] Loading: %s <<<", line.c_str());
    }
    for (const std::string& area : areas) {
        render::VulkanRenderer::Instance().SetCurrentLevel(area); // Fase 72: prioridade do aquecimento desta area
    }
}

u64 FilesSeen() {
    std::lock_guard lock(g_mutex);
    return g_files_seen;
}

void PrintStats() {
    std::lock_guard lock(g_mutex);
    if (g_log) {
        std::fflush(g_log);
    }
    if (g_burst_suppressed) {
        std::printf(">>> [SCREEN RADAR] ... and %u more file(s) in second %lld (full list in radar log) <<<\n",
                    g_burst_suppressed, static_cast<long long>(g_burst_second));
    }
    std::printf("Radar      : %llu distinct files read from .DAT archives (%llu in screen filter)",
                static_cast<unsigned long long>(g_files_seen), static_cast<unsigned long long>(g_screen_files));
    if (g_packs_without_index) {
        std::printf("; %llu pack(s) WITHOUT index - run tools/build_dat_index.py",
                    static_cast<unsigned long long>(g_packs_without_index));
    }
    std::printf("\n");
    for (const auto& [name, pack] : g_packs) {
        if (pack.seen) {
            std::printf("             %-16s %6llu of %zu\n", name.c_str(), static_cast<unsigned long long>(pack.seen),
                        pack.entries.size());
        }
    }
}

} // namespace hle::radar
