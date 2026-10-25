#include "hle/overrides/hle_radar.h"
#include "hle/hle_log.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <chrono>
#include <vector>

#include <windows.h>

#include "hle_symbol_table.h"
#include "runtime/crash_handler.h"

namespace hle::gnm {
void PrintStats();
}
namespace hle {
void PrintRandStats();
void PrintPadStats();
void PrintNgs2Stats();
}

namespace hle::log {

bool g_verbose = false;

namespace {

// Cada stub so aparece no log nas primeiras ocorrencias; o total vai para o resumo.
constexpr u64 kStubRepeatLimit = 6;
constexpr size_t kHistory = 16;

std::recursive_mutex g_mutex;
u64 g_max_lines = 400;
u64 g_lines = 0;
u64 g_total_calls = 0;

std::array<std::string, kHistory> g_history;
u64 g_history_count = 0;

std::map<std::string, u64> g_stub_counts;
std::string g_game_line[3];
std::atomic<QuietCounter*> g_quiet_head{nullptr};

void Remember(std::string text) {
    g_history[g_history_count % kHistory] = std::move(text);
    ++g_history_count;
}

void CountLine() {
    // Descarrega a cada linha: se o processo morrer sem passar pelo VEH (pilha destruida, por
    // exemplo), o log em disco ainda mostra ate onde o jogo chegou.
    std::fflush(stdout);
    if (++g_lines >= g_max_lines) {
        std::printf("\nLimite de %" PRIu64 " linhas de log HLE atingido (--max-calls). Encerrando.\n",
                    g_max_lines);
        Shutdown(0);
    }
}

// ---- limitador de repeticao: no maximo kSpamPerSecond linhas "iguais" por segundo ----
// Duas linhas sao iguais se tem a mesma chave: a etiqueta (ou o nome da funcao) mais o comeco
// do texto sem os digitos, para que contadores e enderecos nao criem chaves novas. O que passar
// do limite e contado, e o total sai numa linha quando a janela de 1 s termina.
constexpr u32 kSpamPerSecond = 10;
struct SpamEntry {
    std::chrono::steady_clock::time_point window{};
    u32 in_window = 0;
    u64 suppressed = 0;
};
std::map<std::string, SpamEntry> g_spam;
u64 g_spam_total = 0;

std::string SpamKey(const char* tag, const char* text) {
    std::string key = tag;
    key += '|';
    for (const char* p = text; *p && key.size() < 56; ++p) {
        if (*p < '0' || *p > '9') {
            key += *p;
        }
    }
    return key;
}

// Chamado com g_mutex. Devolve false se a linha deve ser suprimida.
bool AllowLine(const char* tag, const char* text) {
    if (g_verbose) {
        return true;
    }
    SpamEntry& entry = g_spam[SpamKey(tag, text)];
    const auto now = std::chrono::steady_clock::now();
    if (now - entry.window >= std::chrono::seconds(1)) {
        if (entry.suppressed) {
            std::printf("[LOG] %" PRIu64 " linha(s) repetida(s) suprimida(s) no ultimo segundo: [%s] %.60s...\n", entry.suppressed,
                        tag, text);
            entry.suppressed = 0;
        }
        entry.window = now;
        entry.in_window = 0;
    }
    if (++entry.in_window > kSpamPerSecond) {
        ++entry.suppressed;
        ++g_spam_total;
        return false;
    }
    return true;
}

void EmitGameLine(std::string& line) {
    std::printf("[GAME] %s\n", line.c_str());
    line.clear();
}

} // namespace

QuietCounter::QuietCounter(const char* name_) : name(name_) {
    next = g_quiet_head.load();
    while (!g_quiet_head.compare_exchange_weak(next, this)) {
    }
}

void Init(u64 max_lines, bool verbose) {
    g_max_lines = max_lines;
    g_verbose = verbose;
}

void StubLogger(const StubCall& c) {
    std::lock_guard lock(g_mutex);
    ++g_total_calls;
    const std::string key = std::string(c.library) + "::" + c.name;
    const u64 seen = ++g_stub_counts[key];
    const std::string from = runtime::DescribeAddress(c.return_address);
    Remember("STUB " + key + "  <- " + from);
    if (seen > kStubRepeatLimit && !g_verbose) {
        return;
    }
    std::printf("[%04" PRIu64 " %s] STUB %s  <- %s  rdi=%" PRIx64 " rsi=%" PRIx64 " rdx=%" PRIx64
                " rcx=%" PRIx64 " r8=%" PRIx64 " r9=%" PRIx64 "%s\n",
                g_lines + 1, runtime::CurrentThreadName(), key.c_str(), from.c_str(), c.rdi, c.rsi, c.rdx, c.rcx, c.r8, c.r9,
                seen == kStubRepeatLimit && !g_verbose ? "  (proximas ocorrencias silenciadas)" : "");
    CountLine();
}

void Call(const char* name, u64 return_address, const char* fmt, ...) {
    char args[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(args, sizeof(args), fmt, ap);
    va_end(ap);

    std::lock_guard lock(g_mutex);
    ++g_total_calls;
    const std::string from = runtime::DescribeAddress(return_address);
    Remember(std::string("HLE  ") + name + "  <- " + from);
    if (!AllowLine(name, args)) {
        return;
    }
    std::printf("[%04" PRIu64 " %s] HLE  %s(%s)  <- %s\n", g_lines + 1, runtime::CurrentThreadName(), name,
                args, from.c_str());
    CountLine();
}

void Tagged(const char* tag, const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_mutex);
    if (!AllowLine(tag, text)) {
        return;
    }
    std::printf("[%s] %s\n", tag, text);
    CountLine();
}

void Raw(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_mutex);
    std::printf("%s\n", text);
    CountLine();
}

void Fs(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_mutex);
    std::printf("[FS] %s\n", text);
    CountLine();
}

void GameText(int stream, const char* data, u64 size) {
    std::lock_guard lock(g_mutex);
    std::string& line = g_game_line[stream == 2 ? 2 : 1];
    for (u64 i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            EmitGameLine(line);
        } else if (data[i] != '\r') {
            line += data[i];
        }
    }
}

void FlushGameText() {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    for (auto& line : g_game_line) {
        if (!line.empty()) {
            EmitGameLine(line);
        }
    }
}

u64 StubCallsWithPrefix(const char* prefix, std::string* detail) {
    std::lock_guard lock(g_mutex);
    u64 total = 0;
    for (const auto& [key, count] : g_stub_counts) {
        if (key.rfind(prefix, 0) == 0) {
            total += count;
            if (detail) {
                *detail += (detail->empty() ? "" : "  ") + key + "=" + std::to_string(count);
            }
        }
    }
    return total;
}

std::string TopCallDeltas(size_t count, u64 frames) {
    static std::map<const QuietCounter*, u64> previous;
    std::vector<std::pair<u64, const char*>> deltas;
    for (auto* c = g_quiet_head.load(); c; c = c->next) {
        const u64 now = c->count.load();
        u64& before = previous[c];
        if (now > before) {
            deltas.emplace_back(now - before, c->name);
        }
        before = now;
    }
    std::sort(deltas.rbegin(), deltas.rend());
    std::string out;
    char item[96];
    for (size_t i = 0; i < deltas.size() && i < count; ++i) {
        std::snprintf(item, sizeof(item), "%s%s=%.0f", i ? "  " : "", deltas[i].second,
                      frames ? static_cast<double>(deltas[i].first) / frames : static_cast<double>(deltas[i].first));
        out += item;
    }
    return out;
}

u64 SuppressedLines() {
    return g_spam_total;
}

void DumpHistory() {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    const u64 shown = std::min<u64>(g_history_count, kHistory);
    std::printf("Ultimas chamadas HLE registradas (%" PRIu64 " de %" PRIu64 "):\n", shown,
                g_history_count);
    for (u64 i = g_history_count - shown; i < g_history_count; ++i) {
        std::printf("  %s\n", g_history[i % kHistory].c_str());
    }
    if (!shown) {
        std::printf("  (nenhuma)\n");
    }
}

void PrintSummary() {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    std::printf("\n------------------------- RESUMO HLE -------------------------\n");

    std::vector<std::pair<u64, std::string>> stubs;
    for (const auto& [name, count] : g_stub_counts) {
        stubs.emplace_back(count, name);
    }
    std::sort(stubs.rbegin(), stubs.rend());
    std::printf("Stubs SEM implementacao que o jogo chamou (%zu):\n", stubs.size());
    for (const auto& [count, name] : stubs) {
        std::printf("  %8" PRIu64 "x  %s\n", count, name.c_str());
    }
    if (stubs.empty()) {
        std::printf("  (nenhum)\n");
    }

    std::vector<std::pair<u64, const char*>> quiet;
    u64 quiet_total = 0;
    for (auto* c = g_quiet_head.load(); c; c = c->next) {
        quiet.emplace_back(c->count.load(), c->name);
        quiet_total += c->count.load();
    }
    std::sort(quiet.rbegin(), quiet.rend());
    std::printf("Limitador de repeticao do log: %" PRIu64 " linha(s) suprimida(s) (mais de %u iguais por segundo)\n", g_spam_total,
                kSpamPerSecond);
    std::printf("Chamadas silenciadas de overrides de alta frequencia: %" PRIu64 " em %zu funcoes\n",
                quiet_total, quiet.size());
    int col = 0;
    for (const auto& [count, name] : quiet) {
        std::printf("  %s=%" PRIu64 "%s", name, count, ++col % 5 == 0 ? "\n" : "");
    }
    if (col % 5) {
        std::printf("\n");
    }

    // Cobertura por biblioteca: quantos simbolos importados ja tem override.
    std::map<std::string, std::pair<u32, u32>> coverage;
    for (const auto& sym : Symbols()) {
        auto& c = coverage[std::string(sym.library)];
        ++c.second;
        c.first += sym.overridden;
    }
    std::printf("Overrides instalados por biblioteca (so as que tem algum):\n");
    for (const auto& [lib, c] : coverage) {
        if (c.first) {
            std::printf("  %-16s %3u de %3u simbolos\n", lib.c_str(), c.first, c.second);
        }
    }
    gnm::PrintStats();
    radar::PrintStats();
    PrintPadStats();
    PrintNgs2Stats();
    PrintRandStats();
    std::printf("--------------------------------------------------------------\n");
}

void Shutdown(int exit_code) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // Fase 70b: se algo travar no resumo, o log mostra ate onde foi
    FlushGameText();
    PrintSummary();
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
    std::abort();
}

void Fatal(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FlushGameText();
    std::printf("\n[FATAL HLE] %s\n", msg);
    std::fflush(stdout);
    __debugbreak(); // o VEH imprime o dump e encerra
    Shutdown(5);
}

} // namespace hle::log
