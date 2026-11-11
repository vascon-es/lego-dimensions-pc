#include "hle/overrides/hle_radar.h"
#include "hle/hle_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <map>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
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
        std::printf("\nLimit of %" PRIu64 " HLE log lines reached (--max-calls). Exiting.\n",
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
            std::printf("[LOG] %" PRIu64 " repeated line(s) suppressed in the last second: [%s] %.60s...\n", entry.suppressed,
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
                seen == kStubRepeatLimit && !g_verbose ? "  (further occurrences silenced)" : "");
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
    std::printf("Last recorded HLE calls (%" PRIu64 " of %" PRIu64 "):\n", shown,
                g_history_count);
    for (u64 i = g_history_count - shown; i < g_history_count; ++i) {
        std::printf("  %s\n", g_history[i % kHistory].c_str());
    }
    if (!shown) {
        std::printf("  (none)\n");
    }
}

void PrintSummary() {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    std::printf("\n------------------------- HLE SUMMARY -------------------------\n");

    std::vector<std::pair<u64, std::string>> stubs;
    for (const auto& [name, count] : g_stub_counts) {
        stubs.emplace_back(count, name);
    }
    std::sort(stubs.rbegin(), stubs.rend());
    std::printf("UNIMPLEMENTED stubs called by the game (%zu):\n", stubs.size());
    for (const auto& [count, name] : stubs) {
        std::printf("  %8" PRIu64 "x  %s\n", count, name.c_str());
    }
    if (stubs.empty()) {
        std::printf("  (none)\n");
    }

    std::vector<std::pair<u64, const char*>> quiet;
    u64 quiet_total = 0;
    for (auto* c = g_quiet_head.load(); c; c = c->next) {
        quiet.emplace_back(c->count.load(), c->name);
        quiet_total += c->count.load();
    }
    std::sort(quiet.rbegin(), quiet.rend());
    std::printf("Log repetition limiter: %" PRIu64 " line(s) suppressed (more than %u identical lines per second)\n", g_spam_total,
                kSpamPerSecond);
    std::printf("Silenced calls from high-frequency overrides: %" PRIu64 " in %zu functions\n",
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
    std::printf("Installed overrides by library (nonempty libraries only):\n");
    for (const auto& [lib, c] : coverage) {
        if (c.first) {
            std::printf("  %-16s %3u of %3u symbols\n", lib.c_str(), c.first, c.second);
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
    StopCapture();
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

namespace {

constexpr size_t kMaxAllLines = 3000;
constexpr size_t kMaxImportantLines = 1500;

std::mutex g_cap_mutex;
std::deque<LogEntry> g_all_lines;
std::deque<LogEntry> g_important_lines;
u64 g_next_seq = 1;
u64 g_cap_total = 0;
u64 g_cap_errors = 0;
u64 g_cap_warnings = 0;
u64 g_cap_cutscene = 0;
u64 g_cap_stubs = 0;
std::atomic<bool> g_cap_paused{false};
std::atomic<bool> g_cap_active{false};
const auto g_cap_epoch = std::chrono::steady_clock::now();
std::filesystem::path g_root_path;

int g_orig_out = -1;
int g_orig_err = -1;
int g_pipe_read = -1;
std::thread g_tee_thread;
std::thread::id g_tee_thread_id;

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool ContainsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (haystack.find(n) != std::string::npos) return true;
    }
    return false;
}

void AppendCapturedLine(const std::string& raw_line) {
    if (g_cap_paused.load(std::memory_order_relaxed)) {
        return;
    }
    std::string clean;
    clean.reserve(std::min<size_t>(raw_line.size(), 900));
    for (size_t i = 0; i < raw_line.size() && clean.size() < 900; ++i) {
        unsigned char c = static_cast<unsigned char>(raw_line[i]);
        if (c == 0x1B) { // skip ANSI escape sequences
            if (i + 1 < raw_line.size() && raw_line[i + 1] == '[') {
                i += 2;
                while (i < raw_line.size() && (raw_line[i] < '@' || raw_line[i] > '~')) ++i;
            }
            continue;
        }
        if (c == '\t') {
            clean += "  ";
        } else if (c >= 32) {
            clean.push_back(static_cast<char>(c));
        }
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    if (clean.empty()) {
        return;
    }
    if (raw_line.size() > 900) {
        clean += "...";
    }

    const std::string upper = ToUpperAscii(clean);
    const bool is_fs_line = upper.rfind("[FS] ", 0) == 0;
    const bool is_zero_failure = ContainsAny(upper, {"0 FAILURES", "FAILED PIPELINES 0", "(0 RETRIES)", "0 FAILED"});
    const bool is_stall_stack = upper.rfind("[STALL-STACKS]", 0) == 0;
    const bool is_boot_crashdump_id = upper.find("CRASHDUMP_ID=") != std::string::npos;

    LogEntry entry;
    entry.seq = g_next_seq++;
    entry.time_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cap_epoch).count();
    entry.is_cutscene = ContainsAny(upper, {"[AVPLAYER]", "CUTSCENE", ".MP4", "AVPLAYER", "VIDEODEC", "WATCHDOG", "LIBSCEAVPLAYER"});
    entry.is_stub = (upper.find("] STUB ") != std::string::npos) || (upper.rfind("STUB ", 0) == 0);
    entry.is_game = (upper.rfind("[GAME]", 0) == 0);

    bool is_error = false;
    bool is_warning = false;

    if (!is_zero_failure && !is_boot_crashdump_id && !is_stall_stack) {
        if (ContainsAny(upper, {"[FATAL", "FATAL HLE", "CRASH DUMP", "ACCESS_VIOLATION", "ILLEGAL_INSTRUCTION",
                                "STACK_OVERFLOW", "VK_ERROR_", "I/O ERROR", "0XC00D", "MF_E_", "READSAMPLE FAILED"})) {
            is_error = true;
        } else if (is_fs_line) {
            // Game probes missing optional archives often; only flag critical/cutscene/eboot/addcont misses cleanly
            if (ContainsAny(upper, {".MP4", "CUTSCENE", ".CU3", "MOVIES", "EBOOT", "ADDCONT", "ERROR"})) {
                is_warning = true;
            }
        } else if (ContainsAny(upper, {"ERROR", "ERRO:", "FALHA", "FAILED", "COULD NOT", "NAO FOI POSSIVEL",
                                       "UNSUPPORTED TEXTURE", "NO .MP4 FOUND"})) {
            if (upper.find("UNSUPPORTED TEXTURE") != std::string::npos || upper.find("NO .MP4 FOUND") != std::string::npos) {
                if (upper.find("NO .MP4 FOUND") != std::string::npos) is_error = true;
                else is_warning = true;
            } else {
                is_error = true;
            }
        }
    }

    if (!is_error) {
        if (ContainsAny(upper, {"WARNING", "AVISO", "WARN", "CUTSCENE WATCHDOG", "WATCHDOG #", "SUPPRESSED",
                                "SILENCED", "RELOCKING NON-RECURSIVE", "UNRESOLVED=", "NO MOUNT", "BLOCKED"}) ||
            entry.is_stub || is_stall_stack) {
            // Avoid flagging benign startup "unresolved=0" line as a warning
            if (!(upper.find("UNRESOLVED=0") != std::string::npos && upper.find("RELOCATIONS:") != std::string::npos)) {
                is_warning = true;
            }
        }
    }

    entry.severity = is_error ? 2 : (is_warning ? 1 : 0);
    entry.text = std::move(clean);

    std::lock_guard lock(g_cap_mutex);
    ++g_cap_total;
    if (entry.severity == 2) ++g_cap_errors;
    else if (entry.severity == 1) ++g_cap_warnings;
    if (entry.is_cutscene) ++g_cap_cutscene;
    if (entry.is_stub) ++g_cap_stubs;

    if (entry.severity > 0 || entry.is_cutscene || entry.is_stub) {
        if (g_important_lines.size() >= kMaxImportantLines) {
            g_important_lines.pop_front();
        }
        g_important_lines.push_back(entry);
    }

    if (g_all_lines.size() >= kMaxAllLines) {
        g_all_lines.pop_front();
    }
    g_all_lines.push_back(std::move(entry));
}

void WriteAllFd(int fd, const char* data, int len) {
    int off = 0;
    while (fd >= 0 && off < len) {
        const int w = _write(fd, data + off, static_cast<unsigned int>(len - off));
        if (w <= 0) break;
        off += w;
    }
}

void CaptureTeeLoop() {
    g_tee_thread_id = std::this_thread::get_id();
    char buf[4096];
    std::string pending;
    pending.reserve(1024);
    for (;;) {
        const int n = _read(g_pipe_read, buf, sizeof(buf));
        if (n <= 0) break;
        if (g_orig_out >= 0) {
            WriteAllFd(g_orig_out, buf, n);
        }
        for (int i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n') {
                AppendCapturedLine(pending);
                pending.clear();
            } else if (c != '\r') {
                if (pending.size() < 2048) {
                    pending.push_back(c);
                }
            }
        }
    }
    if (!pending.empty()) {
        AppendCapturedLine(pending);
    }
}

void CopyUtf8ToClipboard(const std::string& utf8) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
        HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen) * sizeof(wchar_t));
        if (hmem) {
            void* ptr = GlobalLock(hmem);
            if (ptr) {
                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, static_cast<wchar_t*>(ptr), wlen);
                GlobalUnlock(hmem);
                if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
                    GlobalFree(hmem);
                }
            } else {
                GlobalFree(hmem);
            }
        }
    }
    CloseClipboard();
}

} // namespace

void SetRootPath(const std::filesystem::path& root) {
    std::lock_guard lock(g_cap_mutex);
    g_root_path = root;
}

void StartCapture() {
    if (g_cap_active.load()) return;

    std::fflush(stdout);
    std::fflush(stderr);

    int orig_out = -1;
    int orig_err = -1;
    if (_get_osfhandle(1) != reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE) && _get_osfhandle(1) != -2) {
        orig_out = _dup(1);
    }
    if (_get_osfhandle(2) != reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE) && _get_osfhandle(2) != -2) {
        orig_err = _dup(2);
    }
    if (orig_out < 0 && orig_err >= 0) {
        orig_out = _dup(orig_err);
    }
    if (orig_out < 0) {
        // Launched without console/log redirection: bind stdout/stderr to NUL first so fd 1/2 exist
        std::freopen("NUL", "w", stdout);
        std::freopen("NUL", "w", stderr);
    }

    int fds[2]{-1, -1};
    if (_pipe(fds, 262144, _O_BINARY | _O_NOINHERIT) != 0) {
        if (orig_out >= 0) _close(orig_out);
        if (orig_err >= 0) _close(orig_err);
        return;
    }
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fds[0])), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fds[1])), HANDLE_FLAG_INHERIT, 0);
    if (orig_out >= 0) {
        SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(orig_out)), HANDLE_FLAG_INHERIT, 0);
    }
    if (orig_err >= 0) {
        _close(orig_err); // orig_out handles both streams to console / sessao_*.log cleanly
    }

    _dup2(fds[1], 1);
    _dup2(fds[1], 2);
    _close(fds[1]);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(1)), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(2)), HANDLE_FLAG_INHERIT, 0);

    g_orig_out = orig_out;
    g_pipe_read = fds[0];
    g_cap_active = true;
    g_tee_thread = std::thread(CaptureTeeLoop);
}

void StopCapture() {
    if (!g_cap_active.exchange(false)) return;
    std::fflush(stdout);
    std::fflush(stderr);
    if (g_orig_out >= 0) {
        _dup2(g_orig_out, 1);
        _dup2(g_orig_out, 2);
    } else {
        _close(1);
        _close(2);
    }
    if (g_tee_thread.joinable() && std::this_thread::get_id() != g_tee_thread_id) {
        g_tee_thread.join();
    }
    if (g_pipe_read >= 0) {
        _close(g_pipe_read);
        g_pipe_read = -1;
    }
    if (g_orig_out >= 0) {
        _close(g_orig_out);
        g_orig_out = -1;
    }
}

void FlushCapture() {
    std::fflush(stdout);
    std::fflush(stderr);
}

void SetCapturePaused(bool paused) {
    g_cap_paused.store(paused);
}

bool IsCapturePaused() {
    return g_cap_paused.load();
}

void ClearCapture() {
    std::lock_guard lock(g_cap_mutex);
    g_all_lines.clear();
    g_important_lines.clear();
    g_cap_total = 0;
    g_cap_errors = 0;
    g_cap_warnings = 0;
    g_cap_cutscene = 0;
    g_cap_stubs = 0;
}

void GetCaptureStats(LogStats* out) {
    if (!out) return;
    std::lock_guard lock(g_cap_mutex);
    out->total_lines = g_cap_total;
    out->error_count = g_cap_errors;
    out->warning_count = g_cap_warnings;
    out->cutscene_count = g_cap_cutscene;
    out->stub_count = g_cap_stubs;
    out->suppressed_lines = g_spam_total;
    out->paused = g_cap_paused.load();
}

std::vector<LogEntry> GetCapturedLogs(LogFilter filter) {
    std::lock_guard lock(g_cap_mutex);
    std::vector<LogEntry> result;
    const auto& source = (filter == LogFilter::kAll || filter == LogFilter::kGame) ? g_all_lines : g_important_lines;
    result.reserve(source.size());
    for (const auto& e : source) {
        switch (filter) {
        case LogFilter::kAll:
            result.push_back(e);
            break;
        case LogFilter::kErrors:
            if (e.severity == 2) result.push_back(e);
            break;
        case LogFilter::kWarnings:
            if (e.severity >= 1) result.push_back(e);
            break;
        case LogFilter::kCutscene:
            if (e.is_cutscene) result.push_back(e);
            break;
        case LogFilter::kStubs:
            if (e.is_stub) result.push_back(e);
            break;
        case LogFilter::kGame:
            if (e.is_game) result.push_back(e);
            break;
        default:
            break;
        }
    }
    return result;
}

std::string ExplainLogEntry(const LogEntry& entry, bool pt) {
    const std::string upper = ToUpperAscii(entry.text);
    if (upper.find("NO .MP4 FOUND") != std::string::npos) {
        return pt
            ? "MOTIVO: Nao ha arquivo .mp4 indexado nos pacotes (.DAT/DLC) lidos. A cutscene de video fecha sem exibir imagem."
            : "REASON: No .mp4 found in loaded .DAT/DLC pack indexes. Video cutscene ends without picture.";
    }
    if (upper.find("0XC00D5212") != std::string::npos || upper.find("MF_E_TOPO_CODEC_NOT_FOUND") != std::string::npos) {
        return pt
            ? "MOTIVO: Codec H.264/MP4 ausente no Windows Media Foundation (instale o Media Feature Pack ou codecs)."
            : "REASON: Missing H.264/MP4 codec in Windows Media Foundation (install Windows Media Feature Pack).";
    }
    if (upper.find("[AVPLAYER]") != std::string::npos &&
        ContainsAny(upper, {"COULD NOT OPEN", "READSAMPLE FAILED", "FAILED", "0X", "ERROR"})) {
        return pt
            ? "MOTIVO: Erro ao abrir ou decodificar video MP4 da cutscene no Media Foundation / pacote .DAT."
            : "REASON: Media Foundation failed opening or decoding the cutscene MP4 stream from .DAT pack.";
    }
    if (upper.find("[AVPLAYER]") != std::string::npos) {
        return pt
            ? "CUTSCENE/AV: Chamada do player de video (libSceAvPlayer). Veja se houve READY/PLAY e quadros > 0."
            : "CUTSCENE/AV: Video player lifecycle call (libSceAvPlayer). Check for READY/PLAY and frames > 0.";
    }
    if (upper.find("CUTSCENE WATCHDOG") != std::string::npos || upper.find("[STALL-STACKS]") != std::string::npos) {
        return pt
            ? "MOTIVO: Jogo travou ou repetiu os mesmos draws por varios segundos durante cena/cutscene (veja threads logo abaixo)."
            : "REASON: Game rendered identical draw count for seconds during scene/cutscene (check threads below).";
    }
    if (upper.find("] STUB ") != std::string::npos) {
        return pt
            ? "MOTIVO: Funcao do PS4 ainda sem implementacao HLE (stub retorna 0). Pode bloquear algo se ocorrer antes de uma cena."
            : "REASON: Unimplemented PS4 HLE function (stub returns 0). May block features if called right before a scene.";
    }
    if (upper.find("[FS]") != std::string::npos && upper.find("NO MOUNT") != std::string::npos) {
        return pt
            ? "MOTIVO: Jogo acessou caminho nao montado (/app0, /temp0, /savedata0). Normal para /data/sce_coredumps."
            : "REASON: Game accessed unmounted PS4 path (outside /app0, /temp0, /savedata0). Normal for /data/sce_coredumps.";
    }
    if (upper.find("[FS]") != std::string::npos && (upper.find("NOT FOUND") != std::string::npos || upper.find("I/O ERROR") != std::string::npos)) {
        return pt
            ? "MOTIVO: Arquivo nao encontrado ou falha de leitura. Normal em busca de PATCH/DLC opcional; critico se for arquivo de cena."
            : "REASON: File missing or read error. Normal when probing optional PATCH/DLC files; critical if asset is required.";
    }
    if (upper.find("[VULKAN]") != std::string::npos || upper.find("VK_ERROR_") != std::string::npos) {
        return pt
            ? "MOTIVO: Aviso ou erro de renderizacao no driver Vulkan / traducao de shader ou textura."
            : "REASON: Vulkan renderer / shader translation or texture format warning/error.";
    }
    if (upper.find("RELOCKING NON-RECURSIVE MUTEX") != std::string::npos) {
        return pt
            ? "MOTIVO: Thread tentou retravar o mesmo mutex; o port adaptou para evitar congelamento."
            : "REASON: Thread tried relocking non-recursive mutex; port adapted lock to prevent deadlock.";
    }
    if (upper.find("[GAME]") != std::string::npos) {
        return pt
            ? "JOGO: Saida de texto interna do proprio executavel (eboot.bin) do LEGO Dimensions."
            : "GAME: Internal stdout/stderr message emitted directly by LEGO Dimensions eboot.bin.";
    }
    if (entry.severity == 2) {
        return pt
            ? "ERRO: Falha detectada durante a execucao. Pressione [S] para salvar e copiar o relatorio completo."
            : "ERROR: Runtime failure detected. Press [S] to save and copy the full diagnostic report.";
    }
    if (entry.severity == 1) {
        return pt
            ? "AVISO: Comportamento incomum registrado. Verifique o contexto ao redor ou salve o relatorio [S]."
            : "WARNING: Notable runtime condition recorded. Check surrounding entries or press [S] to save report.";
    }
    return pt
        ? "INFO: Evento normal de execucao. Use [TAB] para filtrar Erros ou Cutscene/AV, ou [D] para capturar threads."
        : "INFO: Normal execution log. Use [TAB] to filter Errors or Cutscene/AV, or [D] to dump thread states.";
}

std::string SaveDiagnosticReport(LogFilter filter, const LogEntry* selected, bool copy_clipboard, bool pt) {
    std::filesystem::path dir;
    {
        std::lock_guard lock(g_cap_mutex);
        if (!g_root_path.empty()) {
            dir = g_root_path / "build" / "logs";
        } else {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
                auto p = std::filesystem::path(exe).parent_path();
                if (p.filename() == L"build") p = p.parent_path();
                dir = p / "build" / "logs";
            } else {
                dir = std::filesystem::current_path() / "build" / "logs";
            }
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char fname[64];
    std::snprintf(fname, sizeof(fname), "diag_%04u%02u%02u_%02u%02u%02u.log",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::filesystem::path out_path = dir / fname;

    LogStats stats{};
    GetCaptureStats(&stats);
    const auto entries = GetCapturedLogs(filter);
    const auto important = GetCapturedLogs(LogFilter::kWarnings);

    std::string report;
    report.reserve(16384);
    char hdr[512];
    std::snprintf(hdr, sizeof(hdr),
                  "=== LEGO DIMENSIONS PC DIAGNOSTIC REPORT (F9) ===\n"
                  "Lines: %llu | Errors: %llu | Warnings: %llu | Cutscene/AV: %llu | Stubs: %llu\n\n",
                  static_cast<unsigned long long>(stats.total_lines),
                  static_cast<unsigned long long>(stats.error_count),
                  static_cast<unsigned long long>(stats.warning_count),
                  static_cast<unsigned long long>(stats.cutscene_count),
                  static_cast<unsigned long long>(stats.stub_count));
    report += hdr;

    if (selected) {
        report += "--- SELECTED LINE ---\n";
        report += selected->text + "\n";
        report += ExplainLogEntry(*selected, pt) + "\n\n";
    }

    report += "--- PRESERVED ERRORS / WARNINGS / CUTSCENE / STUBS ---\n";
    for (const auto& e : important) {
        char ts[32];
        std::snprintf(ts, sizeof(ts), "[%.2fs] ", e.time_s);
        report += ts + e.text + "\n";
    }

    if (filter == LogFilter::kAll || filter == LogFilter::kGame) {
        report += "\n--- RECENT VIEW LINES ---\n";
        for (const auto& e : entries) {
            char ts[32];
            std::snprintf(ts, sizeof(ts), "[%.2fs] ", e.time_s);
            report += ts + e.text + "\n";
        }
    }

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (f) {
        f.write(report.data(), static_cast<std::streamsize>(report.size()));
    }

    if (copy_clipboard) {
        std::string clip;
        if (selected) {
            clip = selected->text + "\r\n" + ExplainLogEntry(*selected, pt) + "\r\n";
        } else if (!important.empty()) {
            const size_t start = important.size() > 12 ? important.size() - 12 : 0;
            for (size_t i = start; i < important.size(); ++i) {
                clip += important[i].text + "\r\n";
            }
        } else {
            clip = report;
        }
        CopyUtf8ToClipboard(clip);
    }

    return std::string("build\\logs\\") + fname;
}

} // namespace hle::log
