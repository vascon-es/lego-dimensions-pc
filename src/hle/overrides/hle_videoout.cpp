// libSceVideoOut - os 8 simbolos importados - e a janela Win32.
//
// Call sites (FASE6_callsites_video_gnm.txt):
//   sceVideoOutOpen(0xFF, 0, 0, NULL) -> handle
//   sceVideoOutAddFlipEvent(eq, handle, NULL)
//   sceVideoOutSetBufferAttribute(attr, 0x80000000, 0, 0, largura, altura, pitch)
//   sceVideoOutRegisterBuffers(handle, 0, enderecos, quantidade, attr)
//   sceVideoOutSetFlipRate(handle, 0 ou 1), sceVideoOutSetWindowModeMargins(handle, 0x21C, 0x21C)
// O jogo nao le nenhum campo do SceKernelEvent depois de sceKernelWaitEqueue("eq to wait flip"):
// so precisa que UM evento chegue para liberar o buffer que marcou como pendente.
//
// Nada e desenhado: a janela mostra o estado e o titulo traz os contadores. Os flips sao
// concluidos por uma thread de VSync, que e quem dita o ritmo do jogo.
#include "hle/overrides/hle_videoout.h"
#include "render/vulkan_backend.h"
#include "runtime/lang.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

#include <intrin.h>

#include "hle/overrides/hle_equeue.h"
#include "hle/overrides/hle_filesystem.h"
#include "hle/overrides/hle_override.h"
#include "runtime/crash_handler.h"
#include "runtime/hwmon.h"

namespace hle::gnm {
void WarmupStatus(char* text, size_t size); // hle_gnm.cpp
}

namespace hle::videoout {

namespace {

constexpr int kHandle = 1;
constexpr u64 kEventFlip = 0x6; // SCE_VIDEO_OUT_INTERNAL_EVENT_FLIP
constexpr int kErrorInvalidHandle = static_cast<int>(0x8029000B);
constexpr int kErrorInvalidValue = static_cast<int>(0x80290001);
constexpr int kErrorInvalidEventQueue = static_cast<int>(0x8029000C);

std::atomic<bool> g_display_srgb{false}; // formato de pixel do ultimo sceVideoOutSetBufferAttribute

struct BufferAttribute {
    u32 pixel_format;
    u32 tiling_mode;
    u32 aspect_ratio;
    u32 width;
    u32 height;
    u32 pitch_in_pixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};
static_assert(sizeof(BufferAttribute) == 0x28);

struct PendingFlip {
    int buffer_index;
    int flip_mode;
    s64 flip_arg;
    u64 submit_time;
};

struct FlipListener {
    equeue::Handle queue;
    void* udata;
};

std::mutex g_mutex;
bool g_open = false;
bool g_window_enabled = false;
std::atomic<bool> g_window_started{false}; // Fase 42: aberta antes do jogo pela tela de carregamento
int g_flip_rate = 0; // 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz
std::atomic<bool> g_force_60hz{false}; // --fps60
u32 g_width = 1920, g_height = 1080;
std::vector<void*> g_buffers;
std::vector<FlipListener> g_listeners;
std::deque<PendingFlip> g_pending;
FlipStatus g_status{};
std::atomic<u64> g_flip_count{0};
std::atomic<HWND> g_hwnd{nullptr};
const auto g_epoch = std::chrono::steady_clock::now();

u64 NowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_epoch).count();
}

// ------------------------------- janela -------------------------------------
constexpr UINT_PTR kTitleTimer = 1;

// Fase 60: tela cheia sem bordas (janela WS_POPUP do tamanho do monitor). O swapchain acompanha o novo
// tamanho como num redimensionamento comum. F11 alterna; --fullscreen abre assim.
std::atomic<bool> g_start_fullscreen{false};
// Fase 61: tamanho da area util da janela (--resolution LxA). O jogo continua desenhando em 1920x1080 e a
// imagem e escalada para a janela, como no tamanho padrao.
std::atomic<u32> g_window_width{1280}, g_window_height{720};
bool g_fullscreen = false;
std::atomic<bool> g_stats_overlay{false}; // Fase 65: contador de FPS / info (--stats-overlay, F10)
WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};

void SetFullscreen(HWND hwnd, bool enabled) {
    if (enabled == g_fullscreen) {
        return;
    }
    if (enabled) {
        GetWindowPlacement(hwnd, &g_windowed_placement);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& r = monitor.rcMonitor;
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fullscreen = enabled;
    log::Tagged("JANELA", "%s", enabled ? "tela cheia" : "janela");
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_TIMER: {
        char title[224], warm[64];
        gnm::WarmupStatus(warm, sizeof(warm)); // Fase 36: "Compilando shaders: X / Y" enquanto aquece
        std::snprintf(title, sizeof(title),
                      lang::Pt() ? "[LEGO Dimensions PC] Frames (Flips): %" PRIu64 " | .DAT abertos: %" PRIu64 " | Threads: %d%s"
                                 : "[LEGO Dimensions PC] Frames (Flips): %" PRIu64 " | .DAT open: %" PRIu64 " | Threads: %d%s",
                      g_flip_count.load(), fs::DatFilesOpened(), runtime::AliveGuestThreads(), warm);
        SetWindowTextA(hwnd, title);
        if (!render::VulkanRenderer::Instance().Active()) {
            InvalidateRect(hwnd, nullptr, FALSE); // sem Vulkan: redesenha o texto por GDI
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (render::VulkanRenderer::Instance().Active()) {
            EndPaint(hwnd, &ps); // a area cliente pertence ao swapchain do Vulkan
            return 0;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        char text[160];
        std::snprintf(text, sizeof(text),
                      "Nada e desenhado ainda.  Enter=OPTIONS  Espaco=X  Esc=O  X=quadrado  Y=triangulo  "
                      "setas=D-pad  WASD=analogico   Flips: %" PRIu64, g_flip_count.load());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F11 && !(lparam & (1 << 30))) { // Fase 60: F11 alterna a tela cheia (sem repeticao)
            SetFullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        if (wparam == VK_F10 && !(lparam & (1 << 30))) { // Fase 65: F10 liga / desliga o contador de FPS / info
            SetStatsOverlay(!g_stats_overlay);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(lparam) == HTCLIENT) {
            SetCursor(nullptr); // sem seta do mouse por cima do jogo em tela cheia
            return TRUE;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_CLOSE:
        std::printf("\nJanela fechada pelo usuario.\n");
        log::Shutdown(0);
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

bool g_check_modules = false; // --check-modules

void WindowThread() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "LegoDimensionsPcWindow";
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); // Fase 50: icone do port (recurso 1)
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rect{0, 0, static_cast<LONG>(g_window_width.load()), static_cast<LONG>(g_window_height.load())};
    RECT work{};
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0) && work.right > work.left) {
        // Maior que a area de trabalho: encolhe mantendo a proporcao pedida
        const RECT frame = [] { RECT r{0, 0, 0, 0}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE); return r; }();
        const double room_w = (work.right - work.left) - (frame.right - frame.left);
        const double room_h = (work.bottom - work.top) - (frame.bottom - frame.top);
        const double scale = std::min({1.0, room_w / rect.right, room_h / rect.bottom});
        rect.right = static_cast<LONG>(rect.right * scale);
        rect.bottom = static_cast<LONG>(rect.bottom * scale);
    }
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "[LEGO Dimensions PC]", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("AVISO: nao foi possivel criar a janela (erro %lu)\n", GetLastError());
        return;
    }
    g_hwnd.store(hwnd);
    if (g_start_fullscreen.load()) {
        SetFullscreen(hwnd, true); // antes do Vulkan: o swapchain ja nasce do tamanho do monitor
    }
    // Backend grafico: superficie do Vulkan presa a esta janela. Se falhar, a janela continua
    // como antes (preta, com o texto por GDI).
    auto& renderer = render::VulkanRenderer::Instance();
    if (renderer.Initialize(hwnd)) {
        log::Tagged("VULKAN", "inicializado: %s", renderer.Describe().c_str());
        log::Tagged("VULKAN", "shaders do jogo: %s; validacao: %s",
                    renderer.GameShadersSupported() ? "dispositivo com os recursos que os modulos pedem (pares em shaders_spv/pairs)"
                                                    : "dispositivo SEM os recursos necessarios - so os shaders base",
                    renderer.ValidationActive() ? "VK_LAYER_KHRONOS_validation ligada" : "desligada");
        // Fase 38: so com --check-modules. A conferencia segura o lock do backend enquanto le do disco
        // e cria um modulo para cada .spv solto (5 a 15 s): a thread de render e as cargas de par
        // ficavam paradas esse tempo todo na abertura.
        if (g_check_modules && !render::GameSpirvDirectory().empty() && !renderer.ValidationActive()) {
            u32 total = 0;
            const u32 accepted = renderer.CheckGameShaderModules(render::GameSpirvDirectory(), &total);
            log::Tagged("VULKAN", "modulos SPIR-V do jogo em %s: %u de %u aceitos por vkCreateShaderModule",
                        render::GameSpirvDirectory().c_str(), accepted, total);
        }
    } else {
        log::Tagged("VULKAN", "NAO inicializado: %s", renderer.LastError().c_str());
    }
    SetTimer(hwnd, kTitleTimer, 1000, nullptr);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// ------------------------------- VSync --------------------------------------
// A cada periodo conclui no maximo um flip pendente: atualiza o status e dispara o evento.
void VsyncThread() {
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        int rate;
        {
            std::lock_guard lock(g_mutex);
            rate = g_force_60hz.load(std::memory_order_relaxed) ? 0 : g_flip_rate;
        }
        next += std::chrono::microseconds(16667) * (rate + 1);
        std::this_thread::sleep_until(next);

        std::vector<FlipListener> listeners;
        s64 event_data = 0;
        {
            std::lock_guard lock(g_mutex);
            if (g_pending.empty()) {
                continue;
            }
            const PendingFlip flip = g_pending.front();
            g_pending.pop_front();
            const u64 now = NowMicros();
            ++g_status.count;
            g_status.process_time = now;
            g_status.tsc = __rdtsc();
            g_status.submit_tsc = flip.submit_time;
            g_status.flip_arg = flip.flip_arg;
            g_status.current_buffer = flip.buffer_index;
            g_status.gc_queue_num = 0;
            g_status.flip_pending_num = static_cast<s32>(g_pending.size());
            g_flip_count.store(g_status.count);
            // SceVideoOutEventData: time:12 | count:4 | flipArg:48
            event_data = static_cast<s64>((now & 0xFFF) | ((g_status.count & 0xF) << 12) |
                                          (static_cast<u64>(flip.flip_arg) << 16));
            listeners = g_listeners;
        }
        for (const FlipListener& listener : listeners) {
            equeue::Trigger(listener.queue, kEventFlip, equeue::kFilterVideoOut, event_data);
        }
    }
}

HLE_IMPL(int, sceVideoOutOpen, int user_id, int bus_type, int index, const void* param) {
    bool start_window = false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_open) {
            g_open = true;
            start_window = g_window_enabled && !g_window_started.exchange(true); // Fase 42: pode ja ter sido aberta
            std::thread(VsyncThread).detach();
        }
    }
    if (start_window) {
        std::thread(WindowThread).detach();
    }
    HLE_TRACE("user=%d bus=%d indice=%d -> handle %d%s", user_id, bus_type, index, kHandle,
              start_window ? " (janela criada)" : "");
    return kHandle;
}
HLE_OVERRIDE(sceVideoOutOpen);

HLE_IMPL(int, sceVideoOutAddFlipEvent, equeue::Handle queue, int handle, void* udata) {
    HLE_TRACE("handle=%d udata=%p", handle, udata);
    if (handle != kHandle) {
        return kErrorInvalidHandle;
    }
    if (!equeue::Register(queue, kEventFlip, equeue::kFilterVideoOut, udata)) {
        return kErrorInvalidEventQueue;
    }
    std::lock_guard lock(g_mutex);
    g_listeners.push_back({queue, udata});
    return err::OK;
}
HLE_OVERRIDE(sceVideoOutAddFlipEvent);

// O setimo argumento (pitch) chega pela pilha.
HLE_IMPL(void, sceVideoOutSetBufferAttribute, BufferAttribute* attribute, u32 pixel_format, u32 tiling_mode,
         u32 aspect_ratio, u32 width, u32 height, u32 pitch_in_pixel) {
    HLE_TRACE("formato=0x%x tiling=%u %ux%u pitch=%u", pixel_format, tiling_mode, width, height, pitch_in_pixel);
    if (!attribute) {
        return;
    }
    std::memset(attribute, 0, sizeof(*attribute));
    attribute->pixel_format = pixel_format;
    g_display_srgb = pixel_format == 0x80000000u || pixel_format == 0x80002200u;
    attribute->tiling_mode = tiling_mode;
    attribute->aspect_ratio = aspect_ratio;
    attribute->width = width;
    attribute->height = height;
    attribute->pitch_in_pixel = pitch_in_pixel;
}
HLE_OVERRIDE(sceVideoOutSetBufferAttribute);

HLE_IMPL(int, sceVideoOutRegisterBuffers, int handle, int start_index, void* const* addresses, int buffer_count,
         const BufferAttribute* attribute) {
    int result = 0; // indice do conjunto de buffers
    if (handle != kHandle) {
        result = kErrorInvalidHandle;
    } else if (!addresses || !attribute || buffer_count < 1 || buffer_count > 16 || start_index < 0) {
        result = kErrorInvalidValue;
    } else {
        std::lock_guard lock(g_mutex);
        g_width = attribute->width;
        g_height = attribute->height;
        g_buffers.resize(start_index + buffer_count);
        for (int i = 0; i < buffer_count; ++i) {
            g_buffers[start_index + i] = addresses[i];
        }
    }
    HLE_TRACE("handle=%d inicio=%d buffers=%d %ux%u primeiro=%p -> %d", handle, start_index, buffer_count,
              attribute ? attribute->width : 0, attribute ? attribute->height : 0,
              (addresses && buffer_count > 0) ? addresses[0] : nullptr, result);
    return result;
}
HLE_OVERRIDE(sceVideoOutRegisterBuffers);

HLE_IMPL(int, sceVideoOutSetFlipRate, int handle, int rate) {
    HLE_QUIET("handle=%d rate=%d", handle, rate);
    if (handle != kHandle) {
        return kErrorInvalidHandle;
    }
    if (rate < 0 || rate > 2) {
        return kErrorInvalidValue;
    }
    std::lock_guard lock(g_mutex);
    g_flip_rate = rate;
    return err::OK;
}
HLE_OVERRIDE(sceVideoOutSetFlipRate);

HLE_IMPL(int, sceVideoOutSetWindowModeMargins, int handle, int top, int bottom) {
    HLE_QUIET("handle=%d top=%d bottom=%d", handle, top, bottom);
    return handle == kHandle ? err::OK : kErrorInvalidHandle;
}
HLE_OVERRIDE(sceVideoOutSetWindowModeMargins);

// Ajustes de cor/gama da saida de video: aceitos e ignorados.
HLE_IMPL(int, sceVideoOutColorSettingsSetGamma_, void* settings, float gamma) {
    HLE_TRACE("gama=%g", static_cast<double>(gamma));
    return err::OK;
}
HLE_OVERRIDE(sceVideoOutColorSettingsSetGamma_);

HLE_IMPL(int, sceVideoOutAdjustColor_, int handle, const void* settings) {
    HLE_TRACE("handle=%d", handle);
    return handle == kHandle ? err::OK : kErrorInvalidHandle;
}
HLE_OVERRIDE(sceVideoOutAdjustColor_);

} // namespace

void SetCheckModules(bool enabled) {
    g_check_modules = enabled;
}

void SetStartFullscreen(bool enabled) {
    g_start_fullscreen = enabled;
}

void SetWindowSize(u32 width, u32 height) {
    g_window_width = std::clamp(width, 320u, 7680u);
    g_window_height = std::clamp(height, 180u, 4320u);
}

void SetForce60Hz(bool enabled) {
    g_force_60hz = enabled;
}

bool IsSrgbDisplayBuffer(u64 address) {
    if (!address || !g_display_srgb.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    for (const auto& buffer : g_buffers) {
        if (reinterpret_cast<u64>(buffer) == address) {
            return true;
        }
    }
    return false;
}

// ---------------- Fase 42: tela de carregamento ----------------
namespace {
// Fonte 5x7 (uma linha por byte, bit 4 = coluna da esquerda).
const u8* Glyph(char c) {
    static const u8 kLetters[26][7] = {
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
        {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}};
    static const u8 kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
    static const u8 kSlash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}, kPercent[7] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
                    kColon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, kDot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
                    kDash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, kSpace[7] = {},
                    kDegree[7] = {0x0C, 0x12, 0x12, 0x0C, 0x00, 0x00, 0x00}; // Fase 65: '^' = grau
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    switch (c) {
    case '/': return kSlash;
    case '%': return kPercent;
    case ':': return kColon;
    case '.': return kDot;
    case '-': return kDash;
    case '^': return kDegree;
    default: return kSpace;
    }
}

struct LoadingVertex {
    float x, y, z;
    u32 rgba; // R no byte baixo
};

// Coordenadas numa tela virtual de 1280x720 (origem no canto de cima, a esquerda).
void Quad(std::vector<LoadingVertex>& out, float x0, float y0, float x1, float y1, u32 rgba) {
    auto v = [&](float x, float y) { out.push_back({x / 640.0f - 1.0f, 1.0f - y / 360.0f, 0.0f, rgba}); };
    v(x0, y0), v(x1, y0), v(x1, y1);
    v(x0, y0), v(x1, y1), v(x0, y1);
}

float TextWidth(const std::string& text, float pixel) {
    return text.size() * 6.0f * pixel - pixel;
}

void Text(std::vector<LoadingVertex>& out, const std::string& text, float x, float y, float pixel, u32 rgba) {
    for (const char c : text) {
        const u8* rows = Glyph(c);
        for (int r = 0; r < 7; ++r) {
            for (int col = 0; col < 5; ++col) {
                if (rows[r] & (0x10 >> col)) {
                    Quad(out, x + col * pixel, y + r * pixel, x + (col + 1) * pixel, y + (r + 1) * pixel, rgba);
                }
            }
        }
        x += 6.0f * pixel;
    }
}

void DrawLoading(const std::vector<LoadingVertex>& vertices) {
    auto& renderer = render::VulkanRenderer::Instance();
    if (!vertices.empty()) {
        render::DrawCall draw;
        draw.vertex_data = vertices.data();
        draw.vertex_count = static_cast<u32>(vertices.size());
        draw.stride = sizeof(LoadingVertex);
        draw.position_offset = 0;
        draw.position_format = render::DrawCall::kFloat3;
        draw.has_color = true;
        draw.color_offset = 12;
        draw.primitive = render::DrawCall::kTriangleList;
        draw.blend = false;
        draw.uv_format = render::DrawCall::kUvNone;
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(draw.mvp, kIdentity, sizeof(kIdentity));
        draw.viewport[0] = draw.viewport[1] = 0.0f;
        draw.viewport[2] = 1280.0f;
        draw.viewport[3] = 720.0f;
        draw.target_size[0] = 1280.0f;
        draw.target_size[1] = 720.0f;
        renderer.QueueDraw(draw);
    }
    renderer.DrawFrame(0);
}

// ---------------- Fase 65: contador de FPS / info (canto de cima, a esquerda) ----------------

struct OverlayCanvas {
    std::vector<LoadingVertex>& out;
    float w, h; // tamanho da janela em pixels

    void Rect(float x0, float y0, float x1, float y1, u32 rgba) {
        auto v = [&](float x, float y) { out.push_back({x / (w * 0.5f) - 1.0f, 1.0f - y / (h * 0.5f), 0.0f, rgba}); };
        v(x0, y0), v(x1, y0), v(x1, y1);
        v(x0, y0), v(x1, y1), v(x0, y1);
    }
    // pixels acesos seguidos na mesma linha viram um retangulo so
    void Glyphs(const std::string& text, float x, float y, float pixel, u32 rgba) {
        for (const char c : text) {
            const u8* rows = Glyph(c);
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5;) {
                    if (!(rows[r] & (0x10 >> col))) {
                        ++col;
                        continue;
                    }
                    int end = col;
                    while (end < 5 && (rows[r] & (0x10 >> end))) {
                        ++end;
                    }
                    Rect(x + col * pixel, y + r * pixel, x + end * pixel, y + (r + 1) * pixel, rgba);
                    col = end;
                }
            }
            x += 6.0f * pixel;
        }
    }
    // texto com sombra escura (le em cima de qualquer cena); devolve onde termina
    float Write(const std::string& text, float x, float y, float pixel, u32 rgba) {
        const float shadow = std::max(1.0f, pixel * 0.5f);
        Glyphs(text, x + shadow, y + shadow, pixel, 0xFF000000);
        Glyphs(text, x, y, pixel, rgba);
        return x + TextWidth(text, pixel);
    }
    // valor alinhado a direita em `right` + unidade menor logo depois
    void Value(const std::string& value, const char* unit, float right, float y, float pixel) {
        constexpr u32 kOrange = 0xFF008CFF, kWhite = 0xFFFFFFFF;
        Write(value, right - TextWidth(value, pixel), y, pixel, kOrange);
        if (unit && *unit) {
            Write(unit, right + pixel * 1.5f, y, pixel * 0.75f, kWhite);
        }
    }
};
} // namespace

void SetStatsOverlay(bool enabled) {
    g_stats_overlay = enabled;
    if (enabled) {
        runtime::hwmon::Start();
    }
}

void QueueStatsOverlay() {
    constexpr int kFrames = 150;
    static float frame_ms[kFrames] = {};
    static int head = 0;
    static auto last = std::chrono::steady_clock::now();
    static auto window_start = last;
    static int window_frames = 0;
    static float fps = 0.0f;
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last).count();
    last = now;
    frame_ms[head] = ms;
    head = (head + 1) % kFrames;
    ++window_frames;
    if (const double elapsed = std::chrono::duration<double>(now - window_start).count(); elapsed >= 0.5) {
        fps = static_cast<float>(window_frames / elapsed);
        window_frames = 0;
        window_start = now;
    }
    HWND hwnd = static_cast<HWND>(WindowHandle());
    RECT client{};
    if (!g_stats_overlay || !hwnd || !GetClientRect(hwnd, &client) || client.right < 64 || client.bottom < 64) {
        return;
    }
    const runtime::hwmon::Snapshot s = runtime::hwmon::Get();
    static std::vector<LoadingVertex> vertices;
    vertices.clear();
    OverlayCanvas c{vertices, static_cast<float>(client.right), static_cast<float>(client.bottom)};
    const float p = client.bottom >= 1400 ? 3.0f : 2.0f; // tamanho do pixel da fonte
    constexpr u32 kGreen = 0xFF00C800, kBlue = 0xFFFF9638, kPink = 0xFFAA96FF, kGray = 0xFFD0D0D0;
    const float x = 3 * p, c1 = 52 * p, c2 = 84 * p, c3 = 126 * p, c4 = 174 * p;
    float y = 3 * p;
    const float row = 10 * p;
    char v[32];
    auto num = [&](int value) {
        std::snprintf(v, sizeof(v), "%d", value);
        return std::string(v);
    };
    if (!s.gpu_name.empty()) {
        c.Write("GPU", x, y, p, kGreen);
        if (s.gpu_temp >= 0) c.Value(num(s.gpu_temp), "^C", c1, y, p);
        if (s.gpu_load >= 0) c.Value(num(s.gpu_load), "%", c2, y, p);
        if (s.gpu_clock >= 0) c.Value(num(s.gpu_clock), "MHZ", c3, y, p);
        if (s.gpu_power >= 0) {
            std::snprintf(v, sizeof(v), "%.1f", s.gpu_power);
            c.Value(v, "W", c4, y, p);
        }
        y += row;
        if (s.vrm_temp >= 0 || s.hotspot >= 0) {
            c.Write(s.vrm_temp >= 0 ? "VRM" : "HOT", x, y, p, kGreen);
            c.Value(num(s.vrm_temp >= 0 ? s.vrm_temp : s.hotspot), "^C", c1, y, p);
            y += row;
        }
        if (s.vram_mb >= 0 || s.mem_clock >= 0) {
            c.Write("MEM", x, y, p, kGreen);
            if (s.vram_mb >= 0) c.Value(num(s.vram_mb), "MB", c1, y, p);
            if (s.mem_clock >= 0) c.Value(num(s.mem_clock), "MHZ", c3, y, p);
            y += row;
        }
    }
    if (s.cpu_load >= 0) {
        c.Write("CPU", x, y, p, kBlue);
        c.Value(num(s.cpu_load), "%", c2, y, p);
        y += row;
    }
    if (s.ram_mb >= 0) {
        c.Write("RAM", x, y, p, kBlue);
        c.Value(num(s.ram_mb), "MB", c1, y, p);
        y += row;
    }
    c.Write("VULKAN", x, y, p, kPink);
    c.Value(num(static_cast<int>(fps + 0.5f)), "FPS", c1 + 10 * p, y, p);
    std::snprintf(v, sizeof(v), "%.1f", ms);
    c.Value(v, "MS", c3 - 8 * p, y, p);
    y += row;
    // grafico do tempo de cada quadro (escala 0..50 ms), como o do Afterburner
    c.Write("FRAMETIME", x, y, p * 0.75f, kPink);
    c.Write(v, c4 - TextWidth(v, p * 0.75f), y, p * 0.75f, kPink);
    const float gx0 = x, gx1 = c4 + 10 * p, gy0 = y + 7 * p, gy1 = y + 22 * p;
    c.Rect(gx0, gy1, gx1, gy1 + std::max(1.0f, p * 0.5f), 0xFF505050); // linha de base
    for (int i = 0; i < kFrames; ++i) {
        const float t = std::clamp(frame_ms[(head + i) % kFrames], 0.0f, 50.0f) / 50.0f;
        const float px = gx0 + (gx1 - gx0) * i / kFrames;
        const float py = gy1 - (gy1 - gy0) * t;
        c.Rect(px, py - p * 0.5f, px + (gx1 - gx0) / kFrames + 0.5f, py + p * 0.5f, kGray);
    }
    render::DrawCall draw;
    draw.vertex_data = vertices.data();
    draw.vertex_count = static_cast<u32>(vertices.size());
    draw.stride = sizeof(LoadingVertex);
    draw.position_offset = 0;
    draw.position_format = render::DrawCall::kFloat3;
    draw.has_color = true;
    draw.color_offset = 12;
    draw.primitive = render::DrawCall::kTriangleList;
    draw.blend = false;
    draw.uv_format = render::DrawCall::kUvNone;
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(draw.mvp, kIdentity, sizeof(kIdentity));
    draw.viewport[0] = draw.viewport[1] = 0.0f;
    draw.viewport[2] = draw.target_size[0] = c.w;
    draw.viewport[3] = draw.target_size[1] = c.h;
    render::VulkanRenderer::Instance().QueueDraw(draw);
}

bool OpenWindowEarly(u32 timeout_ms) {
    if (!g_window_enabled) {
        return false;
    }
    if (!g_window_started.exchange(true)) {
        std::thread(WindowThread).detach();
    }
    auto& renderer = render::VulkanRenderer::Instance();
    for (u32 waited = 0; waited < timeout_ms && !renderer.Active(); waited += 20) {
        Sleep(20);
    }
    return renderer.Active();
}

void RunLoadingScreen(u32 max_seconds) {
    constexpr u32 kYellow = 0xFF00D5FF, kWhite = 0xFFFFFFFF, kGray = 0xFF808080, kDark = 0xFF303030, kGreen = 0xFF60D060;
    const auto start = std::chrono::steady_clock::now();
    std::vector<LoadingVertex> vertices;
    char line[160];
    for (;;) {
        gnm::LoadingStatus st;
        gnm::GetLoadingStatus(&st);
        const bool pipelines_finished = st.pairs_finished && st.pipelines_done >= st.pipelines_total;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const bool done = st.pack_finished && st.pairs_finished && pipelines_finished && st.aot_finished;
        // progresso total: pacote 10%, pares ja usados 10%, pipelines 10%, pacote inteiro 70%
        const double pack = st.pack_finished ? 1.0 : st.pack_total ? static_cast<double>(st.pack_done) / st.pack_total : 0.0;
        const double pairs = st.pairs_finished ? 1.0 : st.pairs_total ? static_cast<double>(st.pairs_done) / st.pairs_total : 0.0;
        const double pipes = pipelines_finished ? 1.0 : st.pipelines_total ? static_cast<double>(st.pipelines_done) / st.pipelines_total : 0.0;
        const double aot = st.aot_finished ? 1.0 : st.aot_total ? static_cast<double>(st.aot_done) / st.aot_total : 0.0;
        const double total = std::clamp(0.1 * pack + 0.1 * pairs + 0.1 * pipes + 0.7 * aot, 0.0, 1.0);

        vertices.clear();
        const std::string title = "LEGO DIMENSIONS";
        Text(vertices, title, 640.0f - TextWidth(title, 8.0f) / 2, 150.0f, 8.0f, kYellow);
        const std::string subtitle = done ? lang::T("INICIANDO O JOGO", "STARTING THE GAME") : lang::T("PREPARANDO SHADERS E PIPELINES", "PREPARING SHADERS AND PIPELINES");
        Text(vertices, subtitle, 640.0f - TextWidth(subtitle, 3.0f) / 2, 230.0f, 3.0f, kWhite);
        std::snprintf(line, sizeof(line), lang::T("PACOTE DE SHADERS AOT: %llu / %llu MB", "AOT SHADER PACK: %llu / %llu MB"), static_cast<unsigned long long>(st.pack_done >> 20),
                      static_cast<unsigned long long>(st.pack_total >> 20));
        Text(vertices, line, 300.0f, 320.0f, 2.5f, st.pack_finished ? kGreen : kWhite);
        std::snprintf(line, sizeof(line), lang::T("SHADERS JA USADOS: %u / %u", "SHADERS ALREADY USED: %u / %u"), st.pairs_done, st.pairs_total);
        Text(vertices, line, 300.0f, 355.0f, 2.5f, st.pairs_finished ? kGreen : kWhite);
        std::snprintf(line, sizeof(line), lang::T("PIPELINES JA VISTOS: %u / %u", "PIPELINES ALREADY SEEN: %u / %u"), st.pipelines_done, st.pipelines_total);
        Text(vertices, line, 300.0f, 390.0f, 2.5f, pipelines_finished ? kGreen : kWhite);
        std::snprintf(line, sizeof(line), lang::T("TODOS OS SHADERS DO PACOTE: %u / %u", "ALL SHADERS IN THE PACK: %u / %u"), st.aot_done, st.aot_total);
        Text(vertices, line, 300.0f, 425.0f, 2.5f, st.aot_finished ? kGreen : kWhite);
        // barra
        Quad(vertices, 296.0f, 476.0f, 984.0f, 504.0f, kGray);
        Quad(vertices, 300.0f, 480.0f, 980.0f, 500.0f, kDark);
        Quad(vertices, 300.0f, 480.0f, 300.0f + 680.0f * static_cast<float>(total), 500.0f, kYellow);
        std::snprintf(line, sizeof(line), "%d%%", static_cast<int>(total * 100.0 + 0.5));
        Text(vertices, line, 640.0f - TextWidth(line, 3.0f) / 2, 520.0f, 3.0f, kWhite);
        const std::string hint = lang::T("TEXTURAS SAO LIDAS DA MEMORIA DO JOGO QUANDO APARECEM", "TEXTURES ARE READ FROM GAME MEMORY WHEN THEY APPEAR");
        Text(vertices, hint, 640.0f - TextWidth(hint, 2.0f) / 2, 600.0f, 2.0f, kGray);
        DrawLoading(vertices);
        if (done || seconds > max_seconds) {
            std::printf("[CARREGAMENTO] tela de carregamento: %.1f s (pacote %s, %u pares, %u de %u pipelines, %u de %u shaders do pacote)%s\n", seconds,
                        st.pack_finished ? "lido" : "INCOMPLETO", st.pairs_done, st.pipelines_done, st.pipelines_total, st.aot_done, st.aot_total,
                        done ? "" : " - tempo maximo atingido, o jogo comeca assim mesmo");
            return;
        }
        Sleep(33);
    }
}

void SetWindowEnabled(bool enabled) {
    g_window_enabled = enabled;
}

u64 BufferAddress(int buffer_index) {
    std::lock_guard lock(g_mutex);
    return buffer_index >= 0 && buffer_index < static_cast<int>(g_buffers.size()) ? reinterpret_cast<u64>(g_buffers[buffer_index]) : 0;
}

// Fase 76: as mesmas conferencias do SubmitFlip, sem enfileirar (o flip vai depois, pela thread de render).
int CheckFlip(int handle, int buffer_index) {
    if (handle != kHandle) {
        return kErrorInvalidHandle;
    }
    std::lock_guard lock(g_mutex);
    return buffer_index < -1 || buffer_index >= static_cast<int>(g_buffers.size()) ? kErrorInvalidValue : err::OK;
}

int SubmitFlip(int handle, int buffer_index, int flip_mode, s64 flip_arg) {
    if (handle != kHandle) {
        return kErrorInvalidHandle;
    }
    std::lock_guard lock(g_mutex);
    if (buffer_index < -1 || buffer_index >= static_cast<int>(g_buffers.size())) {
        return kErrorInvalidValue;
    }
    g_pending.push_back({buffer_index, flip_mode, flip_arg, NowMicros()});
    g_status.flip_pending_num = static_cast<s32>(g_pending.size());
    return err::OK;
}

FlipStatus GetFlipStatus(int handle) {
    std::lock_guard lock(g_mutex);
    return g_status;
}

u64 FlipCount() {
    return g_flip_count.load();
}

void* WindowHandle() {
    return g_hwnd.load();
}

bool WindowHasFocus() {
    const HWND hwnd = g_hwnd.load();
    return hwnd && GetForegroundWindow() == hwnd;
}

} // namespace hle::videoout
