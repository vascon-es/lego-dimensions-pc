#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

namespace cutscene_patch {
// Adapted from JonyGOi commit 0aeb8b9. Scan only file-backed executable
// segments supplied by the loader. Missing/ambiguous matches leave all bytes
// unchanged; a fixed 48 MiB scan could read unmapped/non-code regions.
//
// O codigo e' o mesmo na versao EUR e na USA (a funcao e' achada pelo padrao de bytes, nunca por um endereco fixo -
// o endereco muda de uma versao para a outra). Alem da leitura final ("cena ja vista / jogo 100%"), a funcao tem
// saidas antecipadas do tipo "je +4; xor eax,eax; jmp", que devolvem "nao pode pular" por causa dos campos 0x390 do
// objeto da cena. Sem desarmar essas saidas tambem, a cutscene de historia continuava sem aceitar o pulo.
struct Result {
    bool applied = false;
    int gates = 0;   // saidas antecipadas desarmadas
    int64_t at = -1; // deslocamento da leitura liberada dentro dos segmentos de codigo (-1 = nao achou)
    explicit operator bool() const { return applied; }
};

// 4.4.40: um trecho de codigo que o patch troca (e sabe desfazer). A partir daqui o patch pode ser LIGADO e DESLIGADO:
// ele so fica ligado dentro de uma fase jogavel, porque a cena de atracao da tela inicial tambem e' uma "cutscene"
// para o jogo e, com o patch ligado, ela desenhava o aviso "Pular" por cima de "Player 1" e do menu do titulo.
struct Site {
    uint8_t* at = nullptr;
    size_t size = 0;
    std::array<uint8_t, 16> before{};
    std::array<uint8_t, 16> after{};
};

namespace detail {
// procura um padrao exigindo exatamente UMA ocorrencia em um unico segmento
inline uint8_t* FindUnique(std::span<const std::span<uint8_t>> segments, const uint8_t* pattern, size_t length,
                           int64_t* where) {
    uint8_t* found = nullptr;
    int64_t found_at = -1;
    int64_t base = 0;
    for (const auto code : segments) {
        for (size_t offset = 0; offset + length <= code.size(); ++offset) {
            if (std::memcmp(code.data() + offset, pattern, length)) continue;
            if (found) return nullptr; // ambiguo: nao mexe em nada
            found = code.data() + offset;
            found_at = base + static_cast<int64_t>(offset);
        }
        base += static_cast<int64_t>(code.size());
    }
    if (where) *where = found_at;
    return found;
}

inline void AddSite(std::vector<Site>* sites, uint8_t* at, std::span<const uint8_t> before,
                    std::span<const uint8_t> after) {
    if (!sites || before.size() > sizeof(Site::before) || before.size() != after.size()) return;
    Site site;
    site.at = at;
    site.size = before.size();
    std::memcpy(site.before.data(), before.data(), before.size());
    std::memcpy(site.after.data(), after.data(), after.size());
    sites->push_back(site);
}
} // namespace detail

// Acha os trechos a trocar, SEM escrever nada. O preenchimento de `sites` e' opcional (os testes e o modo antigo,
// que aplica na hora, nao precisam dela).
inline Result Find(std::span<const std::span<uint8_t>> segments, std::vector<Site>* sites) {
    constexpr std::array<uint8_t, 8> original{0x8A, 0x83, 0xE8, 0, 0, 0, 0x24, 1};
    constexpr std::array<uint8_t, 8> patched{0xB0, 1, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    constexpr std::array<uint8_t, 6> short_original{0x8A, 0x83, 0xE8, 0, 0, 0};
    constexpr std::array<uint8_t, 6> short_patched{0xB0, 1, 0x90, 0x90, 0x90, 0x90};
    Result out;
    int64_t at = -1;
    uint8_t* found = detail::FindUnique(segments, original.data(), original.size(), &at);
    std::span<const uint8_t> patched_bytes = patched;
    if (!found) { // variante sem o "and al,1" colado na leitura
        found = detail::FindUnique(segments, short_original.data(), short_original.size(), &at);
        patched_bytes = short_patched;
        std::span<const uint8_t> before_bytes = short_original;
        if (found) detail::AddSite(sites, found, before_bytes, patched_bytes);
    } else {
        detail::AddSite(sites, found, std::span<const uint8_t>(original), patched_bytes);
    }
    if (!found) return out;
    out.applied = true;
    out.at = at;
    // Saidas antecipadas da MESMA funcao: "je +4; xor eax,eax; jmp" -> "je +4; mov al,1; jmp". As duas variantes
    // (test al,1 e test eax,0x04000000) sao desarmadas - o patch so fica ligado dentro de fase, entao a cena da tela
    // inicial nunca e' afetada.
    constexpr std::array<uint8_t, 5> gate{0x74, 0x04, 0x31, 0xC0, 0xEB};
    constexpr std::array<uint8_t, 5> gate_patched{0x74, 0x04, 0xB0, 0x01, 0xEB};
    constexpr size_t kWindow = 0x120; // a funcao inteira cabe com folga nessa janela antes da leitura liberada
    for (const auto code : segments) {
        uint8_t* begin = code.data();
        uint8_t* end = begin + code.size();
        if (found < begin || found >= end) continue;
        const size_t have = static_cast<size_t>(found - begin);
        const size_t from = have > kWindow ? have - kWindow : 0;
        for (size_t offset = from; offset + gate.size() <= have; ++offset) {
            if (std::memcmp(begin + offset, gate.data(), gate.size())) continue;
            detail::AddSite(sites, begin + offset, std::span<const uint8_t>(gate), std::span<const uint8_t>(gate_patched));
            ++out.gates;
        }
    }
    return out;
}

// Escreve (ou devolve) os bytes de todos os trechos. `after` = patch ligado, senao os bytes do proprio jogo.
inline void Write(const std::vector<Site>& sites, bool after) {
    for (const Site& site : sites) {
        if (!site.at || !site.size) continue;
        DWORD old = 0;
        const bool page = VirtualProtect(site.at, site.size, PAGE_EXECUTE_READWRITE, &old) != 0;
        std::memcpy(site.at, after ? site.after.data() : site.before.data(), site.size);
        FlushInstructionCache(GetCurrentProcess(), site.at, site.size);
        if (page) {
            DWORD ignored = 0;
            VirtualProtect(site.at, site.size, old, &ignored);
        }
    }
}

// Modo antigo (testes e quem quiser o patch ligado desde o inicio): acha e aplica na hora.
inline Result Apply(std::span<const std::span<uint8_t>> segments) {
    std::vector<Site> sites;
    const Result out = Find(segments, &sites);
    if (out.applied) Write(sites, true);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Estado do patch "pular cutscene" em tempo de execucao: ligado so dentro de fase jogavel.
// ---------------------------------------------------------------------------------------------------------------
namespace live {
inline std::mutex g_mutex;
inline std::vector<Site> g_sites;
inline bool g_armed = false;
inline bool g_in_level = false;
inline int g_gates = 0;

inline bool Arm(std::span<const std::span<uint8_t>> segments) {
    std::vector<Site> sites;
    const Result found = Find(segments, &sites);
    std::lock_guard lock(g_mutex);
    g_sites = std::move(sites);
    g_armed = found.applied;
    g_in_level = false; // comeca desligado: nada muda na abertura nem na tela inicial
    g_gates = found.gates;
    return found.applied;
}

// Chamado quando o jogo entra/sai de uma fase jogavel (radar de arquivos). Idempotente e barato quando nao muda.
inline void SetInLevel(bool in_level) {
    std::lock_guard lock(g_mutex);
    if (!g_armed || g_in_level == in_level) return;
    g_in_level = in_level;
    Write(g_sites, in_level);
    std::printf("[CUTSCENES] skip %s (level gate: %zu site(s))\n", in_level ? "ON" : "OFF", g_sites.size());
    std::fflush(stdout);
}

inline bool InLevel() {
    std::lock_guard lock(g_mutex);
    return g_in_level;
}

inline int Gates() {
    std::lock_guard lock(g_mutex);
    return g_gates;
}
} // namespace live
} // namespace cutscene_patch
