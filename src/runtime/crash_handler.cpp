#include "runtime/crash_handler.h"

#include <map>

#include <chrono>

#include <algorithm>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>

#include <Zydis/Zydis.h>

#include "hle/hle_log.h"

namespace runtime {

namespace {

u64 g_base = 0;
u64 g_image_size = 0;
u64 g_reserve_size = 0;

std::atomic_flag g_in_handler = ATOMIC_FLAG_INIT;

std::mutex g_threads_mutex;
std::vector<GuestThread*> g_threads;
thread_local GuestThread* t_current = nullptr;

bool InImage(u64 address) {
    return g_base && address >= g_base && address < g_base + g_image_size;
}

bool IsReadable(u64 address, u64 size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    const u64 region_end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    return address + size <= region_end;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    default: return nullptr;
    }
}

// Desmonta `count` instrucoes a partir de `address`. Retorna false se a memoria nao e legivel.
bool Disassemble(u64 address, int count, bool* uses_fs = nullptr) {
    ZydisDecoder decoder;
    ZydisFormatter formatter;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    for (int i = 0; i < count; ++i) {
        if (!IsReadable(address, 1)) {
            return i > 0;
        }
        u64 avail = 15;
        while (avail > 1 && !IsReadable(address, avail)) {
            --avail;
        }
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        const auto* bytes = reinterpret_cast<const u8*>(address);
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, bytes, avail, &insn, ops))) {
            std::printf("  %s %-28s  (invalid bytes: %02x)\n", i == 0 ? "=>" : "  ",
                        DescribeAddress(address).c_str(), bytes[0]);
            return true;
        }
        char text[160];
        ZydisFormatterFormatInstruction(&formatter, &insn, ops, insn.operand_count_visible, text,
                                        sizeof(text), address, ZYAN_NULL);
        char hex[3 * 15 + 1] = {};
        for (u8 b = 0; b < insn.length; ++b) {
            std::snprintf(hex + b * 3, 4, "%02x ", bytes[b]);
        }
        std::printf("  %s %-18s  %-33s %s\n", i == 0 ? "=>" : "  ",
                    DescribeAddress(address).c_str(), hex, text);
        if (i == 0 && uses_fs) {
            *uses_fs = (insn.attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS) != 0;
        }
        address += insn.length;
    }
    return true;
}

void DumpContext(const CONTEXT& c) {
    std::printf("Registers:\n");
    std::printf("  RAX=%016" PRIx64 "  RBX=%016" PRIx64 "  RCX=%016" PRIx64 "  RDX=%016" PRIx64 "\n",
                (u64)c.Rax, (u64)c.Rbx, (u64)c.Rcx, (u64)c.Rdx);
    std::printf("  RSI=%016" PRIx64 "  RDI=%016" PRIx64 "  RBP=%016" PRIx64 "  RSP=%016" PRIx64 "\n",
                (u64)c.Rsi, (u64)c.Rdi, (u64)c.Rbp, (u64)c.Rsp);
    std::printf("  R8 =%016" PRIx64 "  R9 =%016" PRIx64 "  R10=%016" PRIx64 "  R11=%016" PRIx64 "\n",
                (u64)c.R8, (u64)c.R9, (u64)c.R10, (u64)c.R11);
    std::printf("  R12=%016" PRIx64 "  R13=%016" PRIx64 "  R14=%016" PRIx64 "  R15=%016" PRIx64 "\n",
                (u64)c.R12, (u64)c.R13, (u64)c.R14, (u64)c.R15);
    std::printf("  RIP=%016" PRIx64 "  EFLAGS=%08lx  (%s)\n", (u64)c.Rip, c.EFlags,
                DescribeAddress(c.Rip).c_str());

    std::printf("Stack (from RSP):\n");
    for (int i = 0; i < 8; ++i) {
        const u64 addr = c.Rsp + i * 8;
        if (!IsReadable(addr, 8)) {
            break;
        }
        const u64 value = *reinterpret_cast<const u64*>(addr);
        std::printf("  [rsp+%02x] %016" PRIx64 "%s%s\n", i * 8, value, InImage(value) ? "  " : "",
                    InImage(value) ? DescribeAddress(value).c_str() : "");
    }

    // Sem unwind info do jogo: qualquer valor na pilha que aponte para dentro da imagem e
    // tratado como possivel endereco de retorno.
    std::printf("Possible return addresses on stack:\n");
    int found = 0;
    for (u64 addr = c.Rsp; found < 10 && addr < c.Rsp + 0x800 && IsReadable(addr, 8); addr += 8) {
        const u64 value = *reinterpret_cast<const u64*>(addr);
        if (InImage(value)) {
            std::printf("  [rsp+%03" PRIx64 "] %s\n", addr - c.Rsp, DescribeAddress(value).c_str());
            ++found;
        }
    }
    if (!found) {
        std::printf("  (none)\n");
    }
}

thread_local int t_guarded_reads = 0;
// CallGuardedWalker: 0 = parado, 1 = rodando, 2 = voltou de uma falha
thread_local volatile int t_walker_state = 0;
thread_local CONTEXT t_walker_context;

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const char* name = ExceptionName(rec->ExceptionCode);
    if (!name) {
        return EXCEPTION_CONTINUE_SEARCH; // excecoes C++, OutputDebugString etc.
    }
    if (t_walker_state == 1 && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        // falha dentro do walker: retoma logo depois do RtlCaptureContext de CallGuardedWalker
        const DWORD flags = info->ContextRecord->ContextFlags;
        *info->ContextRecord = t_walker_context;
        info->ContextRecord->ContextFlags = flags;
        t_walker_state = 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (t_guarded_reads > 0 && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH; // leitura protegida por __try no HLE (GuardedReadScope)
    }
    if (g_in_handler.test_and_set()) {
        TerminateProcess(GetCurrentProcess(), 3); // falha dentro do proprio dump
    }
    const CONTEXT& c = *info->ContextRecord;
    hle::log::FlushGameText();
    std::fflush(stdout);
    std::printf("\n======================== CRASH DUMP ========================\n");
    std::printf("Exception: 0x%08lX %s\n", rec->ExceptionCode, name);
    std::printf("Thread  : %s (tid %lu)\n", CurrentThreadName(), GetCurrentThreadId());
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && rec->NumberParameters >= 2) {
        static const char* kOps[] = {"read from", "write to", "?", "?", "?", "?", "?", "?",
                                     "execute from"};
        const ULONG_PTR op = rec->ExceptionInformation[0];
        std::printf("Access  : %s 0x%016" PRIx64 "\n", op <= 8 ? kOps[op] : "?",
                    (u64)rec->ExceptionInformation[1]);
    }
    std::printf("Local   : %s\n", DescribeAddress(c.Rip).c_str());

    std::printf("Instruction:\n");
    bool uses_fs = false;
    if (!Disassemble(c.Rip, 5, &uses_fs)) {
        std::printf("  RIP points to unmapped memory (jump/call to invalid pointer).\n");
        if (IsReadable(c.Rsp, 8)) {
            const u64 ret = *reinterpret_cast<const u64*>(c.Rsp);
            std::printf("  Likely caller: %s\n", DescribeAddress(ret).c_str());
        }
    }
    if (uses_fs) {
        std::printf("  ** Instruction uses FS segment and was not covered by TLS patcher. **\n");
    }
    DumpContext(c);
    hle::log::DumpHistory();
    std::printf("============================================================\n");
    std::fflush(stdout);
    hle::log::PrintSummary();
    hle::log::StopCapture();
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void SetGuestImage(u64 base, u64 image_size, u64 reserve_size) {
    g_base = base;
    g_image_size = image_size;
    g_reserve_size = reserve_size;
}

std::string DescribeAddress(u64 address) {
    char buf[48];
    if (InImage(address)) {
        std::snprintf(buf, sizeof(buf), "eboot+0x%" PRIx64, address - g_base);
    } else {
        std::snprintf(buf, sizeof(buf), "0x%" PRIx64, address);
    }
    return buf;
}

void InstallCrashHandler() {
    AddVectoredExceptionHandler(1, VectoredHandler);
}

GuestThread* RegisterCurrentThread(const char* name) {
    auto* t = new GuestThread;
    std::snprintf(t->name, sizeof(t->name), "%s", name && *name ? name : "unnamed");
    t->thread_id = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    t_current = t;
    std::lock_guard lock(g_threads_mutex);
    g_threads.push_back(t);
    return t;
}

GuestThread* CurrentThread() {
    return t_current;
}

const char* CurrentThreadName() {
    return t_current ? t_current->name : "host";
}

int AliveGuestThreads() {
    std::lock_guard lock(g_threads_mutex);
    int alive = 0;
    for (const GuestThread* t : g_threads) {
        alive += !t->finished.load();
    }
    return alive;
}

WaitScope::WaitScope(const char* what) : thread_(t_current) {
    if (thread_) {
        previous_ = thread_->waiting.exchange(what);
    }
}

WaitScope::~WaitScope() {
    if (thread_) {
        thread_->waiting.store(previous_);
        thread_->waits_completed.fetch_add(1, std::memory_order_relaxed);
    }
}

__declspec(noinline) bool CallGuardedWalker(void* function, const u32* user_data, u32* flat) {
    using Walker = void __attribute__((sysv_abi)) (*)(const u32*, u32*);
    t_walker_state = 0;
    RtlCaptureContext(&t_walker_context);
    if (t_walker_state == 2) { // chegou aqui pela falha
        t_walker_state = 0;
        return false;
    }
    t_walker_state = 1;
    reinterpret_cast<Walker>(function)(user_data, flat);
    t_walker_state = 0;
    return true;
}

GuardedReadScope::GuardedReadScope() {
    ++t_guarded_reads;
}

GuardedReadScope::~GuardedReadScope() {
    --t_guarded_reads;
}

namespace {

std::once_flag g_symbols_once;
u64 g_host_base = 0, g_host_end = 0;

// Nome da funcao de um endereco do proprio executavel ou de uma DLL do sistema (pelo PDB /
// tabela de exportacao). Chamar so com as threads do jogo rodando.
std::string HostSymbol(u64 address) {
    std::call_once(g_symbols_once, [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info))) {
            g_host_base = reinterpret_cast<u64>(info.lpBaseOfDll);
            g_host_end = g_host_base + info.SizeOfImage;
        }
    });
    char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    // Fase 59: fora do executavel, o nome do modulo na frente (o simbolo de uma DLL sem PDB e so a
    // exportacao mais proxima, que engana: o driver de video aparecia como "boost::serialization").
    std::string module;
    if (address && (address < g_host_base || address >= g_host_end)) {
        HMODULE handle = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &handle) &&
            GetModuleFileNameW(handle, path, MAX_PATH)) {
            const wchar_t* base = std::wcsrchr(path, L'\\');
            char narrow[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, narrow, sizeof(narrow), nullptr, nullptr);
            module = std::string(narrow) + "!";
        }
    }
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        std::string name = module + symbol->Name;
        if (name.size() > 90) {
            name.resize(90);
        }
        return name;
    }
    if (!module.empty()) {
        return module + "?";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    return text;
}

} // namespace

void SampleThreads(double seconds, const char* reason) {
    HostSymbol(0); // carrega os simbolos antes de comecar a suspender threads
    struct Sample {
        u64 rip, host_frame, guest_frame;
        const char* waiting;
    };
    struct Track {
        GuestThread* thread;
        std::vector<Sample> samples;
    };
    std::vector<Track> tracks;
    {
        std::lock_guard lock(g_threads_mutex);
        for (GuestThread* t : g_threads) {
            if (!t->finished.load() && t->thread_id != GetCurrentThreadId()) {
                tracks.push_back({t, {}});
            }
        }
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    u32 rounds = 0;
    while (std::chrono::steady_clock::now() < end) {
        ++rounds;
        for (Track& track : tracks) {
            Sample sample{0, 0, 0, track.thread->waiting.load()};
            if (!sample.waiting && SuspendThread(track.thread->handle) != static_cast<DWORD>(-1)) {
                CONTEXT context;
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(track.thread->handle, &context)) {
                    sample.rip = context.Rip;
                    for (u64 addr = context.Rsp; addr < context.Rsp + 0x1000 && (!sample.host_frame || !sample.guest_frame);
                         addr += 8) {
                        if (!IsReadable(addr, 8)) {
                            break;
                        }
                        const u64 value = *reinterpret_cast<const u64*>(addr);
                        if (!sample.guest_frame && InImage(value)) {
                            sample.guest_frame = value;
                        } else if (!sample.host_frame && !sample.guest_frame && value >= g_host_base && value < g_host_end) {
                            sample.host_frame = value; // so vale se vier antes do primeiro endereco do eboot
                        }
                    }
                }
                ResumeThread(track.thread->handle);
            }
            track.samples.push_back(sample);
        }
        Sleep(4);
    }

    std::printf("\n======================== CPU PROFILE: %s (%.1f s, %u samples per thread) ========================\n", reason,
                seconds, rounds);
    for (const Track& track : tracks) {
        std::map<std::string, u32> places;
        u32 running = 0;
        for (const Sample& sample : track.samples) {
            if (sample.waiting) {
                continue;
            }
            ++running;
            std::string place;
            if (InImage(sample.rip)) {
                // dentro do jogo: agrupa por pagina de 4 KiB do eboot
                char text[48];
                std::snprintf(text, sizeof(text), "game code at eboot+0x%llx",
                              static_cast<unsigned long long>((sample.rip - g_base) & ~0xFFFull));
                place = text;
            } else {
                place = HostSymbol(sample.rip);
                if (sample.host_frame) {
                    place += " <- " + HostSymbol(sample.host_frame);
                }
                if (sample.guest_frame) {
                    place += " <- " + DescribeAddress(sample.guest_frame);
                }
            }
            ++places[place];
        }
        const double busy = track.samples.empty() ? 0.0 : 100.0 * running / track.samples.size();
        if (busy < 2.0) {
            continue;
        }
        std::printf("* %-24s running in %.0f%% of samples\n", track.thread->name, busy);
        std::vector<std::pair<u32, std::string>> ranked;
        for (const auto& [place, count] : places) {
            ranked.emplace_back(count, place);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        for (size_t i = 0; i < ranked.size() && i < 8; ++i) {
            std::printf("    %5.1f%%  %s\n", 100.0 * ranked[i].first / track.samples.size(), ranked[i].second.c_str());
        }
    }
    std::printf("  (threads running in fewer than 2%% of samples omitted; \"running\" = outside a blocking HLE wait)\n");
    std::printf("======================== end of profile ========================\n");
    std::fflush(stdout);
}

namespace {

// Desenrola a pilha do host a partir de `context` (thread suspensa). Para no primeiro endereco do
// jogo (o codigo do eboot nao tem tabela de unwind) ou num endereco sem funcao conhecida.
u32 UnwindHostStackInner(CONTEXT* context, u64* out, u32 max) {
    u32 count = 0;
    __try {
        while (count < max) {
            const u64 rip = context->Rip;
            if (!rip || InImage(rip)) {
                break;
            }
            out[count++] = rip;
            DWORD64 image_base = 0;
            RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(rip, &image_base, nullptr);
            if (!function) {
                if (rip < g_host_base || rip >= g_host_end || count > 1) {
                    break; // folha sem unwind fora do executavel: nao da para seguir com seguranca
                }
                context->Rip = *reinterpret_cast<const u64*>(context->Rsp);
                context->Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, rip, function, context, &handler_data, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

} // namespace

void ProfileThreadDeep(const char* thread_name, double seconds, const char* reason) {
    HostSymbol(0);
    GuestThread* target = nullptr;
    {
        std::lock_guard lock(g_threads_mutex);
        for (GuestThread* t : g_threads) {
            if (!t->finished.load() && !std::strcmp(t->name, thread_name) && t->thread_id != GetCurrentThreadId()) {
                target = t;
            }
        }
    }
    if (!target) {
        std::printf("[PROFILE] thread \"%s\" not found\n", thread_name);
        return;
    }
    std::vector<std::vector<u64>> stacks;
    u32 waiting = 0, rounds = 0;
    timeBeginPeriod(1);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        ++rounds;
        if (target->waiting.load()) {
            ++waiting;
        } else if (SuspendThread(target->handle) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            u64 frames[64];
            u32 count = 0;
            if (GetThreadContext(target->handle, &context)) {
                count = UnwindHostStackInner(&context, frames, 64);
            }
            ResumeThread(target->handle);
            stacks.emplace_back(frames, frames + count);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    std::map<u64, std::string> names;
    auto name_of = [&](u64 address) -> const std::string& {
        auto it = names.find(address);
        if (it == names.end()) {
            it = names.emplace(address, HostSymbol(address)).first;
        }
        return it->second;
    };
    std::map<std::string, u32> inclusive, exclusive, own;
    u32 in_guest = 0;
    for (const auto& stack : stacks) {
        if (stack.empty()) {
            ++in_guest; // executando codigo do jogo
            continue;
        }
        ++exclusive[name_of(stack[0])];
        for (const u64 address : stack) { // a funcao do executavel mais interna (inclui o driver / sistema que ela chamou)
            if (address >= g_host_base && address < g_host_end) {
                ++own[name_of(address)];
                break;
            }
        }
        std::vector<std::string> seen;
        for (const u64 address : stack) {
            const std::string& name = name_of(address);
            if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                seen.push_back(name);
                ++inclusive[name];
            }
        }
    }
    const double total = std::max<size_t>(1, stacks.size() + waiting);
    std::printf("\n======================== DEEP PROFILE: thread %s, %s (%.1f s, %u samples: %u waiting in HLE, %u in game code) "
                "========================\n",
                thread_name, reason, seconds, rounds, waiting, in_guest);
    auto print = [&](const char* title, const std::map<std::string, u32>& map, size_t limit) {
        std::vector<std::pair<u32, std::string>> ranked;
        for (const auto& [name, count] : map) {
            ranked.emplace_back(count, name);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        std::printf("  %s:\n", title);
        for (size_t i = 0; i < ranked.size() && i < limit; ++i) {
            std::printf("    %5.1f%%  %s\n", 100.0 * ranked[i].first / total, ranked[i].second.c_str());
        }
    };
    print("inclusive (function and its callees)", inclusive, 45);
    print("exclusive (function itself)", exclusive, 30);
    print("by executable function (innermost; includes DLLs called by it)", own, 30);
    std::printf("======================== end of deep profile ========================\n");
    std::fflush(stdout);
}

// Fase 59: retrato rapido de onde cada thread do jogo esta (pilha do host desenrolada + endereco do
// jogo), para travadas curtas (100-500 ms) que nao aparecem nas contas por etapa. As threads ficam
// suspensas so durante o desenrolar; os nomes sao resolvidos depois de todas voltarem a rodar.
void DumpStallStacks(const char* reason) {
    HostSymbol(0);
    struct Snapshot {
        GuestThread* thread;
        const char* waiting;
        u64 frames[24];
        u32 count;
        u64 guest_rip, guest_return;
    };
    std::vector<Snapshot> shots;
    {
        std::lock_guard lock(g_threads_mutex);
        for (GuestThread* t : g_threads) {
            if (!t->finished.load() && t->thread_id != GetCurrentThreadId()) {
                shots.push_back({t, nullptr, {}, 0, 0, 0});
            }
        }
    }
    for (Snapshot& shot : shots) {
        shot.waiting = shot.thread->waiting.load();
        if (SuspendThread(shot.thread->handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(shot.thread->handle, &context)) {
            if (InImage(context.Rip)) {
                shot.guest_rip = context.Rip;
            } else {
                shot.count = UnwindHostStackInner(&context, shot.frames, 24);
                if (InImage(context.Rip)) {
                    shot.guest_return = context.Rip; // onde o jogo chamou o HLE
                }
            }
        }
        ResumeThread(shot.thread->handle);
    }
    std::printf("[STALL-STACKS] %s\n", reason);
    for (const Snapshot& shot : shots) {
        std::string line;
        if (shot.guest_rip) {
            line = "game code at " + DescribeAddress(shot.guest_rip);
        } else {
            for (u32 i = 0; i < shot.count && i < 7; ++i) {
                line += (i ? " <- " : "") + HostSymbol(shot.frames[i]);
            }
            if (shot.guest_return) {
                line += " <- " + DescribeAddress(shot.guest_return);
            }
        }
        if (line.size() > 600) {
            line.resize(600);
        }
        std::printf("[STALL-STACKS]   %-22s %s%s%s\n", shot.thread->name, shot.waiting ? "[wait: " : "", shot.waiting ? shot.waiting : "",
                    shot.waiting ? "] " : "");
        std::printf("[STALL-STACKS]       %s\n", line.c_str());
    }
    std::fflush(stdout);
}

void DumpAllThreads(const char* reason) {
    struct Snapshot {
        GuestThread* thread;
        CONTEXT context;
        bool ok;
        std::vector<u64> returns;
    };
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lock(g_threads_mutex);
        for (GuestThread* t : g_threads) {
            if (!t->finished.load() && t->thread_id != GetCurrentThreadId()) {
                snaps.push_back({t, {}, false, {}});
            }
        }
    }
    // Cada thread fica suspensa so o tempo de copiar contexto e pilha: imprimir com uma thread
    // suspensa travaria se ela estivesse segurando o lock do stdout.
    for (auto& s : snaps) {
        s.context.ContextFlags = CONTEXT_FULL;
        // Nada de alocar com a thread suspensa: se ela estiver dentro do heap, o push_back abaixo
        // esperaria para sempre pelo lock dela (o travamento na saida visto na Fase 21).
        s.returns.reserve(8);
        if (SuspendThread(s.thread->handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        s.ok = GetThreadContext(s.thread->handle, &s.context) != 0;
        if (s.ok) {
            for (u64 addr = s.context.Rsp; s.returns.size() < 8 && addr < s.context.Rsp + 0x4000; addr += 8) {
                if (!IsReadable(addr, 8)) {
                    break;
                }
                const u64 value = *reinterpret_cast<const u64*>(addr);
                if (InImage(value)) {
                    s.returns.push_back(value);
                }
            }
        }
        ResumeThread(s.thread->handle);
    }

    std::printf("\n======================== %s ========================\n", reason);
    std::printf("%zu live game thread(s):\n", snaps.size());
    for (const auto& s : snaps) {
        const char* waiting = s.thread->waiting.load();
        std::printf("* %-24s tid=%-6lu %s%s  (%llu completed waits)\n", s.thread->name, s.thread->thread_id,
                    waiting ? "BLOCKED in " : "executando", waiting ? waiting : "",
                    static_cast<unsigned long long>(s.thread->waits_completed.load()));
        if (!s.ok) {
            continue;
        }
        std::printf("    RIP=%s", DescribeAddress(s.context.Rip).c_str());
        std::printf("   eboot addresses on stack:");
        for (const u64 r : s.returns) {
            std::printf(" %s", DescribeAddress(r).c_str());
        }
        std::printf("%s\n", s.returns.empty() ? " (none)" : "");
    }
    hle::log::DumpHistory();
    std::printf("============================================================\n");
}

} // namespace runtime
