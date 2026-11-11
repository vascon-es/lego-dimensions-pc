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
#include "hle/trophy_manager.h"
#include "hle/overrides/hle_toypad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
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
void SetButtonIcons(const char* name); // hle_gnm.cpp
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
std::atomic<bool> g_force_60hz{true}; // padrao 60 FPS no PC
std::atomic<bool> g_unlocked_fps{false}; // FPS desbloqueado
std::atomic<int> g_blank_frequency{900}; // Padrao 900 Hz
std::condition_variable g_flip_cv;
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
std::atomic<u32> g_window_width{1920}, g_window_height{1080};
bool g_fullscreen = false;
std::atomic<bool> g_stats_overlay{false};
std::atomic<bool> g_video_menu_open{false};
std::atomic<bool> g_achievements_overlay_open{false};
int g_achievements_selected = 0;
int g_achievements_scroll = 0;
int g_menu_selected = 0;
int g_save_confirm_timer = 0;
WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};

struct MenuItem {
    std::string label;
    std::vector<std::string> options;
    int selected = 0;
};

// F2 menu (2.0: in English, with the options the port really has; they match the launcher's)
static std::vector<MenuItem> g_menu_items = {
    {"RESOLUTION (RESTART)", {"1280X720", "1600X900", "1920X1080", "2560X1080", "2560X1440", "3440X1440", "3840X2160"}, 2},
    {"DISPLAY MODE", {"FULLSCREEN", "WINDOWED"}, 0},
    {"ANTI-ALIASING", {"OFF", "TAA X16", "FSR 3 AA", "FSR 4 AA"}, 0},
    {"FRAME RATE (RESTART)", {"30 FPS", "60 FPS", "UNLOCKED", "UNLOCKED NO VSYNC"}, 1},
    {"FPS COUNTER (F10)", {"OFF", "ON"}, 0},
    {"CONTROLLER ICONS", {"AUTO", "PS4", "XBOX", "SWITCH"}, 0},
    {"TROPHY SOUND", {"PLAYSTATION", "XBOX", "STEAM", "OFF"}, 0},
    {"RESET TROPHIES", {"< PRESS ENTER >", "TROPHIES RESET!"}, 0},
    {"POPUP STYLE", {"PS5 (QUALIFIED)", "PS4", "XBOX", "XBOX 360", "STEAM"}, 0},
    {"BLANK FREQUENCY", {"900 HZ", "600 HZ", "300 HZ", "120 HZ", "60 HZ"}, 0}
};

static void WriteLauncherIni(const wchar_t* key, const wchar_t* val) {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::filesystem::path root = std::filesystem::path(exe).parent_path();
        if (root.filename() == L"build") root = root.parent_path();
        WritePrivateProfileStringW(L"launcher", key, val, (root / L"launcher.ini").wstring().c_str());
    }
}

void SetFullscreen(HWND hwnd, bool enabled) {
    if (enabled == g_fullscreen) {
        return;
    }
    if (enabled) {
        GetWindowPlacement(hwnd, &g_windowed_placement);
        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0 || screen_h <= 0) { screen_w = 1920; screen_h = 1080; }
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, screen_w, screen_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fullscreen = enabled;
    log::Tagged("WINDOW", "%s", enabled ? "fullscreen" : "window");
    render::VulkanRenderer::Instance().RequestSwapchainRebuild();
}

} // namespace

void SetWindowResolution(u32 width, u32 height) {
    g_window_width = width;
    g_window_height = height;
    HWND hwnd = static_cast<HWND>(WindowHandle());
    if (hwnd && !g_fullscreen) {
        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        int win_w = rect.right - rect.left;
        int win_h = rect.bottom - rect.top;
        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        int x = (screen_w > win_w) ? (screen_w - win_w) / 2 : 0;
        int y = (screen_h > win_h) ? (screen_h - win_h) / 2 : 0;
        SetWindowPos(hwnd, nullptr, x, y, win_w, win_h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    render::VulkanRenderer::Instance().RequestSwapchainRebuild();
}

namespace {

static void ApplyAndSaveVideoSettings() {
    HWND hwnd = static_cast<HWND>(WindowHandle());

    // Resolution: the window changes now; the game is drawn at the new resolution from the next start
    static const u32 kResW[] = {1280, 1600, 1920, 2560, 2560, 3440, 3840};
    static const u32 kResH[] = {720, 900, 1080, 1080, 1440, 1440, 2160};
    static const wchar_t* const kResStr[] = {L"1280x720", L"1600x900", L"1920x1080", L"2560x1080", L"2560x1440", L"3440x1440", L"3840x2160"};
    const int r = g_menu_items[0].selected;
    if (r >= 0 && r < 7) {
        WriteLauncherIni(L"resolucao", kResStr[r]);
        SetWindowResolution(kResW[r], kResH[r]);
    }

    // Display mode
    const bool want_full = g_menu_items[1].selected == 0;
    WriteLauncherIni(L"telacheia", want_full ? L"1" : L"0");
    if (hwnd) {
        SetFullscreen(hwnd, want_full);
    }

    // Anti-aliasing (2.0: the real options; it used to offer MSAA, which the port never had). Applies right away.
    static const wchar_t* const kAaStr[] = {L"off", L"taa", L"fsraa", L"fsraa4"};
    const int aa = g_menu_items[2].selected;
    if (aa >= 0 && aa < 4) {
        WriteLauncherIni(L"antialiasing", kAaStr[aa]);
        render::SetAntiAliasing(aa == 1 ? 1 : 0);
        if (aa >= 2) {
            render::SetTemporalUpscaler(aa == 3 ? 4 : 3, true);
        } else {
            // back to the launcher's upscaler (FSR 2 / 3 / 4 only work below 100% render scale)
            wchar_t exe[MAX_PATH]{}, buf[32]{};
            int major = 0;
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
                std::filesystem::path root = std::filesystem::path(exe).parent_path();
                if (root.filename() == L"build") root = root.parent_path();
                GetPrivateProfileStringW(L"launcher", L"upscaler", L"off", buf, 32, (root / L"launcher.ini").wstring().c_str());
                major = !_wcsicmp(buf, L"fsr2") ? 2 : !_wcsicmp(buf, L"fsr3") ? 3 : !_wcsicmp(buf, L"fsr4") ? 4 : 0;
            }
            render::SetTemporalUpscaler(major, false);
        }
    }

    // Frame rate: 30 / 60 / unlocked (up to the monitor's refresh, no tearing) / unlocked without VSync. Applies right away.
    const int f = g_menu_items[3].selected;
    static const wchar_t* const kFpsStr[] = {L"30", L"60", L"livre", L"livre_sem_vsync"};
    if (f >= 0 && f < 4) {
        WriteLauncherIni(L"fps", kFpsStr[f]);
        WriteLauncherIni(L"fps60", f == 0 ? L"0" : f == 1 ? L"1" : L"2");
        SetForce60Hz(f != 0);
        SetUnlockedFps(f >= 2);
        render::VulkanRenderer::Instance().SetUnlockedPresent(f >= 2);
        render::VulkanRenderer::Instance().SetNoVsync(f == 3);
        render::VulkanRenderer::Instance().RequestSwapchainRebuild(); // the present mode is picked with the swapchain
    }

    // FPS counter
    SetStatsOverlay(g_menu_items[4].selected == 1);

    // Controller icons
    static const char* const kIconsAnsi[] = {"auto", "ps4", "xboxone", "wiiu"};
    static const wchar_t* const kIconsStr[] = {L"auto", L"ps4", L"xboxone", L"wiiu"};
    const int ic = g_menu_items[5].selected;
    if (ic >= 0 && ic < 4) {
        WriteLauncherIni(L"icones", kIconsStr[ic]);
        gnm::SetButtonIcons(kIconsAnsi[ic]);
    }

    // Trophy sound
    if (g_menu_items.size() > 6) {
        static const wchar_t* const kSoundStr[] = {L"playstation", L"xbox", L"steam", L"off"};
        static const char* const kSoundAnsi[] = {"playstation", "xbox", "steam", "off"};
        const int ts = g_menu_items[6].selected;
        if (ts >= 0 && ts < 4) {
            WriteLauncherIni(L"trophy_sound", kSoundStr[ts]);
            hle::trophy::TrophyManager::Instance().SetSoundType(kSoundAnsi[ts]);
        }
    }

    // Reset trophies
    if (g_menu_items.size() > 7 && g_menu_selected == 7) {
        hle::trophy::TrophyManager::Instance().ResetTrophies();
        g_menu_items[7].selected = 1;
    }

    // Popup style
    if (g_menu_items.size() > 8) {
        static const wchar_t* const kPopupStr[] = {L"ps5", L"ps4", L"xbox", L"xbox360", L"steam"};
        static const char* const kPopupAnsi[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
        const int ps = g_menu_items[8].selected;
        if (ps >= 0 && ps < 5) {
            WriteLauncherIni(L"popup_style", kPopupStr[ps]);
            hle::trophy::TrophyManager::Instance().SetPopupStyle(kPopupAnsi[ps]);
        }
    }

    // Blank frequency
    if (g_menu_items.size() > 9) {
        static const int kBlankHz[] = {900, 600, 300, 120, 60};
        static const wchar_t* const kBlankStr[] = {L"900", L"600", L"300", L"120", L"60"};
        const int bf = g_menu_items[9].selected;
        if (bf >= 0 && bf < 5) {
            WriteLauncherIni(L"blank_frequency", kBlankStr[bf]);
            SetBlankFrequency(kBlankHz[bf]);
        }
    }

    g_save_confirm_timer = 90;
}

// 2.0: the menu shows what the game is running with (the launcher's command line). It used to read the old fps60 key of
// launcher.ini and turn the unlocked frame rate back off at start-up - the launcher writes fps60=1 for "unlocked".
static void InitMenuDefaults() {
    static bool s_init = false;
    if (s_init) return;
    s_init = true;

    if (lang::Pt()) {
        g_menu_items[0].label = "RESOLUCAO (REINICIAR)";
        g_menu_items[1].label = "MODO DE TELA";
        g_menu_items[1].options = {"TELA CHEIA", "JANELA"};
        g_menu_items[2].label = "ANTI-ALIASING";
        g_menu_items[2].options = {"DESLIGADO", "TAA X16", "FSR 3 AA", "FSR 4 AA"};
        g_menu_items[3].label = "TAXA DE QUADROS";
        g_menu_items[3].options = {"30 FPS", "60 FPS", "ILIMITADO", "ILIMITADO SEM VSYNC"};
        g_menu_items[4].label = "CONTADOR FPS (F10)";
        g_menu_items[4].options = {"DESLIGADO", "LIGADO"};
        g_menu_items[5].label = "ICONES DE CONTROLE";
        g_menu_items[6].label = "SOM DE CONQUISTA";
        g_menu_items[6].options = {"PLAYSTATION", "XBOX", "STEAM", "DESLIGADO"};
        g_menu_items[7].label = "REINICIAR CONQUISTAS";
        g_menu_items[7].options = {"< APERTE ENTER >", "CONQUISTAS REINICIADAS!"};
        g_menu_items[8].label = "ESTILO DO POP-UP";
        g_menu_items[8].options = {"PS5 (QUALIFICADO)", "PS4", "XBOX", "XBOX 360", "STEAM"};
        g_menu_items[9].label = "FREQUENCIA BLANK";
        g_menu_items[9].options = {"900 HZ (FLUIDO)", "600 HZ", "300 HZ", "120 HZ", "60 HZ"};
    }
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::filesystem::path root = std::filesystem::path(exe).parent_path();
        if (root.filename() == L"build") root = root.parent_path();
        std::filesystem::path ini = root / L"launcher.ini";
        wchar_t buf[64]{};

        GetPrivateProfileStringW(L"launcher", L"resolucao", L"1920x1080", buf, 64, ini.wstring().c_str());
        static const wchar_t* const kRes[] = {L"1280x720", L"1600x900", L"1920x1080", L"2560x1080", L"2560x1440", L"3440x1440", L"3840x2160"};
        g_menu_items[0].selected = 2;
        for (int i = 0; i < 7; ++i) {
            if (!_wcsicmp(buf, kRes[i])) {
                g_menu_items[0].selected = i;
            }
        }

        GetPrivateProfileStringW(L"launcher", L"icones", L"auto", buf, 64, ini.wstring().c_str());
        if (wcsstr(buf, L"ps4") || wcsstr(buf, L"ps3")) g_menu_items[5].selected = 1;
        else if (wcsstr(buf, L"xbox")) g_menu_items[5].selected = 2;
        else if (wcsstr(buf, L"wiiu") || wcsstr(buf, L"switch")) g_menu_items[5].selected = 3;
        else g_menu_items[5].selected = 0;

        GetPrivateProfileStringW(L"launcher", L"trophy_sound", L"playstation", buf, 64, ini.wstring().c_str());
        if (!_wcsicmp(buf, L"xbox")) g_menu_items[6].selected = 1;
        else if (!_wcsicmp(buf, L"steam")) g_menu_items[6].selected = 2;
        else if (!_wcsicmp(buf, L"off") || !_wcsicmp(buf, L"desligado")) g_menu_items[6].selected = 3;
        else g_menu_items[6].selected = 0;

        GetPrivateProfileStringW(L"launcher", L"popup_style", L"ps5", buf, 64, ini.wstring().c_str());
        if (!_wcsicmp(buf, L"ps4")) g_menu_items[8].selected = 1;
        else if (!_wcsicmp(buf, L"xbox")) g_menu_items[8].selected = 2;
        else if (!_wcsicmp(buf, L"xbox360")) g_menu_items[8].selected = 3;
        else if (!_wcsicmp(buf, L"steam")) g_menu_items[8].selected = 4;
        else g_menu_items[8].selected = 0;

        int bf = GetPrivateProfileIntW(L"launcher", L"blank_frequency", 900, ini.wstring().c_str());
        if (bf == 600) g_menu_items[9].selected = 1;
        else if (bf == 300) g_menu_items[9].selected = 2;
        else if (bf == 120) g_menu_items[9].selected = 3;
        else if (bf == 60) g_menu_items[9].selected = 4;
        else g_menu_items[9].selected = 0;
        SetBlankFrequency(bf);
    }
    g_menu_items[1].selected = g_start_fullscreen.load() || g_fullscreen ? 0 : 1;
    int major = 0;
    bool native = false;
    render::GetTemporalUpscaler(&major, &native);
    g_menu_items[2].selected = native ? (major == 4 ? 3 : 2) : render::GetAntiAliasing() == 1 ? 1 : 0;
    g_menu_items[3].selected = !g_unlocked_fps.load() ? (g_force_60hz.load() ? 1 : 0)
                                                       : (render::VulkanRenderer::Instance().NoVsync() ? 3 : 2);
    g_menu_items[4].selected = g_stats_overlay.load() ? 1 : 0;
}

} // namespace

void ToggleVideoSettingsMenu() {
    InitMenuDefaults();
    g_video_menu_open = !g_video_menu_open;
    log::Tagged("VIDEO-MENU", "%s", g_video_menu_open ? "open" : "closed");
}

bool IsVideoSettingsMenuOpen() {
    return g_video_menu_open.load();
}

void HandleVideoMenuController(u32 pressed) {
    if (!g_video_menu_open) return;
    static auto last_nav = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float, std::milli>(now - last_nav).count() < 160.0f) {
        return;
    }
    // kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80
    // kCross = 0x4000, kCircle = 0x2000
    if (pressed & 0x10) { // Up
        g_menu_selected = (g_menu_selected - 1 + static_cast<int>(g_menu_items.size())) % g_menu_items.size();
        last_nav = now;
    } else if (pressed & 0x40) { // Down
        g_menu_selected = (g_menu_selected + 1) % g_menu_items.size();
        last_nav = now;
    } else if (pressed & 0x80) { // Left
        auto& item = g_menu_items[g_menu_selected];
        item.selected = (item.selected - 1 + static_cast<int>(item.options.size())) % item.options.size();
        last_nav = now;
    } else if (pressed & 0x20) { // Right
        auto& item = g_menu_items[g_menu_selected];
        item.selected = (item.selected + 1) % item.options.size();
        last_nav = now;
    } else if (pressed & 0x4000) { // Cross (Enter / Apply)
        ApplyAndSaveVideoSettings();
        last_nav = now;
    } else if (pressed & 0x2000) { // Circle (Back / Close)
        g_video_menu_open = false;
        last_nav = now;
    }
}

void ToggleAchievementsMenu() {
    g_achievements_overlay_open = !g_achievements_overlay_open;
    log::Tagged("ACHIEVEMENTS-MENU", "%s", g_achievements_overlay_open ? "open" : "closed");
}

bool IsAchievementsMenuOpen() {
    return g_achievements_overlay_open.load();
}

void HandleAchievementsMenuController(u32 pressed) {
    if (!g_achievements_overlay_open) return;
    static auto last_nav = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float, std::milli>(now - last_nav).count() < 160.0f) {
        return;
    }

    const int total_trophies = 77;
    // kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80
    // kL1 = 0x400 (LB), kR1 = 0x800 (RB)
    // kCircle = 0x2000 (B), kCross = 0x4000 (A), kOptions = 0x8
    if (pressed & 0x10) { // Up
        if (g_achievements_selected > 0) --g_achievements_selected;
        last_nav = now;
    } else if (pressed & 0x40) { // Down
        if (g_achievements_selected + 1 < total_trophies) ++g_achievements_selected;
        last_nav = now;
    } else if (pressed & 0x400) { // LB: Page Up
        g_achievements_selected = std::max(0, g_achievements_selected - 6);
        last_nav = now;
    } else if (pressed & 0x800) { // RB: Page Down
        g_achievements_selected = std::min(total_trophies - 1, g_achievements_selected + 6);
        last_nav = now;
    } else if (pressed & (0x2000 | 0x8)) { // Circle / B or Options: Close
        g_achievements_overlay_open = false;
        last_nav = now;
    }

    if (g_achievements_selected < g_achievements_scroll) {
        g_achievements_scroll = g_achievements_selected;
    }
    if (g_achievements_selected >= g_achievements_scroll + 6) {
        g_achievements_scroll = g_achievements_selected - 5;
    }
}

namespace {

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_TIMER: {
        char title[224], warm[64];
        gnm::WarmupStatus(warm, sizeof(warm)); // Fase 36: "Compilando shaders: X / Y" enquanto aquece
        std::snprintf(title, sizeof(title),
                      lang::Pt() ? "[LEGO Dimensions PC] Frames (Flips): %" PRIu64 " | .DAT open: %" PRIu64 " | Threads: %d%s"
                                 : "[LEGO Dimensions PC] Frames (Flips): %" PRIu64 " | .DAT open: %" PRIu64 " | Threads: %d%s",
                      g_flip_count.load(), fs::DatFilesOpened(), runtime::AliveGuestThreads(), warm);
        SetWindowTextA(hwnd, title);
        if (!render::VulkanRenderer::Instance().Active()) {
            InvalidateRect(hwnd, nullptr, FALSE); // sem Vulkan: redesenha o texto por GDI
        }
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && render::VulkanRenderer::Instance().Active()) {
            render::VulkanRenderer::Instance().RequestSwapchainRebuild();
        }
        return 0;
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
                      "Nothing rendered yet.  Enter=OPTIONS  Space=X  Esc=O  X=square  Y=triangle  "
                      "arrows=D-pad  WASD=stick   Flips: %" PRIu64, g_flip_count.load());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F2 && !(lparam & (1 << 30))) {
            ToggleVideoSettingsMenu();
            return 0;
        }
        if (wparam == VK_F9 && !(lparam & (1 << 30))) {
            ToggleAchievementsMenu();
            return 0;
        }
        if (g_achievements_overlay_open.load()) {
            const int total_t = 77;
            if (wparam == VK_UP) {
                if (g_achievements_selected > 0) --g_achievements_selected;
            } else if (wparam == VK_DOWN) {
                if (g_achievements_selected + 1 < total_t) ++g_achievements_selected;
            } else if (wparam == VK_PRIOR) {
                g_achievements_selected = std::max(0, g_achievements_selected - 6);
            } else if (wparam == VK_NEXT) {
                g_achievements_selected = std::min(total_t - 1, g_achievements_selected + 6);
            } else if (wparam == VK_ESCAPE) {
                g_achievements_overlay_open = false;
            }
            if (g_achievements_selected < g_achievements_scroll) g_achievements_scroll = g_achievements_selected;
            if (g_achievements_selected >= g_achievements_scroll + 6) g_achievements_scroll = g_achievements_selected - 5;
            return 0;
        }
        if (g_video_menu_open.load()) {
            if (wparam == VK_UP) {
                g_menu_selected = (g_menu_selected - 1 + static_cast<int>(g_menu_items.size())) % g_menu_items.size();
                return 0;
            }
            if (wparam == VK_DOWN) {
                g_menu_selected = (g_menu_selected + 1) % g_menu_items.size();
                return 0;
            }
            if (wparam == VK_LEFT) {
                auto& item = g_menu_items[g_menu_selected];
                item.selected = (item.selected - 1 + static_cast<int>(item.options.size())) % item.options.size();
                return 0;
            }
            if (wparam == VK_RIGHT) {
                auto& item = g_menu_items[g_menu_selected];
                item.selected = (item.selected + 1) % item.options.size();
                return 0;
            }
            if (wparam == VK_RETURN) {
                ApplyAndSaveVideoSettings();
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                g_video_menu_open = false;
                return 0;
            }
            return 0;
        }
        if (wparam == VK_F11 && !(lparam & (1 << 30))) {
            SetFullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        if (wparam == VK_F10 && !(lparam & (1 << 30))) {
            SetStatsOverlay(!g_stats_overlay);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(lparam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_CLOSE:
        std::printf("\nJanela fechada pelo usuario.\n");
        hle::toypad::SavePortalState();
        render::VulkanRenderer::Instance().SavePipelineCache();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

bool g_check_modules = false; // --check-modules

void WindowThread() {
    InitMenuDefaults();
    WNDCLASSA wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "LegoDimensionsPcWindow";
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); // Fase 50: icone do port (recurso 1)
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    const u32 target_w = g_window_width.load();
    const u32 target_h = g_window_height.load();
    HWND hwnd = nullptr;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0 || screen_h <= 0) {
        screen_w = 1920;
        screen_h = 1080;
    }
    if (g_start_fullscreen.load()) {
        hwnd = CreateWindowA(wc.lpszClassName, "[LEGO Dimensions PC]", WS_POPUP | WS_VISIBLE,
                             0, 0, screen_w, screen_h, nullptr, nullptr, wc.hInstance, nullptr);
        g_fullscreen = true;
    } else {
        LONG client_w = static_cast<LONG>(target_w ? target_w : 1920), client_h = static_cast<LONG>(target_h ? target_h : 1080);
        // 1.9.1: resolucao maior que o monitor (ex. 4K numa tela 1080p): o jogo continua desenhado nela, mas a janela
        // encolhe para caber na area de trabalho, na mesma proporcao (antes passava da tela)
        RECT work{0, 0, screen_w, screen_h};
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
        const LONG max_w = std::max<LONG>(320, work.right - work.left - 16), max_h = std::max<LONG>(240, work.bottom - work.top - 48);
        if (client_w > max_w || client_h > max_h) {
            const double fit = std::min(static_cast<double>(max_w) / client_w, static_cast<double>(max_h) / client_h);
            client_w = static_cast<LONG>(client_w * fit);
            client_h = static_cast<LONG>(client_h * fit);
        }
        RECT rect{0, 0, client_w, client_h};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        int win_w = rect.right - rect.left;
        int win_h = rect.bottom - rect.top;
        int x = (screen_w > win_w) ? (screen_w - win_w) / 2 : CW_USEDEFAULT;
        int y = (screen_h > win_h) ? (screen_h - win_h) / 2 : CW_USEDEFAULT;
        hwnd = CreateWindowA(wc.lpszClassName, "[LEGO Dimensions PC]", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             x, y, win_w, win_h, nullptr, nullptr, wc.hInstance, nullptr);
        g_fullscreen = false;
    }
    if (!hwnd) {
        std::printf("WARNING: could not create window (error %lu)\n", GetLastError());
        return;
    }
    g_hwnd.store(hwnd);
    if (g_start_fullscreen.load()) {
        SetFullscreen(hwnd, true);
    }
    // Backend grafico: superficie do Vulkan presa a esta janela. Se falhar, a janela continua
    // como antes (preta, com o texto por GDI).
    auto& renderer = render::VulkanRenderer::Instance();
    if (renderer.Initialize(hwnd)) {
        log::Tagged("VULKAN", "initialized: %s", renderer.Describe().c_str());
        log::Tagged("VULKAN", "game shaders: %s; validation: %s",
                    renderer.GameShadersSupported() ? "device supports features required by modules (pairs in shaders_spv/pairs)"
                                                    : "device MISSING required features - base shaders only",
                    renderer.ValidationActive() ? "VK_LAYER_KHRONOS_validation enabled" : "desligada");
        // Fase 38: so com --check-modules. A conferencia segura o lock do backend enquanto le do disco
        // e cria um modulo para cada .spv solto (5 a 15 s): a thread de render e as cargas de par
        // ficavam paradas esse tempo todo na abertura.
        if (g_check_modules && !render::GameSpirvDirectory().empty() && !renderer.ValidationActive()) {
            u32 total = 0;
            const u32 accepted = renderer.CheckGameShaderModules(render::GameSpirvDirectory(), &total);
            log::Tagged("VULKAN", "game SPIR-V modules in %s: %u of %u accepted by vkCreateShaderModule",
                        render::GameSpirvDirectory().c_str(), accepted, total);
        }
    } else {
        log::Tagged("VULKAN", "NOT initialized: %s", renderer.LastError().c_str());
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
        if (g_unlocked_fps.load(std::memory_order_relaxed)) {
            std::unique_lock lock(g_mutex);
            g_flip_cv.wait(lock, [] { return !g_pending.empty() || !g_unlocked_fps.load(std::memory_order_relaxed); });
            if (!g_unlocked_fps.load(std::memory_order_relaxed) || g_pending.empty()) {
                continue;
            }
            const PendingFlip flip = g_pending.front();
            g_pending.pop_front();
            const u64 now_us = NowMicros();
            ++g_status.count;
            g_status.process_time = now_us;
            g_status.tsc = __rdtsc();
            g_status.submit_tsc = flip.submit_time;
            g_status.flip_arg = flip.flip_arg;
            g_status.current_buffer = flip.buffer_index;
            g_status.gc_queue_num = 0;
            g_status.flip_pending_num = static_cast<s32>(g_pending.size());
            g_flip_count.store(g_status.count);
            const s64 event_data = static_cast<s64>((now_us & 0xFFF) | ((g_status.count & 0xF) << 12) |
                                                    (static_cast<u64>(flip.flip_arg) << 16));
            const std::vector<FlipListener> listeners = g_listeners;
            lock.unlock();
            for (const FlipListener& listener : listeners) {
                equeue::Trigger(listener.queue, kEventFlip, equeue::kFilterVideoOut, event_data);
            }
            continue;
        }

        int rate;
        {
            std::lock_guard lock(g_mutex);
            rate = g_force_60hz.load(std::memory_order_relaxed) ? 0 : g_flip_rate;
        }
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;
        }
        const int hz = g_blank_frequency.load(std::memory_order_relaxed);
        const int effective_hz = hz > 0 ? hz : 900;
        const int period_us = std::max(500, 1000000 / effective_hz);
        next += std::chrono::microseconds(period_us) * (rate + 1);
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
    HLE_TRACE("user=%d bus=%d index=%d -> handle %d%s", user_id, bus_type, index, kHandle,
              start_window ? " (window created)" : "");
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
    HLE_TRACE("format=0x%x tiling=%u %ux%u pitch=%u", pixel_format, tiling_mode, width, height, pitch_in_pixel);
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
    HLE_TRACE("handle=%d start=%d buffers=%d %ux%u first=%p -> %d", handle, start_index, buffer_count,
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

u32 WindowWidth() {
    return g_window_width.load();
}

u32 WindowHeight() {
    return g_window_height.load();
}

void SetForce60Hz(bool enabled) {
    g_force_60hz = enabled;
}

void SetUnlockedFps(bool enabled) {
    g_unlocked_fps = enabled;
    g_flip_cv.notify_all();
}

bool IsUnlockedFps() {
    return g_unlocked_fps.load(std::memory_order_relaxed);
}

void SetBlankFrequency(int hz) {
    g_blank_frequency.store(hz > 0 ? hz : 900);
}

int GetBlankFrequency() {
    return g_blank_frequency.load();
}

void InitVideoDefaults() {
    InitMenuDefaults();
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
                    kDegree[7] = {0x0C, 0x12, 0x12, 0x0C, 0x00, 0x00, 0x00},
                    kLeftAngle[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
                    kRightAngle[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
                    kLeftBracket[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
                    kRightBracket[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
                    kLeftParen[7] = {0x04, 0x08, 0x10, 0x10, 0x10, 0x08, 0x04},
                    kRightParen[7] = {0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x04},
                    kPlus[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
                    kEqual[7] = {0x00, 0x1F, 0x00, 0x00, 0x1F, 0x00, 0x00};
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    static const u8 kExcl[7] = {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x04};
    static const u8 kQuest[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
    static const u8 kComma[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08};
    static const u8 kApos[7] = {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00};
    static const u8 kQuote[7] = {0x0A, 0x0A, 0x14, 0x00, 0x00, 0x00, 0x00};
    static const u8 kStar[7] = {0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00};
    static const u8 kUnder[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};

    // Normalize accents
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc == 0xC1 || uc == 0xC0 || uc == 0xC2 || uc == 0xC3 || uc == 0xE1 || uc == 0xE0 || uc == 0xE2 || uc == 0xE3) return kLetters['A' - 'A'];
    if (uc == 0xC9 || uc == 0xCA || uc == 0xE9 || uc == 0xEA) return kLetters['E' - 'A'];
    if (uc == 0xCD || uc == 0xED) return kLetters['I' - 'A'];
    if (uc == 0xD3 || uc == 0xD4 || uc == 0xD5 || uc == 0xF3 || uc == 0xF4 || uc == 0xF5) return kLetters['O' - 'A'];
    if (uc == 0xDA || uc == 0xFA) return kLetters['U' - 'A'];
    if (uc == 0xC7 || uc == 0xE7) return kLetters['C' - 'A'];

    switch (c) {
    case '/': return kSlash;
    case '%': return kPercent;
    case ':': return kColon;
    case '.': return kDot;
    case '-': return kDash;
    case '^': return kDegree;
    case '<': return kLeftAngle;
    case '>': return kRightAngle;
    case '[': return kLeftBracket;
    case ']': return kRightBracket;
    case '(': return kLeftParen;
    case ')': return kRightParen;
    case '+': return kPlus;
    case '=': return kEqual;
    case '!': return kExcl;
    case '?': return kQuest;
    case ',': return kComma;
    case '\'': return kApos;
    case '"': return kQuote;
    case '*': return kStar;
    case '_': return kUnder;
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
    void Circle(float cx, float cy, float r, u32 rgba) {
        const int steps = static_cast<int>(r);
        for (int dy = -steps; dy <= steps; ++dy) {
            float fdy = static_cast<float>(dy);
            float dx = std::sqrt(std::max(0.0f, r * r - fdy * fdy));
            Rect(cx - dx, cy + fdy, cx + dx, cy + fdy + 1.0f, rgba);
        }
    }
    void Pill(float x0, float y0, float x1, float y1, u32 rgba) {
        float r = (y1 - y0) * 0.5f;
        Rect(x0 + r, y0, x1 - r, y1, rgba);
        const int steps = static_cast<int>(r);
        for (int dy = -steps; dy <= steps; ++dy) {
            float fdy = static_cast<float>(dy);
            float dx = std::sqrt(std::max(0.0f, r * r - fdy * fdy));
            Rect(x0 + r - dx, y0 + r + fdy, x0 + r, y0 + r + fdy + 1.0f, rgba);
            Rect(x1 - r, y0 + r + fdy, x1 - r + dx, y0 + r + fdy + 1.0f, rgba);
        }
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

constexpr u8 kTrophyCup[7] = {0x1F, 0x1F, 0x0E, 0x04, 0x04, 0x0E, 0x1F};
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
    if (!hwnd || !GetClientRect(hwnd, &client) || client.right < 64 || client.bottom < 64) {
        return;
    }
    const bool has_toast = hle::trophy::TrophyManager::Instance().HasActiveToast();
    hle::trophy::TrophyManager::Instance().Update(ms * 0.001f);
    if (!g_stats_overlay.load() && !g_video_menu_open.load() && !g_achievements_overlay_open.load() && !has_toast) {
        return;
    }
    static std::vector<LoadingVertex> vertices;
    vertices.clear();
    OverlayCanvas c{vertices, static_cast<float>(client.right), static_cast<float>(client.bottom)};
    const float p = client.bottom >= 1400 ? 3.0f : 2.0f; // tamanho do pixel da fonte

    if (g_stats_overlay.load()) {
        const runtime::hwmon::Snapshot s = runtime::hwmon::Get();
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
    }
    if (g_video_menu_open.load()) {
        constexpr u32 kBg = 0xEE121216, kBorder = 0xFF00D000, kTitleYellow = 0xFF00E0FF, kHiBg = 0xAA303038;
        constexpr u32 kWhite = 0xFFFFFFFF, kGray = 0xFFA0A0A0, kCyan = 0xFFE0C040;
        
        float scale_ui = client.bottom >= 1400 ? 1.5f : (client.bottom >= 1000 ? 1.0f : 0.75f);
        float box_w = 700.0f * scale_ui;
        float box_h = 560.0f * scale_ui;
        float box_x0 = (client.right - box_w) / 2.0f;
        float box_y0 = (client.bottom - box_h) / 2.0f;
        float box_x1 = box_x0 + box_w;
        float box_y1 = box_y0 + box_h;
        
        c.Rect(box_x0 - 4.0f, box_y0 - 4.0f, box_x1 + 4.0f, box_y1 + 4.0f, kBorder);
        c.Rect(box_x0, box_y0, box_x1, box_y1, kBg);
        
        const std::string title = lang::Pt() ? "CONFIGURACOES DE VIDEO" : "VIDEO SETTINGS";
        float font_title = 3.5f * scale_ui;
        c.Write(title, box_x0 + (box_w - TextWidth(title, font_title)) / 2.0f, box_y0 + 20.0f * scale_ui, font_title, kTitleYellow);
        
        c.Rect(box_x0 + 20.0f * scale_ui, box_y0 + 55.0f * scale_ui, box_x1 - 20.0f * scale_ui, box_y0 + 57.0f * scale_ui, kGray);
        
        float row_y = box_y0 + 75.0f * scale_ui;
        float font_item = 2.4f * scale_ui;
        for (size_t i = 0; i < g_menu_items.size(); ++i) {
            const auto& item = g_menu_items[i];
            const bool is_sel = (static_cast<int>(i) == g_menu_selected);
            if (is_sel) {
                c.Rect(box_x0 + 16.0f * scale_ui, row_y - 4.0f * scale_ui, box_x1 - 16.0f * scale_ui, row_y + 26.0f * scale_ui, kHiBg);
            }
            c.Write(is_sel ? ("> " + item.label) : ("  " + item.label), box_x0 + 24.0f * scale_ui, row_y, font_item, is_sel ? kTitleYellow : kWhite);
            
            std::string val_str = "< " + item.options[item.selected] + " >";
            float val_w = TextWidth(val_str, font_item);
            c.Write(val_str, box_x1 - val_w - 32.0f * scale_ui, row_y, font_item, is_sel ? kCyan : kGray);
            
            row_y += 38.0f * scale_ui;
        }
        
        c.Rect(box_x0 + 20.0f * scale_ui, box_y1 - 60.0f * scale_ui, box_x1 - 20.0f * scale_ui, box_y1 - 58.0f * scale_ui, kGray);
        if (g_save_confirm_timer > 0) {
            --g_save_confirm_timer;
            const std::string saved_msg = lang::Pt() ? "CONFIGURACOES SALVAS!" : "SETTINGS SAVED!";
            c.Write(saved_msg, box_x0 + (box_w - TextWidth(saved_msg, font_item)) / 2.0f, box_y1 - 42.0f * scale_ui, font_item, kBorder);
        } else {
            const std::string footer = lang::Pt() ? "[CIMA/BAIXO] NAVEGAR  [ESQ/DIR] ALTERAR  [ENTER] SALVAR  [F2/ESC] SAIR" : "[UP/DOWN] SELECT  [LEFT/RIGHT] CHANGE  [ENTER] SAVE  [F2/ESC] CLOSE";
            float font_foot = 1.7f * scale_ui;
            c.Write(footer, box_x0 + (box_w - TextWidth(footer, font_foot)) / 2.0f, box_y1 - 40.0f * scale_ui, font_foot, kGray);
        }
    }

    if (g_achievements_overlay_open.load()) {
        const float scale_ui = client.bottom >= 1400 ? 1.4f : (client.bottom >= 1000 ? 1.0f : 0.75f);
        const float box_w = 880.0f * scale_ui;
        const float box_h = 630.0f * scale_ui;
        const float box_x0 = (client.right - box_w) * 0.5f;
        const float box_y0 = (client.bottom - box_h) * 0.5f;
        const float box_x1 = box_x0 + box_w;
        const float box_y1 = box_y0 + box_h;

        // Dim background
        c.Rect(0, 0, client.right, client.bottom, 0x99000000);

        // Outer glow frame in bright cyan
        c.Rect(box_x0 - 3.0f * scale_ui, box_y0 - 3.0f * scale_ui, box_x1 + 3.0f * scale_ui, box_y1 + 3.0f * scale_ui, 0xFF00E5FF);
        // Frosted dark modal body
        c.Rect(box_x0, box_y0, box_x1, box_y1, 0xF60E121B);

        int unlocked = 0, total = 77, n_plat = 0, n_gold = 0, n_silv = 0, n_brz = 0;
        hle::trophy::TrophyManager::Instance().GetProgress(&unlocked, &total, &n_plat, &n_gold, &n_silv, &n_brz);

        // Header Title
        const std::string title = lang::Pt() ? "CONQUISTAS DO JOGO" : "GAME ACHIEVEMENTS";
        const float font_title = 3.2f * scale_ui;
        c.Write(title, box_x0 + 26.0f * scale_ui, box_y0 + 16.0f * scale_ui, font_title, 0xFFFFD700);

        // Progress text on right
        char pct_buf[64];
        const int pct = total > 0 ? (unlocked * 100 / total) : 0;
        std::snprintf(pct_buf, sizeof(pct_buf), "%d / %d (%d%%)", unlocked, total, pct);
        const float font_pct = 2.0f * scale_ui;
        const float pct_w = TextWidth(pct_buf, font_pct);
        c.Write(pct_buf, box_x1 - pct_w - 26.0f * scale_ui, box_y0 + 16.0f * scale_ui, font_pct, 0xFF00E5FF);

        // Progress bar
        const float bar_x0 = box_x0 + 26.0f * scale_ui;
        const float bar_x1 = box_x1 - 26.0f * scale_ui;
        const float bar_y = box_y0 + 44.0f * scale_ui;
        const float bar_h = 7.0f * scale_ui;
        c.Pill(bar_x0, bar_y, bar_x1, bar_y + bar_h, 0xFF222634);
        if (total > 0 && unlocked > 0) {
            const float fill_w = (bar_x1 - bar_x0) * (static_cast<float>(unlocked) / total);
            c.Pill(bar_x0, bar_y, bar_x0 + fill_w, bar_y + bar_h, 0xFF00E5FF);
        }

        // Tier counts
        const float tier_y = box_y0 + 58.0f * scale_ui;
        const float font_tier = 1.6f * scale_ui;
        char buf_tier[64];

        std::snprintf(buf_tier, sizeof(buf_tier), lang::Pt() ? "PLATINA: %d/1" : "PLATINUM: %d/1", n_plat);
        c.Write(buf_tier, bar_x0, tier_y, font_tier, 0xFF00E5FF);

        std::snprintf(buf_tier, sizeof(buf_tier), lang::Pt() ? "OURO: %d/4" : "GOLD: %d/4", n_gold);
        c.Write(buf_tier, bar_x0 + 180.0f * scale_ui, tier_y, font_tier, 0xFFFFD700);

        std::snprintf(buf_tier, sizeof(buf_tier), lang::Pt() ? "PRATA: %d/13" : "SILVER: %d/13", n_silv);
        c.Write(buf_tier, bar_x0 + 340.0f * scale_ui, tier_y, font_tier, 0xFFD0D0E0);

        std::snprintf(buf_tier, sizeof(buf_tier), lang::Pt() ? "BRONZE: %d/59" : "BRONZE: %d/59", n_brz);
        c.Write(buf_tier, bar_x0 + 510.0f * scale_ui, tier_y, font_tier, 0xFFCD7F32);

        // Divider
        c.Rect(box_x0 + 20.0f * scale_ui, box_y0 + 78.0f * scale_ui, box_x1 - 20.0f * scale_ui, box_y0 + 80.0f * scale_ui, 0xFF353C4E);

        // List of 6 visible items
        size_t count = 0;
        const TrophyDef* defs = GetTrophyDefs(&count);

        const float list_x0 = box_x0 + 22.0f * scale_ui;
        const float list_x1 = box_x1 - 38.0f * scale_ui;
        const float list_y0 = box_y0 + 88.0f * scale_ui;
        const float item_h = 76.0f * scale_ui;
        const float item_gap = 6.0f * scale_ui;

        for (int v = 0; v < 6; ++v) {
            const int idx = g_achievements_scroll + v;
            if (idx >= static_cast<int>(count)) break;

            const TrophyDef& def = defs[idx];
            const bool is_unlocked = hle::trophy::TrophyManager::Instance().IsUnlocked(def.id);
            const bool is_sel = (idx == g_achievements_selected);

            const float iy0 = list_y0 + v * (item_h + item_gap);
            const float iy1 = iy0 + item_h;

            u32 tier_color = 0xFFCD7F32;
            const char* tier_label = "BRONZE";
            if (def.type == 'P') { tier_color = 0xFF00E5FF; tier_label = lang::Pt() ? "PLATINA" : "PLATINUM"; }
            else if (def.type == 'G') { tier_color = 0xFFFFD700; tier_label = lang::Pt() ? "OURO" : "GOLD"; }
            else if (def.type == 'S') { tier_color = 0xFFD0D0E0; tier_label = lang::Pt() ? "PRATA" : "SILVER"; }

            // Card background
            if (is_sel) {
                c.Rect(list_x0 - 2.0f * scale_ui, iy0 - 2.0f * scale_ui, list_x1 + 2.0f * scale_ui, iy1 + 2.0f * scale_ui, 0xFF00E5FF);
                c.Rect(list_x0, iy0, list_x1, iy1, 0xFF1C273C);
                c.Rect(list_x0, iy0, list_x0 + 6.0f * scale_ui, iy1, 0xFF00E5FF);
            } else {
                c.Rect(list_x0, iy0, list_x1, iy1, is_unlocked ? 0xDD141824 : 0xAA10131C);
            }

            // Left Trophy Cup
            const float cr = 20.0f * scale_ui;
            const float cx = list_x0 + 16.0f * scale_ui + cr;
            const float cy = iy0 + item_h * 0.5f;
            c.Circle(cx, cy, cr, is_unlocked ? (tier_color & 0x44FFFFFF) : 0x33000000);
            c.Circle(cx, cy, cr - 1.5f * scale_ui, is_unlocked ? 0x22FFFFFF : 0x22000000);

            const float cup_p = scale_ui * 2.2f;
            const float cup_x = cx - 2.5f * cup_p;
            const float cup_y = cy - 3.5f * cup_p;
            const u32 cup_col = is_unlocked ? tier_color : 0xFF555B68;
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (kTrophyCup[r] & (0x10 >> col)) {
                        c.Rect(cup_x + col * cup_p, cup_y + r * cup_p, cup_x + (col + 1) * cup_p, cup_y + (r + 1) * cup_p, cup_col);
                    }
                }
            }

            // Text
            const float text_x = cx + cr + 14.0f * scale_ui;
            std::string line1_tier = std::string("[") + tier_label + "] ";
            c.Write(line1_tier, text_x, iy0 + 14.0f * scale_ui, 1.7f * scale_ui, tier_color);

            float tier_w = TextWidth(line1_tier, 1.7f * scale_ui);
            std::string trophy_name = lang::Pt() ? def.name_pt : def.name_en;
            c.Write(trophy_name, text_x + tier_w, iy0 + 14.0f * scale_ui, 1.9f * scale_ui, is_unlocked ? 0xFFFFFFFF : 0xFFA0A5B2);

            std::string trophy_desc = lang::Pt() ? def.desc_pt : def.desc_en;
            c.Write(trophy_desc, text_x, iy0 + 44.0f * scale_ui, 1.4f * scale_ui, is_unlocked ? 0xFF9EAAB8 : 0xFF656C7A);

            // Right status pill
            const float st_w = 126.0f * scale_ui;
            const float st_h = 24.0f * scale_ui;
            const float st_x = list_x1 - st_w - 14.0f * scale_ui;
            const float st_y = iy0 + (item_h - st_h) * 0.5f;

            if (is_unlocked) {
                c.Pill(st_x, st_y, st_x + st_w, st_y + st_h, 0xFF145A1E);
                const std::string ok_str = lang::Pt() ? "DESBLOQUEADO" : "UNLOCKED";
                const float ok_w = TextWidth(ok_str, 1.3f * scale_ui);
                c.Write(ok_str, st_x + (st_w - ok_w) * 0.5f, st_y + 6.0f * scale_ui, 1.3f * scale_ui, 0xFF85E89D);
            } else {
                c.Pill(st_x, st_y, st_x + st_w, st_y + st_h, 0xFF242834);
                const std::string lk_str = lang::Pt() ? "BLOQUEADO" : "LOCKED";
                const float lk_w = TextWidth(lk_str, 1.3f * scale_ui);
                c.Write(lk_str, st_x + (st_w - lk_w) * 0.5f, st_y + 6.0f * scale_ui, 1.3f * scale_ui, 0xFF707684);
            }
        }

        // Scrollbar track & thumb
        const float sb_x0 = box_x1 - 28.0f * scale_ui;
        const float sb_x1 = box_x1 - 22.0f * scale_ui;
        const float sb_y0 = list_y0;
        const float sb_y1 = list_y0 + 6 * (item_h + item_gap) - item_gap;
        c.Pill(sb_x0, sb_y0, sb_x1, sb_y1, 0xFF1A202E);

        if (count > 6) {
            const float thumb_h = std::max(24.0f * scale_ui, (sb_y1 - sb_y0) * 6.0f / count);
            const float thumb_y0 = sb_y0 + (sb_y1 - sb_y0 - thumb_h) * (static_cast<float>(g_achievements_scroll) / (count - 6));
            c.Pill(sb_x0, thumb_y0, sb_x1, thumb_y0 + thumb_h, 0xFF00E5FF);
        }

        // Bottom divider
        c.Rect(box_x0 + 20.0f * scale_ui, box_y1 - 42.0f * scale_ui, box_x1 - 20.0f * scale_ui, box_y1 - 40.0f * scale_ui, 0xFF353C4E);

        // Footer
        const std::string footer = lang::Pt()
            ? "[D-PAD] NAVEGAR   [LB / RB] PAGINA   [B / ESC / F9] FECHAR   [RB+LB+RT] ALTERNAR"
            : "[D-PAD] NAVIGATE   [LB / RB] PAGE   [B / ESC / F9] CLOSE   [RB+LB+RT] TOGGLE";
        const float font_foot = 1.5f * scale_ui;
        c.Write(footer, box_x0 + (box_w - TextWidth(footer, font_foot)) * 0.5f, box_y1 - 28.0f * scale_ui, font_foot, 0xFF8A94A6);
    }

    if (has_toast) {
        const auto toast = hle::trophy::TrophyManager::Instance().GetActiveToast();
        float anim = 1.0f;
        const float time_in = toast.total_duration - toast.time_remaining;
        if (time_in < 0.35f) {
            anim = time_in / 0.35f;
        } else if (toast.time_remaining < 0.35f) {
            anim = toast.time_remaining / 0.35f;
        }

        const float scale_ui = client.bottom >= 1400 ? 1.4f : (client.bottom >= 1000 ? 1.0f : 0.8f);
        const std::string style = toast.popup_style.empty() ? "ps5" : toast.popup_style;

        u32 tier_col = 0xFFCD7F32; // Bronze
        const char* tier_en = "BRONZE";
        const char* tier_pt = "BRONZE";
        if (toast.type == 'P') {
            tier_col = 0xFF00E5FF; // Platinum
            tier_en = "PLATINUM";
            tier_pt = "PLATINA";
        } else if (toast.type == 'G') {
            tier_col = 0xFFFFD700; // Gold
            tier_en = "GOLD";
            tier_pt = "OURO";
        } else if (toast.type == 'S') {
            tier_col = 0xFFD0D0E0; // Silver
            tier_en = "SILVER";
            tier_pt = "PRATA";
        }
        const char* tier_name = lang::Pt() ? tier_pt : tier_en;

        // (kTrophyCup defined in namespace above)

        if (style == "xbox") {
            // Modern Xbox One / Series style: vibrant green pill capsule
            const float box_w = 480.0f * scale_ui;
            const float box_h = 66.0f * scale_ui;
            const float cur_x = (client.right - box_w) * 0.5f;
            const float cur_y = 24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui);

            c.Pill(cur_x, cur_y + 3.0f * scale_ui, cur_x + box_w, cur_y + box_h + 3.0f * scale_ui, 0x66000000);
            c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, 0xFF107C10);

            const float cr = (box_h - 16.0f * scale_ui) * 0.5f;
            const float cx = cur_x + 14.0f * scale_ui + cr;
            const float cy = cur_y + box_h * 0.5f;
            c.Circle(cx, cy, cr, 0xFF0C620C);

            const float cup_p = scale_ui * 2.0f;
            const float cup_x = cx - 2.5f * cup_p;
            const float cup_y = cy - 3.5f * cup_p;
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (kTrophyCup[r] & (0x10 >> col)) {
                        c.Rect(cup_x + col * cup_p, cup_y + r * cup_p, cup_x + (col + 1) * cup_p, cup_y + (r + 1) * cup_p, 0xFFFFFFFF);
                    }
                }
            }

            const float text_x = cx + cr + 14.0f * scale_ui;
            std::string hdr = lang::Pt() ? "Conquista desbloqueada" : "Achievement unlocked";
            c.Write(hdr, text_x, cur_y + 11.0f * scale_ui, 1.8f * scale_ui, 0xFFFFFFFF);
            c.Write(toast.title, text_x, cur_y + 35.0f * scale_ui, 1.6f * scale_ui, 0xFFD8FFD8);

        } else if (style == "xbox360") {
            // Classic Xbox 360 style: dark charcoal pill with green guide orb
            const float box_w = 460.0f * scale_ui;
            const float box_h = 64.0f * scale_ui;
            const float cur_x = (client.right - box_w) * 0.5f;
            const float cur_y = 24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui);

            c.Pill(cur_x - 1.5f * scale_ui, cur_y - 1.5f * scale_ui, cur_x + box_w + 1.5f * scale_ui, cur_y + box_h + 1.5f * scale_ui, 0xFF505054);
            c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, 0xF21C1C1F);

            const float cr = (box_h - 16.0f * scale_ui) * 0.5f;
            const float cx = cur_x + 14.0f * scale_ui + cr;
            const float cy = cur_y + box_h * 0.5f;
            c.Circle(cx, cy, cr, 0xFF353538);
            c.Circle(cx, cy, cr - 3.0f * scale_ui, 0xFF7D7D85);

            // Guide quarter arc (upper left green)
            for (float dy = -cr + 1.0f; dy <= 0.0f; dy += 1.0f) {
                float dx = std::sqrt(std::max(0.0f, (cr - 1.0f) * (cr - 1.0f) - dy * dy));
                c.Rect(cx - dx, cy + dy, cx, cy + dy + 1.0f, 0xFF52B022);
            }

            const float text_x = cx + cr + 14.0f * scale_ui;
            std::string hdr = lang::Pt() ? "Conquista desbloqueada" : "Achievement unlocked";
            c.Write(hdr, text_x, cur_y + 11.0f * scale_ui, 1.8f * scale_ui, 0xFFE0E0E0);
            c.Write(toast.title, text_x, cur_y + 34.0f * scale_ui, 1.6f * scale_ui, 0xFFFFFFFF);

        } else if (style == "ps4") {
            // PlayStation 4 style: clean dark slate notification banner at top-right
            const float box_w = 460.0f * scale_ui;
            const float box_h = 72.0f * scale_ui;
            const float cur_x = client.right - box_w - 24.0f * scale_ui;
            const float cur_y = 24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui);

            c.Rect(cur_x - 2.0f * scale_ui, cur_y - 2.0f * scale_ui, cur_x + box_w + 2.0f * scale_ui, cur_y + box_h + 2.0f * scale_ui, 0xFF363842);
            c.Rect(cur_x, cur_y, cur_x + box_w, cur_y + box_h, 0xEE16181E);

            const float badge_sz = box_h - 16.0f * scale_ui;
            const float badge_x = cur_x + 8.0f * scale_ui;
            const float badge_y = cur_y + 8.0f * scale_ui;
            c.Rect(badge_x, badge_y, badge_x + badge_sz, badge_y + badge_sz, 0xFF242630);

            const float cup_p = scale_ui * 2.2f;
            const float cup_x = badge_x + (badge_sz - 5.0f * cup_p) * 0.5f;
            const float cup_y = badge_y + (badge_sz - 7.0f * cup_p) * 0.5f;
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (kTrophyCup[r] & (0x10 >> col)) {
                        c.Rect(cup_x + col * cup_p, cup_y + r * cup_p, cup_x + (col + 1) * cup_p, cup_y + (r + 1) * cup_p, tier_col);
                    }
                }
            }

            const float text_x = badge_x + badge_sz + 12.0f * scale_ui;
            c.Write(toast.title, text_x, cur_y + 14.0f * scale_ui, 2.0f * scale_ui, 0xFFFFFFFF);
            std::string sub = lang::Pt() ? "Trofeu conquistado!" : "Trophy earned!";
            c.Write(sub, text_x, cur_y + 40.0f * scale_ui, 1.4f * scale_ui, 0xFFA0A0B2);

        } else if (style == "steam") {
            // Steam style: navy rectangular banner at bottom-right
            const float box_w = 420.0f * scale_ui;
            const float box_h = 72.0f * scale_ui;
            const float cur_x = client.right - box_w - 24.0f * scale_ui;
            const float cur_y = client.bottom - box_h - 24.0f * scale_ui + (1.0f - anim) * (box_h + 35.0f * scale_ui);

            c.Rect(cur_x - 2.0f * scale_ui, cur_y - 2.0f * scale_ui, cur_x + box_w + 2.0f * scale_ui, cur_y + box_h + 2.0f * scale_ui, 0xFF2B475D);
            c.Rect(cur_x, cur_y, cur_x + box_w, cur_y + box_h, 0xFA171A21);

            const float badge_sz = box_h - 16.0f * scale_ui;
            const float badge_x = cur_x + 8.0f * scale_ui;
            const float badge_y = cur_y + 8.0f * scale_ui;
            c.Rect(badge_x, badge_y, badge_x + badge_sz, badge_y + badge_sz, 0xFF0E141B);

            const float cup_p = scale_ui * 2.2f;
            const float cup_x = badge_x + (badge_sz - 5.0f * cup_p) * 0.5f;
            const float cup_y = badge_y + (badge_sz - 7.0f * cup_p) * 0.5f;
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (kTrophyCup[r] & (0x10 >> col)) {
                        c.Rect(cup_x + col * cup_p, cup_y + r * cup_p, cup_x + (col + 1) * cup_p, cup_y + (r + 1) * cup_p, 0xFF66C0F4);
                    }
                }
            }

            const float text_x = badge_x + badge_sz + 12.0f * scale_ui;
            std::string hdr = lang::Pt() ? "Steam - Conquista Desbloqueada" : "Steam - Achievement Unlocked";
            c.Write(hdr, text_x, cur_y + 9.0f * scale_ui, 1.4f * scale_ui, 0xFF66C0F4);
            c.Write(toast.title, text_x, cur_y + 26.0f * scale_ui, 1.8f * scale_ui, 0xFFFFFFFF);
            c.Write(toast.desc, text_x, cur_y + 48.0f * scale_ui, 1.2f * scale_ui, 0xFF8F98A0);

        } else {
            // Default / PS5: PlayStation 5 style with full Trophy Tier Qualification
            const float box_w = 500.0f * scale_ui;
            const float box_h = 78.0f * scale_ui;
            const float cur_x = (client.right - box_w) * 0.5f;
            const float cur_y = 24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui);

            // Outer glowing pill outline in tier color
            c.Pill(cur_x - 3.0f * scale_ui, cur_y - 3.0f * scale_ui, cur_x + box_w + 3.0f * scale_ui, cur_y + box_h + 3.0f * scale_ui, tier_col);
            // Sleek dark frosted glass body
            c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, 0xF20F1218);

            // Side tier indicator bar
            c.Pill(cur_x + 8.0f * scale_ui, cur_y + 12.0f * scale_ui, cur_x + 14.0f * scale_ui, cur_y + box_h - 12.0f * scale_ui, tier_col);

            // Left circular trophy badge
            const float cr = (box_h - 18.0f * scale_ui) * 0.5f;
            const float cx = cur_x + 22.0f * scale_ui + cr;
            const float cy = cur_y + box_h * 0.5f;
            c.Circle(cx, cy, cr, 0x44000000);

            const float cup_p = scale_ui * 2.4f;
            const float cup_x = cx - 2.5f * cup_p;
            const float cup_y = cy - 3.5f * cup_p;
            for (int r = 0; r < 7; ++r) {
                for (int col = 0; col < 5; ++col) {
                    if (kTrophyCup[r] & (0x10 >> col)) {
                        c.Rect(cup_x + col * cup_p, cup_y + r * cup_p, cup_x + (col + 1) * cup_p, cup_y + (r + 1) * cup_p, tier_col);
                    }
                }
            }

            const float text_x = cx + cr + 14.0f * scale_ui;
            // Tier qualification header
            std::string hdr = std::string(lang::Pt() ? "* TROFEU DE " : "* ") + tier_name + (lang::Pt() ? " CONQUISTADO!" : " TROPHY EARNED!");
            c.Write(hdr, text_x, cur_y + 10.0f * scale_ui, 1.55f * scale_ui, tier_col);
            c.Write(toast.title, text_x, cur_y + 28.0f * scale_ui, 2.1f * scale_ui, 0xFFFFFFFF);
            c.Write(toast.desc, text_x, cur_y + 52.0f * scale_ui, 1.35f * scale_ui, 0xFFAAAAAA);
        }
    }

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
        draw.blend = true;
        draw.uv_format = render::DrawCall::kUvNone;
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(draw.mvp, kIdentity, sizeof(kIdentity));
        draw.viewport[0] = draw.viewport[1] = 0.0f;
        draw.viewport[2] = draw.target_size[0] = c.w;
        draw.viewport[3] = draw.target_size[1] = c.h;
        render::VulkanRenderer::Instance().QueueDraw(draw);
    }
}

bool OpenWindowEarly(u32 timeout_ms) {
    InitMenuDefaults();
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

// Fase 77: marca do driver para o qual todos os pipelines conhecidos ja foram criados (cache do driver quente).
std::filesystem::path DriverStampFile() {
    const std::string& dir = render::GameSpirvDirectory();
    return dir.empty() ? std::filesystem::path{} : std::filesystem::path(dir) / "pipelines_driver.txt";
}

bool RunLoadingScreen(u32 max_seconds, bool prewarm_only) {
    constexpr u32 kYellow = 0xFF00D5FF, kWhite = 0xFFFFFFFF, kGray = 0xFF808080, kDark = 0xFF303030, kGreen = 0xFF60D060;
    const auto start = std::chrono::steady_clock::now();
    std::vector<LoadingVertex> vertices;
    char line[160];
    // Fase 77: driver novo (ou primeira vez desde o Preparar desta versao) = espera todos os pipelines, uma vez.
    // LD_PREWARM_GATE=area desliga (volta ao comportamento da Fase 72 mesmo com o driver novo).
    const std::string stamp = render::VulkanRenderer::Instance().DriverStamp();
    const std::filesystem::path stamp_file = DriverStampFile();
    bool all = prewarm_only;
    if (!all && !stamp.empty() && !stamp_file.empty()) {
        const char* gate = std::getenv("LD_PREWARM_GATE");
        std::string saved;
        if (std::FILE* f = _wfopen(stamp_file.c_str(), L"rb")) {
            char buffer[2048];
            saved.assign(buffer, std::fread(buffer, 1, sizeof(buffer), f));
            std::fclose(f);
        }
        while (!saved.empty() && (saved.back() == '\n' || saved.back() == '\r')) {
            saved.pop_back();
        }
        all = saved != stamp && !(gate && !std::strcmp(gate, "area"));
        if (all) {
            std::printf("[LOADING] GPU driver or exe path changed since all pipelines were last built (%s): building all of them once\n",
                        saved.empty() ? "no record" : "record differs");
        }
    }
    gnm::SetPrewarmAll(all);
    double last_report = -1.0;
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
        const std::string subtitle = done           ? (prewarm_only ? lang::T("PRONTO", "DONE") : lang::T("INICIANDO O JOGO", "STARTING THE GAME"))
                                     : prewarm_only ? lang::T("PREPARAR: CRIANDO TODOS OS PIPELINES", "PREPARE: BUILDING ALL PIPELINES")
                                     : all          ? lang::T("DRIVER DE VIDEO NOVO: CRIANDO TODOS OS PIPELINES", "NEW GPU DRIVER: BUILDING ALL PIPELINES")
                                                    : lang::T("PREPARANDO SHADERS E PIPELINES", "PREPARING SHADERS AND PIPELINES");
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
        if (prewarm_only && (done || seconds - last_report >= 1.0)) { // Fase 77: progresso para o preparar.py
            last_report = seconds;
            std::printf("[PREWARM] progress aot %u/%u %d pipelines %u/%u\n", st.aot_done, st.aot_total, st.aot_finished ? 1 : 0,
                        st.pipelines_done, st.pipelines_total);
            std::fflush(stdout);
        }
        if (done || seconds > max_seconds) {
            std::printf("[LOADING] loading screen: %.1f s (pack %s, %u pairs, %u of %u pipelines%s, %u of %u pack shaders)%s\n", seconds,
                        st.pack_finished ? "read" : "INCOMPLETO", st.pairs_done, st.pipelines_done, st.pipelines_total, all ? ", all" : "",
                        st.aot_done, st.aot_total, done ? "" : " - time limit reached, starting game anyway");
            if (done && all && !stamp.empty() && !stamp_file.empty()) {
                if (std::FILE* f = _wfopen(stamp_file.c_str(), L"wb")) { // so com tudo pronto: a proxima abertura volta a Fase 72
                    std::fprintf(f, "%s\n", stamp.c_str());
                    std::fclose(f);
                }
            }
            gnm::SetPrewarmAll(false);
            return done;
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
    {
        std::lock_guard lock(g_mutex);
        if (buffer_index < -1 || buffer_index >= static_cast<int>(g_buffers.size())) {
            return kErrorInvalidValue;
        }
        g_pending.push_back({buffer_index, flip_mode, flip_arg, NowMicros()});
        while (g_pending.size() > 2) {
            g_pending.pop_front();
        }
        g_status.flip_pending_num = static_cast<s32>(g_pending.size());
    }
    g_flip_cv.notify_one();
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
