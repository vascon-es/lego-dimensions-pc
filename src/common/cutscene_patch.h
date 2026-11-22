#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace cutscene_patch {
// Adapted from JonyGOi commit 0aeb8b9. Scan only file-backed executable
// segments supplied by the loader. Missing/ambiguous matches leave all bytes
// unchanged; a fixed 48 MiB scan could read unmapped/non-code regions.
//
// 4.4.38: nada de gancho em vtable nem de caso especial para a abertura: apenas o patch do proprio jogo, por bytes.
// 4.4.33: o codigo e' o mesmo na versao EUR e na USA (a funcao e' achada pelo padrao de bytes, nunca por um endereco
// fixo - o endereco muda de uma versao para a outra). Alem da leitura final ("cena ja vista / jogo 100%"), a funcao
// tem saidas antecipadas do tipo "je +4; xor eax,eax; jmp", que devolvem "nao pode pular" por causa dos campos
// 0x390 do objeto da cena. Sem desarmar essas saidas tambem, a cutscene de historia continuava sem aceitar o pulo.
struct Result {
    bool applied = false;
    int gates = 0;   // saidas antecipadas desarmadas
    int64_t at = -1; // deslocamento da leitura liberada dentro dos segmentos de codigo (-1 = nao achou)
    explicit operator bool() const { return applied; }
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
} // namespace detail

inline Result Apply(std::span<const std::span<uint8_t>> segments) {
    constexpr std::array<uint8_t, 8> original{0x8A, 0x83, 0xE8, 0, 0, 0, 0x24, 1};
    constexpr std::array<uint8_t, 8> patched{0xB0, 1, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    constexpr std::array<uint8_t, 6> short_original{0x8A, 0x83, 0xE8, 0, 0, 0};
    constexpr std::array<uint8_t, 6> short_patched{0xB0, 1, 0x90, 0x90, 0x90, 0x90};
    Result out;
    int64_t at = -1;
    uint8_t* found = detail::FindUnique(segments, original.data(), original.size(), &at);
    const uint8_t* patched_bytes = patched.data();
    size_t patched_size = patched.size();
    if (!found) { // variante sem o "and al,1" colado na leitura
        found = detail::FindUnique(segments, short_original.data(), short_original.size(), &at);
        patched_bytes = short_patched.data();
        patched_size = short_patched.size();
    }
    if (!found) return out;
    std::memcpy(found, patched_bytes, patched_size);
    out.applied = true;
    out.at = at;
    // saidas antecipadas da mesma funcao: "je +4; xor eax,eax; jmp ..." -> "je +4; mov al,1; jmp ..."
    constexpr std::array<uint8_t, 5> gate{0x74, 0x04, 0x31, 0xC0, 0xEB};
    constexpr size_t kWindow = 0x120; // a funcao inteira cabe com folga nessa janela antes da leitura liberada
    for (const auto code : segments) {
        uint8_t* begin = code.data();
        uint8_t* end = begin + code.size();
        if (found < begin || found >= end) continue;
        const size_t have = static_cast<size_t>(found - begin);
        const size_t from = have > kWindow ? have - kWindow : 0;
        for (size_t offset = from; offset + gate.size() <= have; ++offset) {
            if (std::memcmp(begin + offset, gate.data(), gate.size())) continue;
            begin[offset + 2] = 0xB0; // mov al, 1 (antes: xor eax, eax)
            begin[offset + 3] = 0x01;
            ++out.gates;
        }
    }
    return out;
}
} // namespace cutscene_patch
