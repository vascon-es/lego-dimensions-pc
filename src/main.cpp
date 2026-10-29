#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <array>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <timeapi.h>

#include "hle/overrides/hle_radar.h"
#include "hle/hle_log.h"
#include "hle/overrides/hle_equeue.h"
#include "hle/overrides/hle_filesystem.h"
#include "hle/overrides/hle_anomaly.h"
#include "hle/overrides/hle_mods.h"
#include "hle/overrides/hle_toypad.h"
#include "hle/overrides/hle_videoout.h"
#include "render/vulkan_backend.h"
#include "hle/overrides/hle_override.h"
#include "hle_stub.h"
#include "hle_symbol_table.h"
#include "loader/elf_pie_loader.h"
#include "common/game_region.h"
#include "common/cutscene_patch.h"
#include "patcher/fs_tls_patcher.h"
#include "runtime/lang.h"
#include "runtime/crash_handler.h"
#include "runtime/orbis_tls.h"

namespace hle {
void ResetUserServiceEvents();
void SetVirtualToypadConnected(bool connected);
bool UseRealToypad(); // Fase 61: Toy Pad fisico pelo HID do Windows
void SetRand15Bits(bool enabled);
void StartPortalWatcher(const std::filesystem::path& portal_file);
void StartToypadListener(int port); // app LegoToypad (cliente TCP em 127.0.0.1)
bool ParsePortalJson(const std::string& json, int pad, std::array<u8, 7>* uid, u32* id);
bool SetPadInputScript(const char* text);
void SetLoadGameMacro(bool enabled);
void SetSkipIntroSpam(bool enabled);
void SetPadKeyboardEnabled(bool enabled);
void SetPadBackgroundInput(bool enabled);
void SetNgs2VoiceClock(bool enabled);
void SetNgs2VoiceLog(bool enabled);
void SetAudioOutput(bool enabled);
void SetAudioDump(const char* path);
void SetSaveUi(bool enabled);
void SetSavesDirName(const char* name);
void SetAddcontDirectory(const std::filesystem::path& directory); // Fase 46: DLC
void SetPlayerName(int index, const std::string& name);           // Fase 65: apelidos dos jogadores
int TestSaveUi();
void SetPadStickLog(bool enabled);
namespace padsdl {
void SetEnabled(bool enabled);
}
namespace gnm {
void SetFrameDumpDirectory(const std::filesystem::path& directory);
void SetShaderDumpDirectory(const std::filesystem::path& directory);
void FlushShaderDumps(); // Fase 45: o extrator grava em segundo plano
void SetCutsceneWatchdog(u32 seconds);
void AddFrameDumpAt(u32 seconds);
void SetVulkanDraws(bool enabled);
void SetGameShaders(bool enabled);
void SetTargetLog(bool enabled);
void SetGeometryCache(bool enabled);
void SetReadback(bool enabled);
void SetHitchLog(bool enabled);
void SetAsyncPairs(bool enabled);
void SetParallelDma(bool enabled);
void SetAsyncTextures(bool enabled);
void StartShaderWarmup(bool enabled);
void SetAot(bool enabled, bool first);
void SetSyncShaders(bool enabled, double pipeline_budget_ms);
void SetLiveSyncBudget(double ms); // Fase 56
void SetPairBudget(double budget_ms);
void SetGeometryBudget(double budget_ms); // Fase 61
void SetButtonIcons(const char* name);    // Fase 62
void SetPreloadAllAot(bool enabled);
void SetSyncTextures(bool enabled, double budget_ms, double spike_budget_ms, u32 spike_count);
void SetTextureWait(bool enabled); // Fase 45
void SetParallelTextures(bool enabled); // Fase 59
bool SelftestDetile(); // Fase 59
void SetStallStacks(bool enabled); // Fase 59
void SetLazyAot(bool enabled);      // Fase 59
void PreloadAotIndex();
void SetTextureRefresh(bool enabled);
void SetLiveCompiler(const std::filesystem::path& tool, const std::filesystem::path& root, const std::filesystem::path& shader_dir);
void SetSrgbDisplay(bool enabled);
void AddProfileAt(u32 seconds);
void StartPerfThread();
}
} // namespace hle

namespace {

// Fase 61: mod "pular qualquer cutscene". A funcao do eboot em 0x173fd80 decide se a cutscene atual aceita
// ser pulada: depois das checagens de estado, vale o "allowcutskip" do nivel ou o bit 0 de [obj+0xe8] (cena
// ja vista / jogo completo). Troca essa ultima leitura por "mov al, 1", mantendo as checagens de antes.
bool g_skip_cutscenes = false;

bool PatchCutsceneSkip(const loader::ElfPieLoader& elf) {
    std::vector<std::span<uint8_t>> code;
    for (const auto& segment : elf.segments()) {
        if ((segment.flags & 1) && segment.vaddr <= elf.image_size() && segment.filesz <= elf.image_size() - segment.vaddr)
            code.emplace_back(elf.base() + segment.vaddr, static_cast<size_t>(segment.filesz));
    }
    return cutscene_patch::Apply(code);
}

std::filesystem::path FindPatchDir(const std::filesystem::path& app0) {
    return game_region::FindPatch(app0);
}

// Dump do jogo usado como /app0 quando --app0 nao e informado: a pasta do jogo escolhida no launcher
// (linha jogo= do launcher.ini na pasta do port).
std::filesystem::path DefaultApp0(const std::filesystem::path& root) {
    wchar_t buffer[2048] = {};
    GetPrivateProfileStringW(L"launcher", L"jogo", L"", buffer, 2048, (root / "launcher.ini").wstring().c_str());
    return buffer;
}

// Fase 49: pasta do port (a que tem assets\ e tools\). Antes era a pasta do projeto gravada na
// compilacao; agora sai de onde esta o .exe (build\lego_dimensions.exe -> pasta acima), para a
// instalacao funcionar em qualquer pasta. --root PASTA forca outra.
std::filesystem::path FindRoot(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--root")) {
            return argv[i + 1];
        }
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length && length < buffer.size()) {
        const std::filesystem::path exe_dir = std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        std::error_code ec;
        for (const auto& dir : {exe_dir.parent_path(), exe_dir}) {
            if (std::filesystem::is_directory(dir / "assets", ec) && std::filesystem::is_directory(dir / "tools", ec)) {
                return dir;
            }
        }
    }
    return LD_PROJECT_ROOT;
}

// O que o crt0 do Orbis recebe em RDI; argc e argv[0] tambem vao para o topo da pilha.
struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[33];
    u64 entry_addr;
};

// Segundo argumento do entry point: o crt0 chama isto se main() retornar.
HLE_SYSV_ABI void ProgramExit() {
    std::printf("\nProgram called the exit handler passed in RSI. Exiting.\n");
    hle::log::Shutdown(0);
}

[[noreturn]] void JumpToEntry(u64 entry, const EntryParams* params, void (HLE_SYSV_ABI* exit_fn)()) {
    // Mesma convencao do loader do Orbis: pilha alinhada com argc/argv[0] no topo,
    // RDI = &EntryParams, RSI = funcao de saida.
    __asm__ volatile("andq $-16, %%rsp\n"
                     "subq $8, %%rsp\n"
                     "pushq 8(%%rdi)\n"
                     "pushq 0(%%rdi)\n"
                     "xorl %%ebp, %%ebp\n"
                     "jmpq *%%rax\n"
                     :
                     : "a"(entry), "D"(params), "S"(exit_fn));
    __builtin_unreachable();
}

// Garantia de encerramento (Fase 22): se o relatorio final travar (uma thread suspensa ou morta
// segurando um lock que o relatorio precisa), o processo termina de qualquer jeito 20 s depois.
// Nao usa a CRT: escreve o aviso direto no handle e chama TerminateProcess.
DWORD WINAPI ExitGuardThread(void*) {
    Sleep(20000);
    static const char kMessage[] = "\n[SHUTDOWN] final report did not finish within 20 s: process forcibly terminated (code 5)\n";
    // O que o relatorio ja tinha escrito esta no buffer da CRT: sem isto o log acabava ANTES do
    // ponto do travamento e nao dava para saber onde foi. Sem lock de proposito (quem travou pode
    // estar com ele); o processo acaba logo abaixo.
    _fflush_nolock(stdout);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), kMessage, sizeof(kMessage) - 1, &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 5);
    return 0;
}

DWORD WINAPI WatchdogThread(void* arg) {
    Sleep(static_cast<DWORD>(reinterpret_cast<u64>(arg)));
    CreateThread(nullptr, 0, ExitGuardThread, nullptr, 0, nullptr);
    hle::log::FlushGameText();
    runtime::DumpAllThreads("TIMEOUT (--timeout)");
    hle::log::Shutdown(4);
}

template <typename Fn>
Fn Guest(const char* name) {
    const hle::HleSymbol* sym = hle::FindSymbolByName(name);
    return sym ? reinterpret_cast<Fn>(sym->address) : nullptr;
}

// Callback de transferencia USB no formato da libusb: marca que foi chamado.
struct UsbTransferView {
    void* dev_handle;
    u8 flags, endpoint, type;
    u32 timeout;
    int status, length, actual_length;
    void* callback;
    void* user_data;
    u8* buffer;
};

HLE_SYSV_ABI void SelfTestUsbCallback(UsbTransferView* transfer) {
    ++*static_cast<int*>(transfer->user_data);
}

struct ThreadTest {
    void* sema;
    runtime::OrbisTcb* tcb;
    bool self_ok;
};

// Entry point de thread no formato que o jogo passa para scePthreadCreate.
HLE_SYSV_ABI void* SelfTestThread(void* arg) {
    using WaitSemaFn = int (HLE_SYSV_ABI*)(void*, s32, u32*);
    auto* t = static_cast<ThreadTest*>(arg);
    Guest<WaitSemaFn>("sceKernelWaitSema")(t->sema, 1, nullptr);
    t->tcb = runtime::CurrentTcb();
    t->self_ok = t->tcb && t->tcb->tcb_self == t->tcb;
    return reinterpret_cast<void*>(0x1234);
}

// Chama os overrides exatamente como o jogo chamaria (ponteiro da tabela, ABI System V,
// chamadas variadicas de verdade) e confere os resultados.
int SelfTest() {
    int failures = 0;
    const auto check = [&](const char* what, bool ok) {
        std::printf("  [%s] %s\n", ok ? " OK " : "FAILURE", what);
        failures += !ok;
    };
    using SnprintfFn = int (HLE_SYSV_ABI*)(char*, u64, const char*, ...);
    using SscanfFn = int (HLE_SYSV_ABI*)(const char*, const char*, ...);
    char buf[256];

    // 5 inteiros variadicos em registradores + 7 na pilha, misturados com doubles
    Guest<SnprintfFn>("snprintf")(buf, sizeof(buf), "%d %s %5.2f %lld %x %c|%d %d %d %ld %g %s %u", 1,
                                  "abc", 3.14159, 123456789012LL, 255, 'z', 7, 8, 9, -5000000000LL, 2.5,
                                  "end", 4000000000u);
    check(buf, !std::strcmp(buf, "1 abc  3.14 123456789012 ff z|7 8 9 -5000000000 2.5 end 4000000000"));

    // 9 doubles: o nono vai para a pilha
    Guest<SnprintfFn>("snprintf")(buf, sizeof(buf), "%g %g %g %g %g %g %g %g %g %d", 1.0, 2.0, 3.0, 4.0, 5.0,
                                  6.0, 7.0, 8.0, 9.0, 42);
    check(buf, !std::strcmp(buf, "1 2 3 4 5 6 7 8 9 42"));

    const int n = Guest<SnprintfFn>("snprintf")(buf, 6, "%s", "truncado");
    check("snprintf truncates and returns full length", n == 8 && !std::strcmp(buf, "trunc"));

    int i = 0;
    float f = 0;
    char word[32] = {};
    s64 big = 0;
    u32 hex = 0;
    const int got = Guest<SscanfFn>("sscanf")("12 abc 3.5 -9000000000 0x1F", "%d %s %f %lld %x", &i, word, &f,
                                              &big, &hex);
    check("sscanf %d %s %f %lld %x",
          got == 5 && i == 12 && !std::strcmp(word, "abc") && f == 3.5f && big == -9000000000LL && hex == 0x1F);

    using MutexFn = int (HLE_SYSV_ABI*)(void**);
    void* static_mutex = nullptr; // SCE_PTHREAD_MUTEX_INITIALIZER
    const int l1 = Guest<MutexFn>("scePthreadMutexLock")(&static_mutex);
    const int l2 = Guest<MutexFn>("scePthreadMutexLock")(&static_mutex);
    const int u1 = Guest<MutexFn>("scePthreadMutexUnlock")(&static_mutex);
    const int u2 = Guest<MutexFn>("scePthreadMutexUnlock")(&static_mutex);
    check("static mutex: lock succeeds, relock EDEADLK, unlock succeeds, extra unlock EPERM",
          l1 == 0 && l2 == hle::err::kEDEADLK && u1 == 0 && u2 == hle::err::kEPERM && static_mutex != nullptr);

    using AllocFn = int (HLE_SYSV_ABI*)(s64, s64, u64, u64, int, s64*);
    using MapFn = int (HLE_SYSV_ABI*)(void**, u64, int, int, s64, u64);
    using UnmapFn = int (HLE_SYSV_ABI*)(void*, u64);
    s64 phys = -1;
    void* addr = nullptr;
    const int a = Guest<AllocFn>("sceKernelAllocateDirectMemory")(0, 0x100000000LL, 0x400000, 0x200000, 0, &phys);
    const int m = Guest<MapFn>("sceKernelMapDirectMemory")(&addr, 0x400000, 3, 0, phys, 0x200000);
    bool mem_ok = a == 0 && m == 0 && addr && !(reinterpret_cast<u64>(addr) & 0x1FFFFF);
    if (mem_ok) {
        std::memset(addr, 0xAB, 0x400000);
        mem_ok = static_cast<u8*>(addr)[0x3FFFFF] == 0xAB;
        mem_ok = mem_ok && Guest<UnmapFn>("sceKernelMunmap")(addr, 0x400000) == 0;
    }
    check("direct memory: allocate, map aligned to 2 MiB, write, unmap", mem_ok);

    using SetjmpFn = int (HLE_SYSV_ABI*)(u64*);
    using LongjmpFn = void (HLE_SYSV_ABI*)(u64*, int);
    static u64 jmp[12];
    static volatile int stage = 0;
    const int r = Guest<SetjmpFn>("setjmp")(jmp);
    if (r == 0 && stage == 0) {
        stage = 1;
        Guest<LongjmpFn>("longjmp")(jmp, 7);
    }
    check("setjmp/longjmp", r == 7 && stage == 1);

    // ---- semaforo: espera com timeout, sinal, limite maximo
    using CreateSemaFn = int (HLE_SYSV_ABI*)(void**, const char*, u32, s32, s32, const void*);
    using WaitSemaFn = int (HLE_SYSV_ABI*)(void*, s32, u32*);
    using SignalSemaFn = int (HLE_SYSV_ABI*)(void*, s32);
    using HandleFn = int (HLE_SYSV_ABI*)(void*);
    void* sema = nullptr;
    u32 timeout = 20000;
    const int sc = Guest<CreateSemaFn>("sceKernelCreateSema")(&sema, "selftest", 0, 1, 2, nullptr);
    const int sw1 = Guest<WaitSemaFn>("sceKernelWaitSema")(sema, 1, nullptr);
    const int sw2 = Guest<WaitSemaFn>("sceKernelWaitSema")(sema, 1, &timeout);
    const int ss1 = Guest<SignalSemaFn>("sceKernelSignalSema")(sema, 2);
    const int ss2 = Guest<SignalSemaFn>("sceKernelSignalSema")(sema, 1);
    const int sw3 = Guest<WaitSemaFn>("sceKernelWaitSema")(sema, 2, nullptr);
    const int sd = Guest<HandleFn>("sceKernelDeleteSema")(sema);
    check("semaphore: wait succeeds, timed wait, signal up to maximum, signal over maximum = EINVAL",
          sc == 0 && sw1 == 0 && sw2 == hle::err::kETIMEDOUT && timeout == 0 && ss1 == 0 &&
              ss2 == hle::err::kEINVAL && sw3 == 0 && sd == 0);

    // ---- event flag: OR, AND, auto-clear, Clear como mascara
    using CreateEvfFn = int (HLE_SYSV_ABI*)(void**, const char*, u32, u64, const void*);
    using BitsEvfFn = int (HLE_SYSV_ABI*)(void*, u64);
    using WaitEvfFn = int (HLE_SYSV_ABI*)(void*, u64, u32, u64*, u32*);
    void* evf = nullptr;
    u64 pattern = 0;
    const int ec = Guest<CreateEvfFn>("sceKernelCreateEventFlag")(&evf, "selftest", 0x22, 0x5, nullptr);
    const int ew1 = Guest<WaitEvfFn>("sceKernelWaitEventFlag")(evf, 0x6, 0x02 | 0x20, &pattern, nullptr); // OR + CLEAR_PAT
    const u64 p1 = pattern;                                                                               // 0x5; sobra 0x1
    timeout = 20000;
    const int ew2 = Guest<WaitEvfFn>("sceKernelWaitEventFlag")(evf, 0x3, 0x01, &pattern, &timeout); // AND: falta 0x2
    const bool evf_timeout_kept = timeout == 20000; // o event flag nao mexe no timeout do chamador
    Guest<BitsEvfFn>("sceKernelSetEventFlag")(evf, 0xA);                                             // 0xB
    const int ew3 = Guest<WaitEvfFn>("sceKernelWaitEventFlag")(evf, 0x3, 0x01 | 0x10, &pattern, nullptr); // AND + CLEAR_ALL
    const u64 p3 = pattern;
    Guest<BitsEvfFn>("sceKernelSetEventFlag")(evf, 0xF);
    Guest<BitsEvfFn>("sceKernelClearEventFlag")(evf, 0x9); // mantem so 0x9
    timeout = 1000;
    const int ew4 = Guest<WaitEvfFn>("sceKernelWaitEventFlag")(evf, 0x6, 0x02, &pattern, &timeout);
    const int ed = Guest<HandleFn>("sceKernelDeleteEventFlag")(evf);
    check("event flag: OR+CLEAR_PAT, timed AND (timeout unchanged), AND+CLEAR_ALL, Clear as mask",
          ec == 0 && ew1 == 0 && p1 == 0x5 && ew2 == hle::err::kETIMEDOUT && evf_timeout_kept && ew3 == 0 &&
              p3 == 0xB &&
              ew4 == hle::err::kETIMEDOUT && pattern == 0x9 && ed == 0);

    // ---- thread real com TLS proprio, acordada por semaforo
    using CreateThreadFn = int (HLE_SYSV_ABI*)(void**, const void*, void* (HLE_SYSV_ABI*)(void*), void*, const char*);
    using JoinFn = int (HLE_SYSV_ABI*)(void*, void**);
    static ThreadTest tt{};
    Guest<CreateSemaFn>("sceKernelCreateSema")(&tt.sema, "selftest-thread", 0, 0, 1, nullptr);
    void* thread = nullptr;
    void* thread_result = nullptr;
    const int tc = Guest<CreateThreadFn>("scePthreadCreate")(
        &thread, nullptr, &SelfTestThread, &tt, "selftest");
    Guest<SignalSemaFn>("sceKernelSignalSema")(tt.sema, 1);
    const int tj = Guest<JoinFn>("scePthreadJoin")(thread, &thread_result);
    check("thread: created, blocks on semaphore, own TCB (different from main), join returns result",
          tc == 0 && tj == 0 && thread_result == reinterpret_cast<void*>(0x1234) && tt.self_ok &&
              tt.tcb != runtime::CurrentTcb());

    // ---- UserService: um LOGIN, depois fila vazia
    using GetEventFn = int (HLE_SYSV_ABI*)(int*);
    int event[2] = {-1, -1};
    hle::ResetUserServiceEvents();
    const int ue1 = Guest<GetEventFn>("sceUserServiceGetEvent")(event);
    const int ue2 = Guest<GetEventFn>("sceUserServiceGetEvent")(event);
    const int ue3 = Guest<GetEventFn>("sceUserServiceGetEvent")(event);
    hle::ResetUserServiceEvents();
    check("sceUserServiceGetEvent: LOGIN for user 1 on first call, then NO_EVENT",
          ue1 == 0 && event[0] == 0 && event[1] == 1 && ue2 == static_cast<int>(0x80960007) && ue2 == ue3);

    // ---- VFS: /app0 somente leitura, /temp0 gravavel, leitura posicional
    using OpenFn = int (HLE_SYSV_ABI*)(const char*, int, u32);
    using CloseFn = int (HLE_SYSV_ABI*)(int);
    using PwriteFn = s64 (HLE_SYSV_ABI*)(int, const void*, u64, s64);
    using PreadFn = s64 (HLE_SYSV_ABI*)(int, void*, u64, s64);
    using PathFn = int (HLE_SYSV_ABI*)(const char*);
    const int ro = Guest<OpenFn>("sceKernelOpen")("/app0/selftest_nao_pode_criar.bin", 0x602, 0666);
    const int fd = Guest<OpenFn>("sceKernelOpen")("/temp0/selftest.bin", 0x602, 0666); // RDWR|CREAT|TRUNC
    char rd[8] = {};
    const s64 wr = Guest<PwriteFn>("sceKernelPwrite")(fd, "abcdefgh", 8, 0);
    const s64 rr = Guest<PreadFn>("sceKernelPread")(fd, rd, 4, 2);
    const int cl = Guest<CloseFn>("sceKernelClose")(fd);
    const int ul = Guest<PathFn>("sceKernelUnlink")("/temp0/selftest.bin");
    const int esc = Guest<OpenFn>("sceKernelOpen")("/temp0/../../fora.bin", 0x602, 0666);
    check("VFS: /app0 refuses writes, /temp0 writes and reads by offset, '..' cannot escape mount",
          ro == hle::fs::kEROFS && fd >= 3 && wr == 8 && rr == 4 && !std::memcmp(rd, "cdef", 4) && cl == 0 &&
              ul == 0 && esc < 0);
    {
        // Fase 58: atualizacao em pasta separada por cima do /app0 (o arquivo dela vence; listagem junta as duas)
        const std::filesystem::path saved_app0 = hle::fs::App0(), saved_patch = hle::fs::App0Patch(), writable = hle::fs::WritableRoot();
        const std::filesystem::path base = std::filesystem::path(LD_PROJECT_ROOT) / "build" / "selftest_patch" / "jogo";
        const std::filesystem::path over = base / "CUSA01176-patch";
        std::error_code ec;
        std::filesystem::create_directories(base / "data", ec);
        std::filesystem::create_directories(over / "data", ec);
        std::filesystem::create_directories(over / "sce_sys", ec);
        std::filesystem::create_directories(base / "sce_sys", ec);
        std::array<unsigned char, 55> sfo{};
        const auto put = [&](size_t at, u32 value) { std::memcpy(sfo.data()+at, &value, 4); };
        std::memcpy(sfo.data(), "\0PSF", 4); put(4, 0x101); put(8, 36); put(12, 45); put(16, 1);
        sfo[22] = 4; sfo[23] = 2; put(24, 10); put(28, 10);
        std::memcpy(sfo.data()+36, "TITLE_ID", 9); std::memcpy(sfo.data()+45, "CUSA01176", 10);
        for (const auto& folder : {base, over})
            std::ofstream(folder / "sce_sys" / "param.sfo", std::ios::binary).write(reinterpret_cast<const char*>(sfo.data()), sfo.size());
        for (const auto& [file, text] : {std::pair{base / "data" / "a.txt", "base"}, std::pair{base / "data" / "b.txt", "base"},
                                         std::pair{over / "data" / "b.txt", "patch"}, std::pair{over / "data" / "c.txt", "patch"}}) {
            std::ofstream(file) << text;
        }
        hle::fs::Configure(base, writable);
        hle::fs::SetApp0Patch(FindPatchDir(base));
        const auto b = hle::fs::Resolve("/app0/data/b.txt"), a_file = hle::fs::Resolve("/app0/data/a.txt");
        std::vector<hle::fs::DirEntry> list;
        const int le = hle::fs::ListDirectory("selftest", "/app0/data", &list);
        const bool ok = hle::fs::App0Patch() == over && b.host == (over / "data" / "b.txt").lexically_normal() &&
                        a_file.host == (base / "data" / "a.txt").lexically_normal() && le == 0 && list.size() == 3;
        hle::fs::Configure(saved_app0, writable);
        hle::fs::SetApp0Patch(saved_patch);
        check("VFS: separate update (CUSA01176-patch) auto-detected, overrides base game and listing merges both", ok);
    }

    // ---- Ngs2: tamanhos de buffer no offset que o jogo le, handles validos, render em silencio
    using QuerySysFn = int (HLE_SYSV_ABI*)(const void*, void*);
    using QueryRackFn = int (HLE_SYSV_ABI*)(u32, const void*, void*);
    using SysCreateFn = int (HLE_SYSV_ABI*)(const void*, const void*, void**);
    using RackCreateFn = int (HLE_SYSV_ABI*)(void*, u32, const void*, const void*, void**);
    using VoiceHandleFn = int (HLE_SYSV_ABI*)(void*, u32, void**);
    using VoiceCtlFn = int (HLE_SYSV_ABI*)(void*, const void*);
    using VoiceStateFn = int (HLE_SYSV_ABI*)(void*, void*, u64);
    using RenderFn = int (HLE_SYSV_ABI*)(void*, const void*, u32);
    u64 info[8];
    std::memset(info, 0xCC, sizeof(info));
    const int nq = Guest<QuerySysFn>("sceNgs2SystemQueryBufferSize")(nullptr, info);
    const bool sys_size_ok = nq == 0 && info[0] == 0 && info[1] >= 0x1000 && info[1] <= 0x100000;
    std::memset(info, 0xCC, sizeof(info));
    const int nr = Guest<QueryRackFn>("sceNgs2RackQueryBufferSize")(0x1000, nullptr, info);
    void *ngs_system = nullptr, *rack = nullptr, *voice = nullptr;
    const int nsc = Guest<SysCreateFn>("sceNgs2SystemCreate")(nullptr, info, &ngs_system);
    const int nrc = Guest<RackCreateFn>("sceNgs2RackCreate")(ngs_system, 0x1000, nullptr, info, &rack);
    const int nvh = Guest<VoiceHandleFn>("sceNgs2RackGetVoiceHandle")(rack, 0, &voice);
    const int nvc = Guest<VoiceCtlFn>("sceNgs2VoiceControl")(voice, info);
    u8 state[0x30];
    std::memset(state, 0xCC, sizeof(state));
    const int nvs = Guest<VoiceStateFn>("sceNgs2VoiceGetState")(voice, state, sizeof(state));
    static float pcm[256 * 8];
    for (float& sample : pcm) {
        sample = 1.0f;
    }
    struct {
        void* buffer;
        u64 size;
        u32 waveform, channels;
    } render = {pcm, sizeof(pcm), 0x18, 8};
    const int nrd = Guest<RenderFn>("sceNgs2SystemRender")(ngs_system, &render, 1);
    check("Ngs2: hostBufferSize at +0x08, non-null handles, zeroed state, silent render",
          sys_size_ok && nr == 0 && info[1] >= 0x1000 && nsc == 0 && ngs_system && nrc == 0 && rack && nvh == 0 &&
              voice && nvc == 0 && nvs == 0 && state[0x18] == 0 && nrd == 0 && pcm[0] == 0.0f && pcm[2047] == 0.0f);

    // ---- AudioOut: 10 buffers de 256 amostras a 48 kHz tem de levar ~53 ms
    using AudioOpenFn = int (HLE_SYSV_ABI*)(int, int, int, u32, u32, u32);
    using AudioOutFn = int (HLE_SYSV_ABI*)(int, const void*);
    const int port = Guest<AudioOpenFn>("sceAudioOutOpen")(0xFF, 0, 0, 256, 48000, 5);
    const ULONGLONG audio_start = GetTickCount64();
    int audio_result = 0;
    for (int i = 0; i < 10; ++i) {
        audio_result = Guest<AudioOutFn>("sceAudioOutOutput")(port, pcm);
    }
    const ULONGLONG audio_ms = GetTickCount64() - audio_start;
    std::printf("         (10 buffers took %llu ms)\n", audio_ms);
    check("AudioOut: port >= 1 and real-time output (40-90 ms for 10 buffers)",
          port >= 1 && audio_result == 256 && audio_ms >= 40 && audio_ms <= 90);

    // ---- Usbd: HandleEvents nao pode retornar na hora; lista de dispositivos conforme --toypad
    using UsbVoidFn = int (HLE_SYSV_ABI*)();
    using UsbListFn = s64 (HLE_SYSV_ABI*)(void***);
    using UsbFreeListFn = void (HLE_SYSV_ABI*)(void**, int);
    using UsbDescFn = int (HLE_SYSV_ABI*)(void*, void*);
    const ULONGLONG usb_start = GetTickCount64();
    for (int i = 0; i < 5; ++i) {
        Guest<UsbVoidFn>("sceUsbdHandleEvents")();
    }
    const ULONGLONG usb_ms = GetTickCount64() - usb_start;
    void** usb_list = nullptr;
    hle::SetVirtualToypadConnected(false);
    const s64 none = Guest<UsbListFn>("sceUsbdGetDeviceList")(&usb_list);
    Guest<UsbFreeListFn>("sceUsbdFreeDeviceList")(usb_list, 1);
    hle::SetVirtualToypadConnected(true);
    const s64 one = Guest<UsbListFn>("sceUsbdGetDeviceList")(&usb_list);
    u8 descriptor[18] = {};
    const int ud = one == 1 ? Guest<UsbDescFn>("sceUsbdGetDeviceDescriptor")(usb_list[0], descriptor) : -1;
    Guest<UsbFreeListFn>("sceUsbdFreeDeviceList")(usb_list, 1);
    hle::SetVirtualToypadConnected(false);
    std::printf("         (5 x HandleEvents took %llu ms)\n", usb_ms);
    check("Usbd: HandleEvents blocks ~16 ms; 0 devices without --toypad, Toy Pad 0e6f:0241 with it",
          usb_ms >= 60 && none == 0 && one == 1 && ud == 0 && descriptor[8] == 0x6F && descriptor[9] == 0x0E &&
              descriptor[10] == 0x41 && descriptor[11] == 0x02 && descriptor[17] == 1);

    // ---- Fios2 contra o dump real: protocolo de sondagem do DHOpen, leitura, erro de "nao existe"
    using FiosDhOpenFn = int (HLE_SYSV_ABI*)(const void*, int*, const char*, void*, u64);
    using FiosOpFn = int (HLE_SYSV_ABI*)(int);
    using FiosOpCountFn = s64 (HLE_SYSV_ABI*)(int);
    using FiosDhReadFn = int (HLE_SYSV_ABI*)(const void*, int, void*);
    using FiosDhCloseFn = int (HLE_SYSV_ABI*)(const void*, int);
    using FiosOpenFn = int (HLE_SYSV_ABI*)(const void*, int*, const char*, const void*);
    using FiosStatFn = int (HLE_SYSV_ABI*)(const void*, int, void*);
    using FiosReadFn = s64 (HLE_SYSV_ABI*)(const void*, int, void*, s64);
    using FiosSeekFn = s64 (HLE_SYSV_ABI*)(int, s64, int);
    int dh = 0;
    const int probe_op = Guest<FiosDhOpenFn>("sceFiosDHOpen")(nullptr, &dh, "/app0/sce_sys", nullptr, 0);
    const int probe = Guest<FiosOpFn>("sceFiosOpWait")(probe_op);
    const s64 needed = Guest<FiosOpCountFn>("sceFiosOpGetActualCount")(probe_op);
    Guest<FiosOpFn>("sceFiosOpDelete")(probe_op);
    std::vector<u8> dir_buffer(needed > 0 ? static_cast<size_t>(needed) : 1);
    const int dho = Guest<FiosDhOpenFn>("sceFiosDHOpenSync")(nullptr, &dh, "/app0/sce_sys", dir_buffer.data(),
                                                              dir_buffer.size());
    static u8 entry[0x440];
    int entries = 0;
    bool saw_param_sfo = false;
    while (dho == 0 && Guest<FiosDhReadFn>("sceFiosDHReadSync")(nullptr, dh, entry) == 0) {
        u16 name_offset;
        std::memcpy(&name_offset, entry + 0x10, 2);
        saw_param_sfo |= !std::strcmp(reinterpret_cast<const char*>(entry + 0x18 + name_offset), "param.sfo");
        ++entries;
    }
    Guest<FiosDhCloseFn>("sceFiosDHCloseSync")(nullptr, dh);
    int fh = 0;
    const int fo = Guest<FiosOpenFn>("sceFiosFHOpenSync")(nullptr, &fh, "/app0/sce_sys/param.sfo", nullptr);
    u8 stat[80] = {};
    const int fst = Guest<FiosStatFn>("sceFiosFHStatSync")(nullptr, fh, stat);
    s64 stat_size;
    std::memcpy(&stat_size, stat, 8);
    u8 magic[4] = {};
    const s64 frd = Guest<FiosReadFn>("sceFiosFHReadSync")(nullptr, fh, magic, 4);
    const s64 end_pos = Guest<FiosSeekFn>("sceFiosFHSeek")(fh, 0, 2);
    Guest<FiosDhCloseFn>("sceFiosFHCloseSync")(nullptr, fh);
    const int missing = Guest<FiosOpenFn>("sceFiosFHOpenSync")(nullptr, &fh, "/app0/nao_existe.dat", nullptr);
    check("Fios2: DHOpen without buffer = 0x80820008 + size, listing finds param.sfo, read, 0x80820005",
          probe == static_cast<int>(0x80820008) && needed > 0 && dho == 0 && entries > 5 && saw_param_sfo &&
              fo == 0 && fst == 0 && stat_size > 0 && !(stat[0x20] & 1) && frd == 4 &&
              !std::memcmp(magic, "\0PSF", 4) && end_pos == stat_size && missing == static_cast<int>(0x80820005));

    // ---- equeue: timeout, entrega com os campos certos, fusao de disparos, acordar outra thread
    using CreateEqFn = int (HLE_SYSV_ABI*)(void**, const char*);
    using WaitEqFn = int (HLE_SYSV_ABI*)(void*, void*, int, int*, u32*);
    namespace eq = hle::equeue;
    void* queue = nullptr;
    Guest<CreateEqFn>("sceKernelCreateEqueue")(&queue, "selftest");
    eq::KernelEvent events[4] = {};
    int event_count = -1;
    u32 eq_timeout = 20000;
    void* const tag = reinterpret_cast<void*>(0xABCDEF);
    eq::Register(queue, 0x99, -11, tag);
    const int w1 = Guest<WaitEqFn>("sceKernelWaitEqueue")(queue, events, 4, &event_count, &eq_timeout);
    const bool eq_timeout_ok = w1 == hle::err::kETIMEDOUT && event_count == 0 && eq_timeout == 20000;
    eq::Trigger(queue, 0x99, -11, 111);
    eq::Trigger(queue, 0x99, -11, 222); // funde com o anterior
    const bool unregistered_refused = !eq::Trigger(queue, 0x55, -11, 0);
    const int w2 = Guest<WaitEqFn>("sceKernelWaitEqueue")(queue, events, 4, &event_count, nullptr);
    const bool eq_event_ok = w2 == 0 && event_count == 1 && events[0].ident == 0x99 && events[0].filter == -11 &&
                             events[0].data == 222 && events[0].udata == tag;
    std::thread waker([queue] {
        Sleep(30);
        eq::Trigger(queue, 0x99, -11, 333);
    });
    const ULONGLONG eq_start = GetTickCount64();
    const int w3 = Guest<WaitEqFn>("sceKernelWaitEqueue")(queue, events, 4, &event_count, nullptr);
    const ULONGLONG eq_ms = GetTickCount64() - eq_start;
    waker.join();
    check("equeue: timeout without event, event with ident/filter/data/udata, coalesced triggers, wakes another thread",
          eq_timeout_ok && eq_event_ok && unregistered_refused && w3 == 0 && event_count == 1 &&
              events[0].data == 333 && eq_ms >= 15 && eq_ms < 500);

    // ---- PM4 + flip: labels escritos de verdade, evento de EOP e evento de flip no VSync
    using VoOpenFn = int (HLE_SYSV_ABI*)(int, int, int, const void*);
    using VoAddFlipFn = int (HLE_SYSV_ABI*)(void*, int, void*);
    using VoSetAttrFn = void (HLE_SYSV_ABI*)(void*, u32, u32, u32, u32, u32, u32);
    using VoRegisterFn = int (HLE_SYSV_ABI*)(int, int, void* const*, int, const void*);
    using GnmAddEqFn = int (HLE_SYSV_ABI*)(void*, u64, void*);
    using GnmWriterFn = int (HLE_SYSV_ABI*)(u32*, u32, const u32*, u32); // (cmdbuf, size, registradores, modificador)
    using GnmSubmitFlipFn = int (HLE_SYSV_ABI*)(u32, const u32* const*, const u32*, const u32* const*, const u32*,
                                                 u32, u32, u32, s64);
    void* flip_queue = nullptr;
    Guest<CreateEqFn>("sceKernelCreateEqueue")(&flip_queue, "selftest-flip");
    const int vo = Guest<VoOpenFn>("sceVideoOutOpen")(0xFF, 0, 0, nullptr);
    const int vaf = Guest<VoAddFlipFn>("sceVideoOutAddFlipEvent")(flip_queue, vo, tag);
    u8 attr[0x28];
    Guest<VoSetAttrFn>("sceVideoOutSetBufferAttribute")(attr, 0x80000000, 0, 0, 1920, 1080, 1920);
    u32 attr_width, attr_pitch;
    std::memcpy(&attr_width, attr + 12, 4);
    std::memcpy(&attr_pitch, attr + 20, 4); // 7o argumento: veio pela pilha
    static u8 fake_framebuffer[64];
    void* framebuffers[2] = {fake_framebuffer, fake_framebuffer};
    const int vreg = Guest<VoRegisterFn>("sceVideoOutRegisterBuffers")(vo, 0, framebuffers, 2, attr);
    const int gae = Guest<GnmAddEqFn>("sceGnmAddEqEvent")(flip_queue, 0x40, tag);

    static u64 eop_label, dma_label;
    static u32 eos_label, write_label[2];
    eop_label = dma_label = 0;
    eos_label = write_label[0] = write_label[1] = 0;
    const auto lo = [](const void* p) { return static_cast<u32>(reinterpret_cast<u64>(p)); };
    const auto hi = [](const void* p) { return static_cast<u32>(reinterpret_cast<u64>(p) >> 32); };
    const auto pm4 = [](u32 opcode, u32 total_dwords) { return 0xC0000000u | ((total_dwords - 2) << 16) | (opcode << 8); };
    static u32 dcb[64];
    u32 dw = 0;
    // 29 dwords que uma funcao do driver preenche (tem de virar um NOP percorrivel)
    const int writer = Guest<GnmWriterFn>("sceGnmSetVsShader")(dcb, 29, nullptr, 0);
    dw += 29;
    dcb[dw++] = pm4(0x37, 6); // WRITE_DATA: 2 dwords para a memoria
    dcb[dw++] = 5u << 8;
    dcb[dw++] = lo(write_label);
    dcb[dw++] = hi(write_label);
    dcb[dw++] = 0x11111111;
    dcb[dw++] = 0x22222222;
    dcb[dw++] = pm4(0x50, 7); // DMA_DATA: dado imediato, 8 bytes
    dcb[dw++] = 2u << 29;
    dcb[dw++] = 0xAABBCCDD;
    dcb[dw++] = 0;
    dcb[dw++] = lo(&dma_label);
    dcb[dw++] = hi(&dma_label);
    dcb[dw++] = 8;
    dcb[dw++] = pm4(0x48, 5); // EVENT_WRITE_EOS: fence de 32 bits
    dcb[dw++] = 0;
    dcb[dw++] = lo(&eos_label);
    dcb[dw++] = (hi(&eos_label) & 0xFFFF) | (2u << 29);
    dcb[dw++] = 0x33333333;
    dcb[dw++] = pm4(0x47, 6); // EVENT_WRITE_EOP: 64 bits + interrupcao
    dcb[dw++] = 0x504;
    dcb[dw++] = lo(&eop_label);
    dcb[dw++] = (hi(&eop_label) & 0xFFFF) | (2u << 24) | (2u << 29);
    dcb[dw++] = 0x55555555;
    dcb[dw++] = 0x66666666;
    const u32* dcb_list[1] = {dcb};
    const u32 dcb_sizes[1] = {dw * 4};
    const u64 flips_before = hle::videoout::FlipCount();
    const int sf = Guest<GnmSubmitFlipFn>("sceGnmSubmitAndFlipCommandBuffers")(1, dcb_list, dcb_sizes, nullptr, nullptr,
                                                                              vo, 1, 3, 0x1234);
    // Os labels tem de valer assim que o submit retorna.
    const bool labels_ok = write_label[0] == 0x11111111 && write_label[1] == 0x22222222 &&
                           dma_label == 0xAABBCCDDAABBCCDDULL && eos_label == 0x33333333 &&
                           eop_label == 0x6666666655555555ULL;
    bool got_eop = false, got_flip = false;
    s64 flip_data = 0;
    for (int attempt = 0; attempt < 6 && !(got_eop && got_flip); ++attempt) {
        u32 wait = 100000;
        if (Guest<WaitEqFn>("sceKernelWaitEqueue")(flip_queue, events, 4, &event_count, &wait) != 0) {
            continue;
        }
        for (int i = 0; i < event_count; ++i) {
            if (events[i].ident == 0x40 && events[i].filter == eq::kFilterGraphicsCore && events[i].data == 0x40) {
                got_eop = true;
            }
            if (events[i].ident == 0x6 && events[i].filter == eq::kFilterVideoOut && events[i].udata == tag) {
                got_flip = true;
                flip_data = events[i].data;
            }
        }
    }
    const hle::videoout::FlipStatus status = hle::videoout::GetFlipStatus(vo);
    check("PM4/flip: driver NOP traversable, WRITE_DATA/DMA_DATA/EOS/EOP write labels, EOP event, flip on VSync",
          vo >= 1 && vaf == 0 && attr_width == 1920 && attr_pitch == 1920 && vreg == 0 && gae == 0 && writer == 0 &&
              sf == 0 && labels_ok && got_eop && got_flip && (flip_data >> 16) == 0x1234 &&
              hle::videoout::FlipCount() == flips_before + 1 && status.flip_arg == 0x1234 &&
              status.current_buffer == 1 && status.flip_pending_num == 0);

    // ---- rand: 31 bits como no Orbis (a MSVCRT so da 15)
    using RandFn = int (HLE_SYSV_ABI*)();
    int rand_max_seen = 0;
    bool rand_negative = false;
    for (int i = 0; i < 2000; ++i) {
        const int v = Guest<RandFn>("rand")();
        rand_negative |= v < 0;
        rand_max_seen = v > rand_max_seen ? v : rand_max_seen;
    }
    check("rand: never negative and exceeds 0x7FFF (31-bit range)", !rand_negative && rand_max_seen > 0x3FFFFFFF);

    // ---- Toy Pad: protocolo puro
    namespace tp = hle::toypad;
    const u8 wake_cmd[32] = {0x55, 0x0f, 0xb0, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4c, 0x45, 0x47,
                             0x4f, 0x20, 0x32, 0x30, 0x31, 0x34, 0xf7};
    const u8 wake_expected[17] = {0x55, 0x0e, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4c, 0x45,
                                  0x47, 0x4f, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
    const auto wake_reply = tp::HandleCommand(wake_cmd, 32);
    const bool wake_ok = wake_reply && !std::memcmp(wake_reply->data(), wake_expected, 17);
    // seed: [semente LE][confirmacao] cifrado -> resposta decifrada comeca pela confirmacao
    const u8 seed_plain[8] = {0x78, 0x56, 0x34, 0x12, 0xDE, 0xAD, 0xBE, 0xEF};
    u8 seed_cmd[32] = {0x55, 0x0a, 0xb1, 0x02};
    std::memcpy(seed_cmd + 4, tp::Encrypt(seed_plain).data(), 8);
    seed_cmd[12] = tp::Checksum(seed_cmd, 12);
    const auto seed_reply = tp::HandleCommand(seed_cmd, 32);
    bool seed_ok = seed_reply && (*seed_reply)[1] == 0x09 && (*seed_reply)[2] == 0x02 &&
                   (*seed_reply)[11] == tp::Checksum(seed_reply->data(), 11);
    if (seed_ok) {
        const auto plain = tp::Decrypt(seed_reply->data() + 3);
        seed_ok = !std::memcmp(plain.data(), seed_plain + 4, 4) && !plain[4] && !plain[5] && !plain[6] && !plain[7];
    }
    const u8 round_trip_in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const bool tea_ok = tp::Decrypt(tp::Encrypt(round_trip_in).data()) == std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 8};
    const u8 led_cmd[32] = {0x55, 0x06, 0xc0, 0x07, 0x01, 0xff, 0x00, 0x00, 0x22};
    const auto led_reply = tp::HandleCommand(led_cmd, 32);
    const bool led_ok = led_reply && (*led_reply)[0] == 0x55 && (*led_reply)[1] == 0x01 && (*led_reply)[2] == 0x07 &&
                        (*led_reply)[3] == 0x5D;
    check("Toy Pad: byte-exact WAKE response, SEED (TEA) returns confirmation, LED ack with checksum",
          wake_ok && seed_ok && tea_ok && led_ok);

    // ---- Fase 60: estado dos LEDs para o app (GET_LED v2): cor, piscar e fade, com a cor de origem do fade
    {
        auto led = [](std::initializer_list<u8> body) { // monta 0x55, tamanho, comando, sequencia, payload, checksum
            u8 cmd[32] = {0x55, static_cast<u8>(body.size())}; // o tamanho conta comando + sequencia + payload
            std::copy(body.begin(), body.end(), cmd + 2);
            cmd[2 + cmd[1]] = tp::Checksum(cmd, 2 + cmd[1]);
            tp::HandleCommand(cmd, 32);
        };
        u8 before[tp::kLedSnapshotBytes];
        tp::LedSnapshot(before);
        led({0xc0, 0x10, 0x02, 0x10, 0x20, 0x30});                   // esquerda: cor fixa
        led({0xc3, 0x11, 0x03, 0x05, 0x06, 0xff, 0x00, 0xff, 0x00}); // direita: pisca 5/6 ticks para sempre
        led({0xc2, 0x12, 0x02, 0x19, 0x04, 0x40, 0x50, 0x60});        // esquerda: fade a partir da cor anterior
        u8 snap[tp::kLedSnapshotBytes];
        tp::LedSnapshot(snap);
        const u8* left = snap + 4 + 1 * 12;
        const u8* right = snap + 4 + 2 * 12;
        const bool led_v2_ok = snap[0] == 'L' && snap[2] == 2 && snap[3] == 3 && snap[1] != before[1] && left[0] == 2 && left[1] == 3 &&
                               left[2] == 0x40 && left[3] == 0x50 && left[4] == 0x60 && left[5] == 0x10 && left[6] == 0x20 && left[7] == 0x30 &&
                               left[10] == 0x04 && left[11] == 0x19 && right[0] == 3 && right[1] == 2 && right[2] == 0 && right[3] == 0xff &&
                               right[8] == 5 && right[9] == 6 && right[10] == 0;
        led({0xc8, 0x13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); // apaga tudo
        check("Toy Pad: LEDs to app (GET_LED v2, 40 bytes): color, blink, fade with source color; all-colors clears", led_v2_ok);
    }

    // ---- figura sintetica: tag coerente com o UID, evento de colocacao, leitura e modelo
    const tp::Uid fig_uid = {4, 163, 55, 18, 34, 55, 66};
    const tp::TagData fig_tag = tp::BuildTag(fig_uid, 1);
    const auto fig_key = tp::FigureKey(fig_uid);
    const auto fig_plain = tp::Decrypt(&fig_tag[0x24 * 4], fig_key.data());
    const bool tag_ok = fig_tag[0] == 4 && fig_tag[1] == 163 && fig_tag[2] == 55 && fig_tag[3] == (0x88 ^ 4 ^ 163 ^ 55) && fig_tag[4] == 18 &&
                        fig_plain[0] == 1 && !fig_plain[1] && fig_plain[4] == 1 && !fig_plain[5];
    tp::RemoveFigure(1);
    const auto placed = tp::PlaceFigure(1, fig_uid, 1);
    const u8 placed_expected[13] = {0x56, 0x0b, 0x01, 0x00, 0x01, 0x00, 4, 163, 55, 18, 34, 55, 66};
    const bool placed_ok = placed && !std::memcmp(placed->data(), placed_expected, 13) &&
                           (*placed)[13] == tp::Checksum(placed->data(), 13) && !tp::PlaceFigure(1, fig_uid, 1);
    u8 read_cmd[32] = {0x55, 0x04, 0xd2, 0x09, 0x01, 0x00}; // indice 1, pagina 0
    read_cmd[6] = tp::Checksum(read_cmd, 6);
    const auto read_reply = tp::HandleCommand(read_cmd, 32);
    const bool read_ok = read_reply && (*read_reply)[1] == 0x12 && (*read_reply)[3] == 0 &&
                         !std::memcmp(read_reply->data() + 4, fig_tag.data(), 16);
    const u8 model_plain[8] = {0x01, 0, 0, 0, 0xCA, 0xFE, 0xBA, 0xBE}; // indice 1 + confirmacao
    u8 model_cmd[32] = {0x55, 0x0a, 0xd4, 0x0a};
    std::memcpy(model_cmd + 4, tp::Encrypt(model_plain).data(), 8);
    model_cmd[12] = tp::Checksum(model_cmd, 12);
    const auto model_reply = tp::HandleCommand(model_cmd, 32);
    bool model_ok = model_reply && (*model_reply)[1] == 0x0a && (*model_reply)[3] == 0;
    if (model_ok) {
        const auto plain = tp::Decrypt(model_reply->data() + 4);
        model_ok = plain[0] == 1 && !plain[1] && !plain[2] && !plain[3] && !std::memcmp(plain.data() + 4, model_plain + 4, 4);
    }
    const auto removed = tp::RemoveFigure(1);
    const bool removed_ok = removed && (*removed)[5] == 1 && (*removed)[2] == 1 && !tp::RemoveFigure(1);
    check("Toy Pad figure: synthetic tag decodes to id, placed/removed event 56 0b, READ and MODEL",
          tag_ok && placed_ok && read_ok && model_ok && removed_ok);

    // ---- portal.json com as tres zonas, e tres figuras ao mesmo tempo no Toy Pad
    {
        const std::string trio = "{\"pad1\": {\"uid\": [4,163,55,18,34,55,66], \"id\": 1},"
                                 " \"pad2\": {\"uid\": [4,81,112,26,91,64,129], \"id\": 2},"
                                 " \"pad3\": {\"id\": 3, \"uid\": [4,202,17,74,153,54,128]}}";
        const std::string partial = "{\"pad1\": {\"uid\": [4,163,55,18,34,55,66], \"id\": 1}, \"pad2\": null,"
                                    " \"pad3\": {\"uid\": [1,2,3], \"id\": 9}}";
        tp::Uid uids[3];
        u32 ids[3] = {};
        bool parse_ok = true;
        for (int pad = 1; pad <= 3; ++pad) {
            parse_ok &= hle::ParsePortalJson(trio, pad, &uids[pad - 1], &ids[pad - 1]);
        }
        parse_ok &= ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && uids[1][1] == 81 && uids[2][6] == 128 && uids[0] != uids[1];
        tp::Uid scratch_uid;
        u32 scratch_id = 0;
        // zona null e zona com UID incompleto contam como vazias, sem afetar a zona 1
        parse_ok &= hle::ParsePortalJson(partial, 1, &scratch_uid, &scratch_id) && scratch_id == 1 &&
                    !hle::ParsePortalJson(partial, 2, &scratch_uid, &scratch_id) &&
                    !hle::ParsePortalJson(partial, 3, &scratch_uid, &scratch_id);
        check("portal.json: three independent zones (any key order; null and incomplete UID = empty zone)", parse_ok);

        auto read_page0 = [&](u8 index, u8 seq) {
            u8 cmd[32] = {0x55, 0x04, 0xd2, seq, index, 0x00};
            cmd[6] = tp::Checksum(cmd, 6);
            return tp::HandleCommand(cmd, 32);
        };
        bool multi_ok = true;
        for (int pad = 1; pad <= 3; ++pad) {
            tp::RemoveFigure(pad);
        }
        for (int pad = 1; pad <= 3; ++pad) {
            const auto event = tp::PlaceFigure(pad, uids[pad - 1], ids[pad - 1]);
            multi_ok &= event && (*event)[0] == 0x56 && (*event)[2] == pad && (*event)[4] == pad && (*event)[5] == 0 &&
                        !std::memcmp(event->data() + 6, uids[pad - 1].data(), 7);
        }
        // as tres respondem a leitura, cada uma com a propria tag
        for (int pad = 1; pad <= 3; ++pad) {
            const auto reply = read_page0(static_cast<u8>(pad), static_cast<u8>(0x20 + pad));
            const tp::TagData tag = tp::BuildTag(uids[pad - 1], ids[pad - 1]);
            multi_ok &= reply && (*reply)[3] == 0 && !std::memcmp(reply->data() + 4, tag.data(), 16);
        }
        // tirar a do meio nao mexe nas outras duas
        const auto removed_event = tp::RemoveFigure(2);
        multi_ok &= removed_event && (*removed_event)[2] == 2 && (*removed_event)[5] == 1;
        const tp::TagData gandalf = tp::BuildTag(uids[1], ids[1]);
        const auto gone = read_page0(2, 0x30);
        multi_ok &= !(gone && (*gone)[3] == 0 && !std::memcmp(gone->data() + 4, gandalf.data(), 16));
        for (int pad : {1, 3}) {
            const auto reply = read_page0(static_cast<u8>(pad), static_cast<u8>(0x40 + pad));
            const tp::TagData tag = tp::BuildTag(uids[pad - 1], ids[pad - 1]);
            multi_ok &= reply && (*reply)[3] == 0 && !std::memcmp(reply->data() + 4, tag.data(), 16);
        }
        tp::RemoveFigure(1);
        tp::RemoveFigure(3);
        check("Toy Pad multi-tag: three figures placed, read by index and independently removed", multi_ok);
        // Fase 60: a mesma tag (mesmo arquivo do app) em duas posicoes: a segunda copia ganha um UID novo, com o
        // id recifrado para ele (o jogo descartaria duas figuras com o mesmo UID)
        const tp::TagData copy_tag = tp::BuildTag(uids[0], ids[0]);
        tp::RemoveIndex(4);
        tp::RemoveIndex(5);
        const auto first_copy = tp::LoadTag(1, 4, copy_tag, std::string());
        const auto second_copy = tp::LoadTag(2, 5, copy_tag, std::string());
        const bool dup_ok = first_copy && second_copy && !std::memcmp(first_copy->data() + 6, copy_tag.data(), 3) &&
                            std::memcmp(second_copy->data() + 6, first_copy->data() + 6, 7) != 0 && (*second_copy)[6] == 0x04;
        u32 second_id = 0;
        tp::RemoveIndex(5);
        const auto again = tp::LoadTag(2, 5, copy_tag, std::string(), &second_id);
        tp::RemoveIndex(4);
        tp::RemoveIndex(5);
        check("Toy Pad: second copy of same tag gets new UID and remains same character", dup_ok && again && second_id == ids[0]);
    }

    // ---- extrator de shaders: acha o OrbShdr a partir dos registradores e grava um arquivo por shader distinto
    {
        using SetShaderFn = int (HLE_SYSV_ABI*)(u32*, u32, const u32*, u32);
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lego_dimensions_selftest_shaders";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        hle::gnm::SetShaderDumpDirectory(dir);
        // "shader" falso: mov vcc_hi + distancia, 6 dwords de codigo, OrbShdr com length = 32 bytes
        u32* code = static_cast<u32*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        const u64 address = reinterpret_cast<u64>(code);
        code[0] = 0xBEEB03FF;
        code[1] = 3; // info em (3 + 1) * 8 = 32 bytes
        for (u32 i = 2; i < 8; ++i) {
            code[i] = 0x1000 + i;
        }
        std::memcpy(code + 8, "OrbShdr", 7);
        code[10] = 32u << 8;
        code[14] = 0x12345678; // crc32
        u32 regs[12] = {static_cast<u32>(address >> 8), static_cast<u32>(address >> 40)};
        u32 cmdbuf[0x28];
        const int r1 = Guest<SetShaderFn>("sceGnmSetVsShader")(cmdbuf, 0x1D, regs, 0);
        const int r2 = Guest<SetShaderFn>("sceGnmUpdateVsShader")(cmdbuf, 0x1D, regs, 0); // repetido: nao grava de novo
        const int r3 = Guest<SetShaderFn>("sceGnmSetPsShader")(cmdbuf, 0x28, regs, 0);
        const int r4 = Guest<SetShaderFn>("sceGnmSetPsShader")(cmdbuf, 0x28, nullptr, 0); // PS desligado
        hle::gnm::FlushShaderDumps();
        u32 vs_files = 0, ps_files = 0;
        u64 size = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = entry.path().filename().string();
            vs_files += name.rfind("vs_", 0) == 0;
            ps_files += name.rfind("ps_", 0) == 0;
            if (name.rfind("vs_", 0) == 0) {
                size = entry.file_size();
            }
        }
        check("shader extractor: OrbShdr found via registers, one file per distinct shader (VS and PS), size = code + header",
              r1 == 0 && r2 == 0 && r3 == 0 && r4 == 0 && vs_files == 1 && ps_files == 1 && size == 32 + 28 &&
                  // PS nulo: o pacote comeca com SET_SH_REG de SPI_SHADER_PGM_LO_PS (0x08) = 0 e o resto e NOP
                  cmdbuf[0] == (0xC0000000u | (2u << 16) | (0x76u << 8)) && cmdbuf[1] == 0x08 && cmdbuf[2] == 0 &&
                  cmdbuf[4] == (0xC0000000u | ((0x28 - 4 - 2) << 16) | (0x10 << 8)));
        // memoria do shader desmapeada (nivel descarregado): a chamada seguinte com o mesmo
        // endereco nao pode derrubar o processo
        VirtualFree(code, 0, MEM_RELEASE);
        const int r5 = Guest<SetShaderFn>("sceGnmSetVsShader")(cmdbuf, 0x1D, regs, 0);
        check("shader extractor: previously seen address later unmapped is ignored without exception", r5 == 0);
        hle::gnm::SetShaderDumpDirectory({});
        std::filesystem::remove_all(dir, ec);
    }

    // ---- Vulkan: instancia, superficie numa janela propria, dispositivo, swapchain e 3 frames
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "LegoDimensionsPcSelftestVulkan";
        RegisterClassA(&wc);
        RECT rect{0, 0, 1280, 720};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowA(wc.lpszClassName, "selftest", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        auto& renderer = render::VulkanRenderer::Instance();
        const bool init = hwnd && renderer.Initialize(hwnd);
        u32 shown = 0;
        for (int i = 0; init && i < 3; ++i) {
            shown += renderer.DrawFrame();
        }
        // um quad (lista de quads -> dois triangulos) e um triangulo indexado, pelo caminho dos draws do jogo
        bool geometry = false;
        if (init) {
            struct Vertex {
                float x, y, z;
                u8 rgba[4];
            };
            const Vertex quad[4] = {{-0.5f, -0.5f, 0, {255, 0, 0, 255}}, {0.5f, -0.5f, 0, {0, 255, 0, 255}},
                                    {0.5f, 0.5f, 0, {0, 0, 255, 255}}, {-0.5f, 0.5f, 0, {255, 255, 255, 255}}};
            const u16 triangle_indices[3] = {0, 1, 2};
            render::DrawCall draw;
            draw.vertex_data = quad;
            draw.vertex_count = 4;
            draw.stride = sizeof(Vertex);
            draw.has_color = true;
            draw.color_offset = 12;
            draw.primitive = render::DrawCall::kQuadList;
            draw.mvp[0] = draw.mvp[5] = draw.mvp[10] = draw.mvp[15] = 1.0f;
            const bool queued_quad = renderer.QueueDraw(draw);
            draw.primitive = render::DrawCall::kTriangleList;
            draw.index_data = triangle_indices;
            draw.index_count = 3;
            draw.has_color = false; // usa o branco fixo do binding 1
            const bool queued_triangle = renderer.QueueDraw(draw);
            const bool frame = renderer.DrawFrame();
            geometry = queued_quad && queued_triangle && frame && renderer.DrawsRendered() == 2 && renderer.PipelinesCreated() == 2 &&
                       renderer.LastFrameDraws() == 2;
        }
        check("Textures: microtile detile (Phase 59) matches element-by-element detile (BC 8/16 bytes, clipped edges)",
              hle::gnm::SelftestDetile());
        check("Vulkan: draws with vertices/indices copied to ring buffers, 2 pipelines (with and without color), vkCmdDrawIndexed in frame",
              geometry);
        std::printf("         (%s; %u of 3 frames presented)\n", init ? renderer.Describe().c_str() : renderer.LastError().c_str(),
                    shown);
        check("Vulkan: instance + Win32 surface + device + swapchain 1280x720 B8G8R8A8 FIFO, 3 frames cleared and presented",
              init && shown == 3 && renderer.Describe().find("1280x720 B8G8R8A8_UNORM") != std::string::npos);
        renderer.Shutdown();
        if (hwnd) {
            DestroyWindow(hwnd);
        }
    }

    // ---- Pad: roteiro de entrada (um botao por um intervalo) e sticks centrados
    using PadReadFn = int (HLE_SYSV_ABI*)(int, void*);
    u8 pad_data[0x78];
    std::memset(pad_data, 0xCC, sizeof(pad_data));
    const int pad_read = Guest<PadReadFn>("scePadReadState")(1, pad_data);
    u32 pad_buttons;
    std::memcpy(&pad_buttons, pad_data, 4);
    // Sem janela em foco e sem roteiro ativo: so um controle XInput poderia mexer nisto.
    std::printf("         (buttons read now: 0x%x, sticks %u %u %u %u)\n", pad_buttons, pad_data[4], pad_data[5],
                pad_data[6], pad_data[7]);
    {
        u8 other[0x78];
        // Fase 61: handles 1 a 4 (co-op local) valem; o 5 nao
        const bool coop_handles = Guest<PadReadFn>("scePadReadState")(2, other) == 0 && Guest<PadReadFn>("scePadReadState")(5, other) < 0;
        check("Pad: ReadState returns connected, timestamp and (without controller) sticks at 0x80; handles 1-4 valid (co-op), 5 invalid",
              pad_read == 0 && pad_data[0x4C] == 1 && coop_handles);
    }
    // ---- roteiro com eixos analogicos: "LS_UP:ms" segura LeftY em 0, o resto fica em 128
    {
        const bool parsed = hle::SetPadInputScript("LS_UP:600000");
        std::memset(pad_data, 0xCC, sizeof(pad_data));
        Guest<PadReadFn>("scePadReadState")(1, pad_data);
        const bool up = pad_data[4] == 0x80 && pad_data[5] == 0x00 && pad_data[6] == 0x80 && pad_data[7] == 0x80;
        // sequencia: o segundo item comeca quando o primeiro termina; formas mistas e maiusculas
        const bool mixed = hle::SetPadInputScript("0:ls_right:600000,0:RS_DOWN:600000,0:cross:600000");
        std::memset(pad_data, 0xCC, sizeof(pad_data));
        Guest<PadReadFn>("scePadReadState")(1, pad_data);
        u32 buttons_now;
        std::memcpy(&buttons_now, pad_data, 4);
        const bool combined = pad_data[4] == 0xFF && pad_data[5] == 0x80 && pad_data[7] == 0xFF && (buttons_now & 0x4000);
        const bool rejected = !hle::SetPadInputScript("ls_sideways:100") && !hle::SetPadInputScript("ls_up:100:5");
        hle::SetPadInputScript("");
        std::memset(pad_data, 0xCC, sizeof(pad_data));
        Guest<PadReadFn>("scePadReadState")(1, pad_data);
        check("Pad: script with sticks (LS_UP = LeftY 0, LS_RIGHT = LeftX 255, RS_DOWN = RightY 255, with button; center 128 on release)",
              parsed && up && mixed && combined && rejected && pad_data[5] == 0x80 && pad_data[4] == 0x80);
    }

    // Offsets que o consumidor do jogo (eboot+0x4b9040) le, mais os pedidos na Fase 9.
    u64 pad_ts1, pad_ts2;
    std::memset(pad_data, 0xCC, sizeof(pad_data));
    Guest<PadReadFn>("scePadReadState")(1, pad_data);
    std::memcpy(&pad_ts1, pad_data + 0x50, 8);
    const bool pad_layout = pad_data[0x4C] == 1 && pad_data[0x68] == 1 && pad_data[0x34] == 0 && !(pad_data[3] & 0x80);
    Guest<PadReadFn>("scePadReadState")(1, pad_data);
    std::memcpy(&pad_ts2, pad_data + 0x50, 8);
    check("Pad: connected at +0x4C, connectedCount at +0x68, no 'intercepted' bit, increasing timestamp",
          pad_layout && pad_ts2 > pad_ts1);

    // ---- Radar de telas: (pacote, offset) -> nome, cada arquivo conta uma vez
    {
        const auto radar_dir = std::filesystem::path(LD_PROJECT_ROOT) / "build" / "selftest_radar";
        std::filesystem::create_directories(radar_dir);
        {
            std::ofstream index(radar_dir / "TESTE.tsv");
            index << "\\levels\\frontend\\frontend_dx11.gsc\t4000\t1000\t5000\t3\n"
                  << "\\audio\\musica.ogg\t200\t200\t100\t0\n"
                  << "\\stuff\\text\\text.csv\t50\t50\t6000\t0\n";
        }
        hle::radar::Configure(radar_dir, radar_dir / "radar.log"); // sem log o radar nao faz nada (1.8.3)
        hle::radar::OnRead("/app0/data/chunk1/TESTE.DAT", 5500, 16);  // meio do .gsc
        hle::radar::OnRead("/app0/data/chunk1/TESTE.DAT", 5600, 16);  // o mesmo: nao conta de novo
        const bool radar_one = hle::radar::FilesSeen() == 1;
        hle::radar::OnRead("/app0/data/chunk1/TESTE.DAT", 0, 100);    // antes do .ogg (offset 100)
        hle::radar::OnRead("/app0/data/chunk1/OUTRO.HDR", 5500, 16);  // nao e pacote
        const bool radar_none = hle::radar::FilesSeen() == 1;
        hle::radar::OnRead("/app0/data/chunk1/TESTE.DAT", 250, 5800); // .ogg + (gsc ja visto) + .csv
        check("Radar: archive offset becomes filename, once per file, screen filter matches",
              radar_one && radar_none && hle::radar::FilesSeen() == 3 &&
                  hle::radar::IsScreenFile("\\levels\\frontend\\frontend_dx11.gsc") &&
                  hle::radar::IsScreenFile("\\levels\\loading_screen_startup\\a.led") &&
                  !hle::radar::IsScreenFile("\\audio\\musica.ogg"));
        // o player de video acha o .mp4 assim: so arquivo guardado sem compressao (o .gsc comprimido nao conta)
        hle::radar::StoredFile stored;
        check("Radar: stored (uncompressed) file found by name suffix, with pack path, offset and size",
              hle::radar::FindStoredFile(".ogg", &stored) && stored.guest_pack == "/app0/data/chunk1/TESTE.DAT" &&
                  stored.offset == 100 && stored.size == 200 && !hle::radar::FindStoredFile(".gsc", &stored));
        hle::radar::Configure({}, {});
    }

    // ---- Toy Pad pela API da libSceUsbd: OUT com comando -> IN pendente recebe a resposta
    using UsbOpenFn = void* (HLE_SYSV_ABI*)(u16, u16);
    using UsbAllocFn = UsbTransferView* (HLE_SYSV_ABI*)(int);
    using UsbFillFn = void (HLE_SYSV_ABI*)(UsbTransferView*, void*, u8, u8*, int, void*, void*, u32);
    using UsbSubmitFn = int (HLE_SYSV_ABI*)(UsbTransferView*);
    hle::SetVirtualToypadConnected(true);
    void* usb_handle = Guest<UsbOpenFn>("sceUsbdOpenDeviceWithVidPid")(0x0E6F, 0x0241);
    static u8 in_buffer[32], out_buffer[32];
    static int in_calls, out_calls;
    in_calls = out_calls = 0;
    std::memset(in_buffer, 0, sizeof(in_buffer));
    std::memcpy(out_buffer, wake_cmd, 32);
    UsbTransferView* in_transfer = Guest<UsbAllocFn>("sceUsbdAllocTransfer")(0);
    UsbTransferView* out_transfer = Guest<UsbAllocFn>("sceUsbdAllocTransfer")(0);
    Guest<UsbFillFn>("sceUsbdFillInterruptTransfer")(in_transfer, usb_handle, 0x81, in_buffer, 32,
                                                     reinterpret_cast<void*>(&SelfTestUsbCallback), &in_calls, 0);
    Guest<UsbFillFn>("sceUsbdFillInterruptTransfer")(out_transfer, usb_handle, 0x01, out_buffer, 32,
                                                     reinterpret_cast<void*>(&SelfTestUsbCallback), &out_calls, 0);
    const bool user_data_ok = in_transfer->user_data == &in_calls; // 7o argumento, veio pela pilha
    const int sub_in = Guest<UsbSubmitFn>("sceUsbdSubmitTransfer")(in_transfer);
    Guest<UsbVoidFn>("sceUsbdHandleEvents")();
    const bool in_stays_pending = in_calls == 0;
    const int sub_out = Guest<UsbSubmitFn>("sceUsbdSubmitTransfer")(out_transfer);
    Guest<UsbVoidFn>("sceUsbdHandleEvents")();
    hle::SetVirtualToypadConnected(false);
    check("Usbd + Toy Pad: read remains pending, OUT command completes read with response, 2 callbacks",
          usb_handle && user_data_ok && sub_in == 0 && in_stays_pending && sub_out == 0 && out_calls == 1 &&
              in_calls == 1 && in_transfer->actual_length == 32 && in_transfer->status == 0 &&
              !std::memcmp(in_buffer, wake_expected, 17));

    // ---- SaveData: criar, gravar pelo VFS, parametros, desmontar, buscar, apagar
    using SdInitFn = int (HLE_SYSV_ABI*)(const void*);
    using SdMountFn = int (HLE_SYSV_ABI*)(const void*, void*);
    using SdPointFn = int (HLE_SYSV_ABI*)(const void*);
    using SdSearchFn = int (HLE_SYSV_ABI*)(const void*, void*);
    using SdSetParamFn = int (HLE_SYSV_ABI*)(const void*, u32, const void*, u64);
    using SdGetParamFn = int (HLE_SYSV_ABI*)(const void*, u32, void*, u64, u64*);
    using SdInfoFn = int (HLE_SYSV_ABI*)(const void*, void*);
    struct {
        s32 user_id, pad;
        const void* title_id;
        const char* dir_name;
        const void* fingerprint;
        u64 blocks;
        u32 mode;
        u8 reserved[32];
    } sd_mount{};
    char sd_name[32] = "SelfTestSlot";
    u8 sd_result[0x40] = {};
    sd_mount.user_id = 1;
    sd_mount.dir_name = sd_name;
    sd_mount.blocks = 0x280;
    struct {
        s32 user_id, pad;
        const void* title_id;
        const char* dir_name;
        u32 unused;
        u8 reserved[32];
        s32 pad2;
    } sd_delete{1, 0, nullptr, sd_name, 0, {}, 0};
    Guest<SdInitFn>("sceSaveDataInitialize")(nullptr);
    Guest<SdPointFn>("sceSaveDataDelete")(&sd_delete); // sobra de execucao anterior
    sd_mount.mode = 2; // abrir: ainda nao existe
    const int sd_missing = Guest<SdMountFn>("sceSaveDataMount")(&sd_mount, sd_result);
    sd_mount.mode = 6; // criar
    const int sd_create = Guest<SdMountFn>("sceSaveDataMount")(&sd_mount, sd_result);
    const std::string sd_point = reinterpret_cast<const char*>(sd_result);
    const int sd_fd = Guest<OpenFn>("sceKernelOpen")((sd_point + "/SAVE.DAT").c_str(), 0x602, 0666);
    const s64 sd_written = Guest<PwriteFn>("sceKernelPwrite")(sd_fd, "LEGO", 4, 0);
    Guest<CloseFn>("sceKernelClose")(sd_fd);
    const int sd_setparam = Guest<SdSetParamFn>("sceSaveDataSetParam")(sd_result, 1, "Test title", 16);
    char sd_title[128] = {};
    u64 sd_got = 0;
    const int sd_getparam = Guest<SdGetParamFn>("sceSaveDataGetParam")(sd_result, 1, sd_title, sizeof(sd_title), &sd_got);
    u64 sd_info[6] = {};
    const int sd_getinfo = Guest<SdInfoFn>("sceSaveDataGetMountInfo")(sd_result, sd_info);
    const int sd_umount = Guest<SdPointFn>("sceSaveDataUmount")(sd_result);
    const int sd_after = Guest<OpenFn>("sceKernelOpen")((sd_point + "/SAVE.DAT").c_str(), 0, 0); // desmontado
    const int sd_exists = Guest<SdMountFn>("sceSaveDataMount")(&sd_mount, sd_result);            // criar de novo
    struct {
        s32 user_id, pad;
        const void* title_id;
        const void* dir_name;
        u32 key, order;
        u8 reserved[32];
    } sd_cond{1, 0, nullptr, nullptr, 3, 0, {}};
    static char sd_names[8][32];
    struct {
        u32 hit_num;
        s32 pad;
        void* dir_names;
        u32 dir_names_num, set_num;
        void* params;
        void* infos;
        u8 reserved[12];
        s32 pad2;
    } sd_search{0, 0, sd_names, 8, 0, nullptr, nullptr, {}, 0};
    const int sd_searched = Guest<SdSearchFn>("sceSaveDataDirNameSearch")(&sd_cond, &sd_search);
    bool sd_found = false;
    for (u32 i = 0; i < sd_search.set_num; ++i) {
        sd_found |= !std::strcmp(sd_names[i], sd_name);
    }
    const bool sd_on_disk = std::filesystem::exists(hle::fs::WritableRoot() / "savedata0" / sd_name / "SAVE.DAT");
    const int sd_deleted = Guest<SdPointFn>("sceSaveDataDelete")(&sd_delete);
    check("SaveData: NOT_FOUND, create, write in /savedataN, Set/GetParam, MountInfo, Umount, EXISTS, search, Delete",
          sd_missing == static_cast<int>(0x809F0008) && sd_create == 0 && sd_point == "/savedata0" && sd_fd >= 3 &&
              sd_written == 4 && sd_setparam == 0 && sd_getparam == 0 && sd_got == 128 &&
              !std::strcmp(sd_title, "Test title") && sd_getinfo == 0 && sd_info[0] > 0x140 && sd_umount == 0 &&
              sd_after < 0 && sd_exists == static_cast<int>(0x809F0007) && sd_searched == 0 && sd_found &&
              sd_search.hit_num >= 1 && sd_on_disk && sd_deleted == 0);

    std::printf("Selftest: %s\n", failures ? "FAILED" : "all OK");
    return failures;
}

} // namespace

// Fase 60: argumento `i` como caminho, lido da linha de comando Unicode (argv chega na pagina ANSI e perde
// caracteres fora dela, como o dois-pontos de largura cheia que ferramentas de extracao poem nos nomes).
std::filesystem::path WideArg(int i, const char* fallback) {
    static const std::vector<std::wstring> args = [] {
        std::vector<std::wstring> out;
        int count = 0;
        if (LPWSTR* list = CommandLineToArgvW(GetCommandLineW(), &count)) {
            out.assign(list, list + count);
            LocalFree(list);
        }
        return out;
    }();
    return i >= 0 && i < static_cast<int>(args.size()) ? std::filesystem::path(args[static_cast<size_t>(i)]) : std::filesystem::path(fallback);
}

int main(int argc, char** argv) {
    // DPI Awareness: evita virtualização de DPI pelo Windows e distorção bilinear para 720p em telas de alta escala
    using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        using SetAware = BOOL(WINAPI*)();
        if (auto set_aware = reinterpret_cast<SetAware>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware"))) {
            set_aware();
        }
    }

    const std::filesystem::path root = FindRoot(argc, argv);

    // Carrega resolucao e tela cheia do launcher.ini como padrao se nao informados na linha de comando
    {
        wchar_t ini_res[64] = {};
        GetPrivateProfileStringW(L"launcher", L"resolucao", L"", ini_res, 64, (root / "launcher.ini").wstring().c_str());
        if (ini_res[0]) {
            unsigned rw = 0, rh = 0;
            if (std::swscanf(ini_res, L"%ux%u", &rw, &rh) == 2 && rw && rh) {
                hle::videoout::SetWindowSize(rw, rh);
            }
        }
        wchar_t ini_full[16] = {};
        GetPrivateProfileStringW(L"launcher", L"telacheia", L"", ini_full, 16, (root / "launcher.ini").wstring().c_str());
        if (ini_full[0] == L'1') {
            hle::videoout::SetStartFullscreen(true);
        }
        wchar_t ini_fps[16] = {};
        GetPrivateProfileStringW(L"launcher", L"fps60", L"1", ini_fps, 16, (root / "launcher.ini").wstring().c_str());
        if (ini_fps[0] == L'0') {
            hle::videoout::SetForce60Hz(false);
            hle::videoout::SetUnlockedFps(false);
        } else if (ini_fps[0] == L'2') {
            hle::videoout::SetForce60Hz(true);
            hle::videoout::SetUnlockedFps(true);
        } else {
            hle::videoout::SetForce60Hz(true);
            hle::videoout::SetUnlockedFps(false);
        }
    }
    std::filesystem::path elf_path = root / "assets" / "decrypted_elfs" / "eboot.bin.elf";
    std::filesystem::path app0 = DefaultApp0(root);
    std::filesystem::path app0_patch; // Fase 58
    std::filesystem::path mods_cache = root / "assets" / "mods_cache"; // Fase 64: mods preparados pelo mods.py ("-" = --no-mods)
    bool live_compiler = true;
    bool shader_warmup = true;
    bool sync_shaders = true;   // Fase 38
    double sync_budget_ms = 50; // Fase 38
    bool sync_textures = true;     // Fase 39
    bool texture_wait = true;      // Fase 45: espera das texturas pendentes no fim do frame
    std::filesystem::path addcont_dir; // Fase 46
    bool addcont_set = false;
    bool loading_screen = true;    // Fase 42
    double texture_budget_ms = 16; // Fase 39
    double texture_spike_budget_ms = 33; // Fase 40
    u32 texture_spike_count = 10;        // Fase 40
    bool test_save_ui = false;
    DWORD timeout_s = 0; // 0 = sem limite (padrao desde a Fase 28; antes eram 15 s, heranca dos testes)
    u64 max_lines = 4000000; // folga para execucoes de 20 min ou mais
    std::filesystem::path shader_dump_dir = root / "assets" / "dumped_shaders";
    u32 cutscene_watchdog_s = 30; // relatorio quando a contagem de draws fica igual por tanto tempo
    bool verbose = false;
    bool selftest = false;
    bool toypad = true; // Fase 27: o Toy Pad virtual passa a ser o padrao (--no-toypad desliga)
    int toypad_port = 9191;
    bool real_toypad = true; // Fase 61: --no-real-toypad ignora o portal fisico
    std::filesystem::path portal_file = root / "assets" / "portal.json";
    std::filesystem::path radar_log = root / "build" / "radar_arquivos.log";
    // Fase 28: o dump de frame (ferramenta de depuracao da Fase 10) passa a ser opt-in. Ligado, ele
    // grava um arquivo a cada tela "parada" e prende a thread de render por 0,3 a 1,2 s cada vez.
    std::filesystem::path frame_dump_dir;
    bool window = true;
    bool prewarm_only = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max-calls") && i + 1 < argc) {
            max_lines = std::strtoull(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_s = std::strtoul(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--app0") && i + 1 < argc) {
            app0 = WideArg(i + 1, argv[i + 1]), ++i; // Fase 60: caminho lido da linha de comando Unicode
        } else if (!std::strcmp(argv[i], "--patch") && i + 1 < argc) {
            app0_patch = WideArg(i + 1, argv[i + 1]), ++i; // Fase 58: atualizacao em pasta separada (sem a opcao = procurar; "-" = nenhuma)
        } else if (!std::strcmp(argv[i], "--mods-cache") && i + 1 < argc) {
            mods_cache = WideArg(i + 1, argv[i + 1]), ++i; // Fase 64: saida do tools/setup/mods.py (padrao: assets\mods_cache)
        } else if (!std::strcmp(argv[i], "--no-mods")) {
            mods_cache = "-"; // Fase 64: nenhum mod
        } else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) {
            ++i; // Fase 49: lido antes, em FindRoot
        } else if (!std::strcmp(argv[i], "--verbose-hle")) {
            verbose = true; // loga tambem os overrides de alta frequencia (memcpy, mutex lock...)
        } else if (!std::strcmp(argv[i], "--input-script") && i + 1 < argc) {
            // "seg:botao[:ms],..." ex.: "25:options,30:cross:500" - aperta botoes sem teclado
            if (!hle::SetPadInputScript(argv[++i])) {
                std::printf("ERROR: invalid --input-script: %s\n", argv[i]);
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--portal") && i + 1 < argc) {
            portal_file = argv[++i];
        } else if (!std::strcmp(argv[i], "--radar-log") && i + 1 < argc) {
            radar_log = argv[++i];
        } else if (!std::strcmp(argv[i], "--frame-dump") && i + 1 < argc) {
            frame_dump_dir = argv[++i]; // pasta dos dumps de tela parada
        } else if (!std::strcmp(argv[i], "--no-frame-dump")) {
            frame_dump_dir.clear();
        } else if (!std::strcmp(argv[i], "--shader-dump") && i + 1 < argc) {
            shader_dump_dir = argv[++i]; // pasta do extrator de shaders (padrao assets/dumped_shaders)
        } else if (!std::strcmp(argv[i], "--no-shader-dump")) {
            shader_dump_dir.clear();
        } else if (!std::strcmp(argv[i], "--frame-dump-at") && i + 1 < argc) {
            // lista de horarios em segundos, separados por virgula: dump do frame que estiver na tela
            for (const char* text = argv[++i]; *text;) {
                char* end = nullptr;
                const unsigned long seconds = std::strtoul(text, &end, 10);
                if (end == text) {
                    break;
                }
                hle::gnm::AddFrameDumpAt(static_cast<u32>(seconds));
                text = *end == ',' ? end + 1 : end;
            }
        } else if (!std::strcmp(argv[i], "--load-game-macro")) {
            hle::SetLoadGameMacro(true); // titulo -> OPTIONS; menu principal -> BAIXO, X, X, X, X
            hle::SetSkipIntroSpam(true); // e, antes do titulo, X / O / triangulo para tentar pular a abertura
        } else if (!std::strcmp(argv[i], "--no-skip-spam")) {
            hle::SetSkipIntroSpam(false); // controle: macro sem a tentativa de pular a abertura
        } else if (!std::strcmp(argv[i], "--no-vulkan-draws")) {
            hle::gnm::SetVulkanDraws(false); // Vulkan so limpa a tela, sem a geometria do jogo
        } else if (!std::strcmp(argv[i], "--no-srgb-display")) {
            hle::gnm::SetSrgbDisplay(false); // (padrao)
        } else if (!std::strcmp(argv[i], "--srgb-display")) {
            hle::gnm::SetSrgbDisplay(true); // experimento: buffer de video como alvo sRGB (gama a mais)
        } else if (!std::strcmp(argv[i], "--player-name") && i + 2 < argc) {
            // Fase 65: --player-name N APELIDO (N = 1..4): o nome que o jogo mostra no lugar de PlayerN
            const int player = std::atoi(argv[i + 1]);
            hle::SetPlayerName(player - 1, hle::fs::PathText(WideArg(i + 2, argv[i + 2])));
            i += 2;
        } else if (!std::strcmp(argv[i], "--stats-overlay")) {
            hle::videoout::SetStatsOverlay(true); // Fase 65: contador de FPS / info (F10 alterna no jogo)
        } else if (!std::strcmp(argv[i], "--lang") && i + 1 < argc) {
            // Fase 60/61: idioma dos textos do port e do jogo (pt / en / es / fr / de / it / nl / da); sem a opcao, o do Windows
            lang::SetFromCode(argv[++i]);
        } else if (!std::strcmp(argv[i], "--background-pad")) {
            hle::SetPadBackgroundInput(true); // Fase 60: o controle vale mesmo com a janela fora de foco
        } else if (!std::strcmp(argv[i], "--skip-cutscenes")) {
            g_skip_cutscenes = true; // Fase 61: toda cutscene pode ser pulada, mesmo sem 100%
        } else if (!std::strcmp(argv[i], "--resolution") && i + 1 < argc) {
            unsigned width = 0, height = 0; // Fase 61: tamanho da janela, ex. 1920x1080
            if (std::sscanf(argv[++i], "%ux%u", &width, &height) == 2 && width && height) {
                hle::videoout::SetWindowSize(width, height);
            }
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            hle::videoout::SetStartFullscreen(true); // Fase 60: tela cheia sem bordas (F11 alterna)
        } else if (!std::strcmp(argv[i], "--fps60")) {
            hle::videoout::SetForce60Hz(true);
            hle::videoout::SetUnlockedFps(false);
        } else if (!std::strcmp(argv[i], "--fps30")) {
            hle::videoout::SetForce60Hz(false);
            hle::videoout::SetUnlockedFps(false);
        } else if (!std::strcmp(argv[i], "--fps-unlocked") || !std::strcmp(argv[i], "--fps-unlimited")) {
            hle::videoout::SetForce60Hz(true);
            hle::videoout::SetUnlockedFps(true);
        } else if (!std::strcmp(argv[i], "--no-async-pipelines")) {
            render::VulkanRenderer::Instance().SetAsyncPipelines(false); // pipelines criados na thread de render
        } else if (!std::strcmp(argv[i], "--check-modules")) {
            hle::videoout::SetCheckModules(true); // confere todos os .spv soltos na abertura (diagnostico lento)
        } else if (!std::strcmp(argv[i], "--button-icons") && i + 1 < argc) {
            hle::gnm::SetButtonIcons(argv[++i]); // Fase 62: auto (pelo controle) | ps4 | ps3 | 360 | xboxone | wiiu
        } else if (!std::strcmp(argv[i], "--geometry-budget") && i + 1 < argc) {
            hle::gnm::SetGeometryBudget(std::atof(argv[++i])); // Fase 61: ms por frame para promover / conferir geometria (0 = sem limite)
        } else if (!std::strcmp(argv[i], "--pair-budget") && i + 1 < argc) {
            hle::gnm::SetPairBudget(std::atof(argv[++i])); // Fase 41: ms por frame para carregar pares (e compilar as bibliotecas) na hora
        } else if (!std::strcmp(argv[i], "--no-preload-aot")) {
            hle::gnm::SetPreloadAllAot(false); // Fase 43: so os shaders ja usados na abertura (o pacote fica para a hora)
        } else if (!std::strcmp(argv[i], "--no-loading-screen")) {
            loading_screen = false; // Fase 42: o jogo comeca direto (o aquecimento continua em segundo plano)
        } else if (!std::strcmp(argv[i], "--gpl")) {
            render::VulkanRenderer::Instance().SetGpl(true); // Fase 41: bibliotecas de pipeline (experimental)
        } else if (!std::strcmp(argv[i], "--no-gpl")) {
            render::VulkanRenderer::Instance().SetGpl(false); // Fase 41: pipelines monoliticos (sem bibliotecas de pipeline)
        } else if (!std::strcmp(argv[i], "--no-lazy-aot")) { // Fase 59: modulos do pacote AOT inteiro criados na abertura
            hle::gnm::SetLazyAot(false);
        } else if (!std::strcmp(argv[i], "--no-stall-stacks")) { // Fase 59: sem o retrato das threads nas travadas
            hle::gnm::SetStallStacks(false);
        } else if (!std::strcmp(argv[i], "--no-parallel-textures")) { // Fase 59: volta ao destile em serie na thread de render
            hle::gnm::SetParallelTextures(false);
        } else if (!std::strcmp(argv[i], "--no-texture-wait")) {
            texture_wait = false; // Fase 45: draw com textura pendente fica de fora do frame (como antes)
        } else if (!std::strcmp(argv[i], "--no-sync-textures")) {
            sync_textures = false; // Fase 39: texturas novas so pela thread de texturas (draw de fora ate chegarem)
        } else if (!std::strcmp(argv[i], "--texture-spike-budget") && i + 1 < argc) {
            texture_spike_budget_ms = std::atof(argv[++i]); // Fase 40: orcamento num frame com pico de texturas novas
        } else if (!std::strcmp(argv[i], "--texture-spike-count") && i + 1 < argc) {
            texture_spike_count = static_cast<u32>(std::atoi(argv[++i])); // Fase 40: texturas novas no frame que contam como pico
        } else if (!std::strcmp(argv[i], "--texture-budget") && i + 1 < argc) {
            texture_budget_ms = std::atof(argv[++i]); // ms por frame para ler / destilar texturas na hora
        } else if (!std::strcmp(argv[i], "--no-sync-shaders")) {
            sync_shaders = false; // Fase 38: pares e pipelines novos so em segundo plano (draw de fora ate chegarem)
        } else if (!std::strcmp(argv[i], "--sync-budget") && i + 1 < argc) {
            sync_budget_ms = std::atof(argv[++i]); // ms por frame para criar pipelines na hora
        } else if (!std::strcmp(argv[i], "--pipeline-burst-wait") && i + 1 < argc) {
            render::VulkanRenderer::Instance().SetPipelineBurstWait(std::atof(argv[++i])); // Fase 55: 0 desliga
        } else if (!std::strcmp(argv[i], "--live-sync-budget") && i + 1 < argc) {
            hle::gnm::SetLiveSyncBudget(std::atof(argv[++i])); // Fase 56: ms por frame para traduzir par novo na hora (0 = segundo plano)
        } else if (!std::strcmp(argv[i], "--no-fast-pipelines")) {
            render::VulkanRenderer::Instance().SetFastPipelines(false); // Fase 56: so a versao otimizada
        } else if (!std::strcmp(argv[i], "--no-aot")) {
            hle::gnm::SetAot(false, false); // nao usa o pacote de pares compilados dos .DAT (assets/shaders_aot)
        } else if (!std::strcmp(argv[i], "--aot-first")) {
            hle::gnm::SetAot(true, true); // diagnostico: pares do pacote AOT mesmo quando o compilado com o jogo existe
        } else if (!std::strcmp(argv[i], "--prewarm-only")) {
            prewarm_only = true;
        } else if (!std::strcmp(argv[i], "--no-warmup")) {
            shader_warmup = false; // nao carrega na abertura os pares e pipelines ja vistos
        } else if (!std::strcmp(argv[i], "--no-async-textures")) {
            hle::gnm::SetAsyncTextures(false); // texturas lidas e destiladas na thread de render
        } else if (!std::strcmp(argv[i], "--parallel-dma")) {
            hle::gnm::SetParallelDma(true); // experimental: copias do DMA_DATA em lote, por varias threads
        } else if (!std::strcmp(argv[i], "--no-async-pairs")) {
            hle::gnm::SetAsyncPairs(false); // pares carregados na thread de render, como ate a Fase 28
        } else if (!std::strcmp(argv[i], "--no-live-compiler")) {
            live_compiler = false; // pares novos nao sao recompilados durante o jogo
        } else if (!std::strcmp(argv[i], "--no-texture-refresh")) {
            hle::gnm::SetTextureRefresh(false); // texturas grandes enviadas uma unica vez
        } else if (!std::strcmp(argv[i], "--hitch-log")) {
            hle::gnm::SetHitchLog(true); // Fase 28: uma linha [ENGASGO] por frame lento, com o motivo
        } else if (!std::strcmp(argv[i], "--no-readback")) {
            hle::gnm::SetReadback(false); // nao devolve o alvo de luminancia para a memoria do jogo
        } else if (!std::strcmp(argv[i], "--no-geometry-cache")) {
            hle::gnm::SetGeometryCache(false); // toda a geometria copiada da memoria do jogo a cada draw
        } else if (!std::strcmp(argv[i], "--target-log")) {
            hle::gnm::SetTargetLog(true); // diagnostico: alvos de render, viewports e alvos usados como textura
        } else if (!std::strcmp(argv[i], "--no-game-shaders")) {
            hle::gnm::SetGameShaders(false); // interface com os dois shaders base, como na Fase 18
        } else if (!std::strcmp(argv[i], "--vulkan-validation")) {
            render::SetVulkanValidation(true); // liga a camada de validacao; um erro encerra o processo
        } else if (!std::strcmp(argv[i], "--no-vulkan-validation")) {
            render::SetVulkanValidation(false);
        } else if (!std::strcmp(argv[i], "--vulkan-validation-continue")) {
            render::SetVulkanValidation(true);
            render::SetVulkanValidationAbort(false); // erros de validacao so sao registrados
        } else if (!std::strcmp(argv[i], "--no-vulkan")) {
            render::SetVulkanEnabled(false); // janela preta com texto por GDI, como ate a Fase 15
        } else if (!std::strcmp(argv[i], "--profile-at") && i + 1 < argc) {
            // lista de horarios em segundos: perfil de CPU por amostragem (4 s) naquele momento
            for (const char* text = argv[++i]; *text;) {
                char* end = nullptr;
                const unsigned long seconds = std::strtoul(text, &end, 10);
                if (end == text) {
                    break;
                }
                hle::gnm::AddProfileAt(static_cast<u32>(seconds));
                text = *end == ',' ? end + 1 : end;
            }
        } else if (!std::strcmp(argv[i], "--cutscene-watchdog") && i + 1 < argc) {
            cutscene_watchdog_s = std::strtoul(argv[++i], nullptr, 0); // 0 desliga
        } else if (!std::strcmp(argv[i], "--ngs2-static")) {
            hle::SetNgs2VoiceClock(false); // controle A/B: estado de voz zerado, como ate a Fase 9
        } else if (!std::strcmp(argv[i], "--no-voice-log")) {
            hle::SetNgs2VoiceLog(false);
        } else if (!std::strcmp(argv[i], "--test-save-ui")) {
            test_save_ui = true;
        } else if (!std::strcmp(argv[i], "--addcont") && i + 1 < argc) {
            addcont_dir = WideArg(i + 1, argv[i + 1]), ++i; // Fase 46: pasta com uma subpasta por DLC (sce_sys/param.sfo)
            addcont_set = true;
        } else if (!std::strcmp(argv[i], "--no-dlc")) {
            addcont_dir.clear();
            addcont_set = true;
        } else if (!std::strcmp(argv[i], "--saves-dir") && i + 1 < argc) {
            hle::SetSavesDirName(argv[++i]); // pasta dos saves dentro de assets (padrao savedata0)
        } else if (!std::strcmp(argv[i], "--no-save-ui")) {
            hle::SetSaveUi(false); // a lista de saves do sistema responde OK sozinha (sempre o primeiro slot)
        } else if (!std::strcmp(argv[i], "--audio-dump") && i + 1 < argc) {
            hle::SetAudioDump(argv[++i]); // grava em WAV o que vai para o dispositivo de som
        } else if (!std::strcmp(argv[i], "--no-audio")) {
            hle::SetAudioOutput(false); // sem mixagem nem dispositivo de som (como antes da Fase 24)
        } else if (!std::strcmp(argv[i], "--no-sdl-pad")) {
            hle::padsdl::SetEnabled(false); // so XInput / teclado, como antes da Fase 24.5
        } else if (!std::strcmp(argv[i], "--stick-log")) {
            hle::SetPadStickLog(true); // imprime o analogico esquerdo quando sai da zona morta
        } else if (!std::strcmp(argv[i], "--no-keyboard")) {
            hle::SetPadKeyboardEnabled(false); // execucoes de teste: so XInput e --input-script
        } else if (!std::strcmp(argv[i], "--rand15")) {
            hle::SetRand15Bits(true); // diagnostico: rand() com a faixa de 15 bits da MSVCRT
        } else if (!std::strcmp(argv[i], "--no-window")) {
            window = false; // sceVideoOutOpen nao cria a janela Win32
        } else if (!std::strcmp(argv[i], "--toypad")) {
            toypad = true; // enumera um Toy Pad virtual (0e6f:0241) no libSceUsbd (ja e o padrao)
        } else if (!std::strcmp(argv[i], "--no-toypad")) {
            toypad = false;
        } else if (!std::strcmp(argv[i], "--no-real-toypad")) {
            real_toypad = false; // Fase 61: mesmo com o portal fisico ligado, usa o virtual (portal.json / app)
        } else if (!std::strcmp(argv[i], "--toypad-port") && i + 1 < argc) {
            toypad_port = std::atoi(argv[++i]); // porta do listener do app LegoToypad (0 = desligado)
        } else if (!std::strcmp(argv[i], "--selftest")) {
            selftest = true;
        } else {
            elf_path = argv[i];
        }
    }
    // LD_LOG_FILE=<arquivo>: a saida vai para esse arquivo (testes em que o jogo e aberto por outro programa, como
    // o RenderDoc, e o console nao pode ser redirecionado)
    if (const char* log_file = std::getenv("LD_LOG_FILE"); log_file && *log_file) {
        if (std::freopen(log_file, "w", stdout)) {
            std::freopen(log_file, "a", stderr);
        }
    }
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    // Resolucao de 1 ms no agendador: sem isto Sleep/sleep_until arredondam para ~15,6 ms e a
    // saida de audio (5,33 ms por buffer) e os timeouts curtos do jogo perdem o ritmo.
    timeBeginPeriod(1);
    hle::log::Init(max_lines, verbose);

    // ---- 0. overrides: precisam estar na tabela antes das relocacoes ------
    const hle::OverrideStats ov = hle::InstallOverrides();
    hle::InitDataObjects();
    hle::fs::Configure(app0, root / "assets");
    // Fase 58: atualizacao numa pasta separada (formato do shadPS4). Sem --patch, procura
    // <jogo>-patch ou CUSA01176-patch dentro da pasta do jogo e ao lado dela.
    if (app0_patch.empty()) {
        app0_patch = FindPatchDir(app0);
    } else if (app0_patch == "-") {
        app0_patch.clear();
    }
    hle::fs::SetApp0Patch(app0_patch);
    // Fase 64: mods de PS4 / shadPS4 (pacotes da TT Games e arquivos soltos), depois da atualizacao
    if (!selftest && mods_cache != "-") {
        if (const int mods = hle::mods::Setup(mods_cache / "mods.txt")) {
            std::printf("mods       : %d mod pack(s) / folder(s)  [%s]  (--no-mods disables)\n", mods, hle::fs::PathText(mods_cache).c_str());
        }
    }
    // Fase 46: DLC. Padrao: assets\addcont se existir; senao a pasta de addcont do shadPS4 para este
    // jogo (%APPDATA%\shadPS4\addcont\CUSA01176), lida no lugar (sem copiar os pacotes).
    if (!addcont_set) {
        std::error_code ec;
        if (std::filesystem::is_directory(root / "assets" / "addcont", ec)) {
            addcont_dir = root / "assets" / "addcont";
        } else if (const char* appdata = std::getenv("APPDATA")) {
            addcont_dir = game_region::Addcont(app0, appdata);
        }
    }
    hle::SetAddcontDirectory(selftest ? std::filesystem::path{} : addcont_dir);
    if (test_save_ui) {
        return hle::TestSaveUi(); // so a janela da lista de saves, sem carregar o jogo
    }
    if (!selftest) {
        hle::gnm::SetFrameDumpDirectory(frame_dump_dir);
        hle::gnm::SetShaderDumpDirectory(shader_dump_dir);
        render::SetGameSpirvDirectory((root / "assets" / "shaders_spv").string());
        render::VulkanRenderer::Instance().SetPipelineStateFile((root / "assets" / "shaders_spv" / "pipeline_states.txt").string());
        render::VulkanRenderer::Instance().SetPipelineCacheFile((root / "assets" / "shaders_spv" / "vk_pipeline_cache.bin").string());
        hle::gnm::SetSyncShaders(sync_shaders, sync_budget_ms);
        hle::gnm::SetSyncTextures(sync_textures, texture_budget_ms, texture_spike_budget_ms, texture_spike_count);
        hle::gnm::SetTextureWait(texture_wait);
        hle::gnm::PreloadAotIndex();
        hle::gnm::StartShaderWarmup(shader_warmup);
        if (live_compiler && !shader_dump_dir.empty()) {
            hle::gnm::SetLiveCompiler(root / "tools" / "shader_compiler_tool" / "build" / "shader_compiler_tool.exe", root, shader_dump_dir);
        }
        hle::gnm::SetCutsceneWatchdog(cutscene_watchdog_s);
        hle::gnm::StartPerfThread();
        hle::radar::Configure(root / "tools" / "data" / "dat_index", radar_log);
        hle::radar::SetLocalIndexDir(root / "assets" / "dat_index"); // Fase 64: pacotes do jogador e dos mods
        std::printf("radar      : %s  (every file read from .DAT archives)\n",
                    hle::fs::PathText(radar_log).c_str());
    }
    hle::SetVirtualToypadConnected(toypad);
    // Fase 61: com o Toy Pad fisico ligado, e ele que atende o jogo (o virtual, o portal.json e o app ficam de fora)
    if (toypad && !selftest && real_toypad && hle::UseRealToypad()) {
        std::printf("portal     : Physical Toy Pad (USB); --no-real-toypad uses virtual portal\n");
    } else if (toypad && !selftest) {
        hle::StartPortalWatcher(portal_file);
        std::printf("portal     : %s  (reread every 500 ms)\n", hle::fs::PathText(portal_file).c_str());
        hle::StartToypadListener(toypad_port);
    }
    hle::videoout::SetWindowEnabled(window && !selftest);
    hle::SetStubLogger(&hle::log::StubLogger);
    runtime::InstallCrashHandler();
    std::printf("Overrides  : %u actual implementations installed over generated table (%zu symbols)\n",
                ov.installed, hle::Symbols().size());
    {
        std::error_code ec;
        const bool has_eboot = std::filesystem::exists(app0 / "eboot.bin", ec);
        const bool has_data = std::filesystem::is_directory(app0 / "data", ec);
        std::printf("/app0      : %s  (eboot.bin: %s, data/: %s) - read only\n", hle::fs::PathText(app0).c_str(),
                    has_eboot ? "yes" : "NO", has_data ? "yes" : "NO");
        if (!app0_patch.empty()) {
            std::printf("update     : %s  (overlaying /app0)\n", hle::fs::PathText(app0_patch).c_str());
        }
        std::printf("/temp0     : %s\n", hle::fs::PathText((root / "assets" / "temp0")).c_str());
        std::printf("saves      : %s  (mounted at /savedataN by sceSaveDataMount)\n",
                    hle::fs::PathText((root / "assets" / "savedata0")).c_str());
    }
    runtime::RegisterCurrentThread("main");

    // ---- 1. carga ---------------------------------------------------------
    loader::ElfPieLoader elf;
    if (!elf.Load(elf_path)) {
        std::printf("ERROR loading %s: %s\n", hle::fs::PathText(elf_path).c_str(), elf.error().c_str());
        return 1;
    }
    runtime::SetGuestImage(elf.base_address(), elf.image_size(), loader::ElfPieLoader::kReserveSize);
    std::printf("ELF        : %s\n", hle::fs::PathText(elf_path).c_str());
    std::printf("Base       : 0x%" PRIx64 "  (reservation of %" PRIu64 " MiB, image of %.1f MiB)\n",
                elf.base_address(), loader::ElfPieLoader::kReserveSize >> 20,
                elf.image_size() / 1048576.0);

    // ---- 2. relocacoes ----------------------------------------------------
    elf.Relocate();
    const auto& rs = elf.reloc_stats();
    std::printf("Relocations: .rela.dyn=%" PRIu64 "  .rela.plt=%" PRIu64 "  unresolved=%" PRIu64
                "  unknown types=%zu\n",
                rs.rela_dyn, rs.rela_plt, rs.unresolved, rs.unknown_types.size());

    // ---- 3. TLS + patch de FS ---------------------------------------------
    runtime::OrbisTls tls;
    if (!tls.AllocateSlot() || !tls.SetupCurrentThread(elf.tls(), elf.base())) {
        std::printf("ERROR: could not initialize main thread TLS\n");
        return 1;
    }
    for (const auto& seg : elf.segments()) {
        if (!(seg.flags & loader::PF_X)) {
            continue;
        }
        const auto ps = patcher::PatchFsThreadPointer(elf.base() + seg.vaddr, seg.filesz, tls.gs_offset());
        std::printf("Patch FS   : %" PRIu64 " x 'mov r64, fs:[0]' -> gs:[0x%x]\n", ps.patched, tls.gs_offset());
        std::printf("             %" PRIu64 " x 'mov r64, fs:[disp != 0]' (absolute form, reliable match)\n",
                    ps.absolute_nonzero);
        for (const auto& [off, disp] : ps.absolute_nonzero_samples) {
            std::printf("               eboot+0x%" PRIx64 "  disp=0x%" PRIx64 "\n", seg.vaddr + off, (u64)disp);
        }
        std::printf("             %" PRIu64 " other candidates with FS prefix (includes .rodata and middle of "
                    "instruction; VEH reports any that execute)\n",
                    ps.other_forms);
    }

    if (g_skip_cutscenes) {
        std::printf("Cutscenes : %s\n", PatchCutsceneSkip(elf) ? "all can be skipped (--skip-cutscenes)"
                                                                       : "WARNING: different eboot, skip mod not applied");
    }
    if (!selftest) {
        hle::anomaly::Install(elf.base()); // Fase 67: carregador de mods Anomaly (connorh315), se houver pacotes de mod
    }
    if (!elf.Protect()) {
        std::printf("ERROR: %s\n", elf.error().c_str());
        return 1;
    }
    if (selftest) {
        // Depois da carga: os testes de thread precisam do TLS do eboot ja montado.
        hle::log::Init(100000, verbose);
        const int failures = SelfTest();
        std::fflush(stdout);
        return failures;
    }

    // ---- 4. tela de carregamento (Fase 42) --------------------------------
    // Abre a janela antes do jogo e espera o pacote AOT ser lido, os pares ja usados serem
    // carregados e os pipelines ja vistos serem criados; so entao o jogo comeca.
    if (prewarm_only) {
        std::printf("[PREWARM] Modo pre-aquecimento de pipelines solicitado (--prewarm-only)\n");
        if (hle::videoout::OpenWindowEarly(5000)) {
            render::VulkanRenderer::Instance().PrewarmPipelines();
            for (int i = 0; i < 400; ++i) {
                u32 done = 0, total = 0;
                render::VulkanRenderer::Instance().PrewarmProgress(&done, &total);
                if (total > 0 && done >= total) {
                    std::printf("[PREWARM] Concluidos todos os %u pipelines pre-aquecidos!\n", total);
                    break;
                }
                Sleep(50);
            }
            render::VulkanRenderer::Instance().SaveDiskPipelineCache();
            std::printf("[PREWARM] Cache salvo em disco com sucesso. Finalizando pre-aquecimento.\n");
        }
        return 0;
    }

    if (loading_screen && window && hle::videoout::OpenWindowEarly(15000)) {
        hle::videoout::RunLoadingScreen(600);
    }

    // ---- 5. entrada -------------------------------------------------------
    if (timeout_s) {
        CreateThread(nullptr, 0, WatchdogThread, reinterpret_cast<void*>(static_cast<u64>(timeout_s) * 1000), 0, nullptr);
    }

    static EntryParams params{};
    params.argc = 1;
    params.argv[0] = "eboot.bin";
    params.entry_addr = elf.entry();

    std::printf("Entry      : 0x%" PRIx64 " (base + 0x%" PRIx64 ")\n", elf.entry(), elf.entry_offset());
    std::printf("------------------------------------------------------------\n");
    std::fflush(stdout);
    JumpToEntry(elf.entry(), &params, &ProgramExit);
}
