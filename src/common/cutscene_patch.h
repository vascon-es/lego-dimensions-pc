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

inline uint8_t* FindSkipPermission(std::span<const std::span<uint8_t>> code) {
    // Prologo de Cutscene::CanSkip, incluindo o primeiro teste de flags da sequencia.
    constexpr std::array<uint8_t, 24> signature{
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x03,
        0xff, 0x50, 0x10, 0x8b, 0x88, 0x90, 0x03, 0x00, 0x00, 0xf6, 0xc1, 0x80};
    return detail::FindUnique(code, signature.data(), signature.size(), nullptr);
}

inline uint8_t* FindStartupClassifier(std::span<const std::span<uint8_t>> code) {
    // O caminho de pulo chama a funcao que reconhece Loading_Screen_startup
    // no conjunto de sequencias da propria cena.
    constexpr std::array<uint8_t, 12> call_site{
        0xc6, 0x45, 0xe7, 0, 0xc6, 0x45, 0xe6, 0, 0x48, 0x89, 0xdf, 0xe8};
    constexpr std::array<uint8_t, 15> after_call{
        0x48, 0x8d, 0x75, 0xe7, 0x48, 0x8d, 0x55, 0xe6, 0x48, 0x89, 0xdf, 0x84, 0xc0, 0x74, 0x07};
    constexpr std::array<uint8_t, 16> classifier{
        0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x10, 0x31, 0xf6, 0x48, 0x89, 0xc7, 0xe8};
    const auto* call = detail::FindUnique(code, call_site.data(), call_site.size(), nullptr);
    if (!call) return nullptr;
    const auto operand = reinterpret_cast<uintptr_t>(call) + call_site.size();
    const auto next = operand + sizeof(int32_t);
    for (const auto segment : code) {
        const auto begin = reinterpret_cast<uintptr_t>(segment.data());
        const auto end = begin + segment.size();
        if (operand < begin || next > end || end - next < after_call.size()) continue;
        if (std::memcmp(reinterpret_cast<const void*>(next), after_call.data(), after_call.size())) return nullptr;
        int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(operand), sizeof(displacement));
        const auto target = static_cast<uintptr_t>(static_cast<intptr_t>(next) + displacement);
        for (const auto callee : code) {
            const auto start = reinterpret_cast<uintptr_t>(callee.data());
            if (target < start || target - start > callee.size() || callee.size() - (target - start) < classifier.size())
                continue;
            auto* function = callee.data() + (target - start);
            return std::memcmp(function, classifier.data(), classifier.size()) == 0 ? function : nullptr;
        }
        return nullptr;
    }
    return nullptr;
}

inline uint8_t* FindPermissionSlot(std::span<const std::span<uint8_t>> data, const uint8_t* function) {
    uint8_t* slot = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(function);
    for (const auto segment : data) {
        for (size_t i = 0; i + sizeof(address) <= segment.size(); i += sizeof(address)) {
            if (std::memcmp(segment.data() + i, &address, sizeof(address))) continue;
            if (slot) return nullptr;
            slot = segment.data() + i;
        }
    }
    return slot;
}

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
