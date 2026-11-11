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
#include <objidl.h>
#include <gdiplus.h>

#include <intrin.h>

#include "hle/overrides/hle_equeue.h"
#include "hle/overrides/hle_filesystem.h"
#include "hle/overrides/hle_override.h"
#include "hle/hle_log.h"
#include "runtime/crash_handler.h"
#include "runtime/hwmon.h"

namespace hle::gnm {
void WarmupStatus(char* text, size_t size); // hle_gnm.cpp
void SetButtonIcons(const char* name); // hle_gnm.cpp
void CaptureWatchdogSnapshot(const char* reason); // hle_gnm.cpp
}
namespace hle::avplayer {
std::string GetDiagnosticsSummary(bool pt);
int ForceStopActive();
}
namespace hle {
bool TogglePadKeyboardPlayer(); // hle_pad.cpp (2.0.2: F2)
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
std::atomic<bool> g_logs_overlay_open{false};
int g_achievements_selected = 0;
int g_achievements_scroll = 0;
int g_achievements_filter = 0; // 0 = all, 1 = unearned, 2 = earned
int g_achievements_visible_count = 8;
hle::log::LogFilter g_logs_filter = hle::log::LogFilter::kAll;
int g_logs_selected = 0;
int g_logs_scroll = 0;
int g_logs_visible_count = 24;
bool g_logs_auto_follow = true;
std::string g_logs_banner_msg;
int g_logs_banner_timer = 0;
int g_menu_selected = 0;
int g_save_confirm_timer = 0;
WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};

static std::vector<int> GetFilteredAchievements(int filter) {
    size_t count = 0;
    const TrophyDef* defs = GetTrophyDefs(&count);
    std::vector<int> indices;
    indices.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const bool unl = hle::trophy::TrophyManager::Instance().IsUnlocked(defs[i].id);
        if (filter == 1 && unl) continue; // unearned only
        if (filter == 2 && !unl) continue; // earned only
        indices.push_back(static_cast<int>(i));
    }
    return indices;
}

struct MenuItem {
    std::string label;
    std::vector<std::string> options;
    int selected = 0;
};

// F1 menu (2.0.2; was F2) (2.0: in English, with the options the port really has; they match the launcher's)
static std::vector<MenuItem> g_menu_items = {
    {"RESOLUTION (RESTART)", {"1280X720", "1600X900", "1920X1080", "2560X1080", "2560X1440", "3440X1440", "3840X2160"}, 2},
    {"DISPLAY MODE", {"FULLSCREEN", "WINDOWED"}, 0},
    {"ANTI-ALIASING", {"OFF", "TAA X16", "FSR 3 AA", "FSR 4 AA"}, 0},
    {"FRAME RATE (RESTART)", {"30 FPS", "60 FPS", "UNLOCKED", "UNLOCKED NO VSYNC"}, 1},
    {"FPS COUNTER (F10)", {"OFF", "ON"}, 0},
    {"CONTROLLER ICONS", {"AUTO", "PS4", "XBOX", "SWITCH", "KEYBOARD"}, 0},
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
    static const char* const kIconsAnsi[] = {"auto", "ps4", "xboxone", "wiiu", "keyboard"};
    static const wchar_t* const kIconsStr[] = {L"auto", L"ps4", L"xboxone", L"wiiu", L"keyboard"};
    const int ic = g_menu_items[5].selected;
    if (ic >= 0 && ic < 5) {
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
        g_menu_items[5].options = {"AUTO", "PS4", "XBOX", "SWITCH", "TECLADO"};
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
        else if (wcsstr(buf, L"keyboard")) g_menu_items[5].selected = 4;
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
    const bool next = !g_video_menu_open.load();
    if (next) {
        g_achievements_overlay_open = false;
        g_logs_overlay_open = false;
    }
    g_video_menu_open = next;
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
        if (g_menu_selected == 8) {
            static const char* const kPopupAnsi[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
            hle::trophy::TrophyManager::Instance().SetPopupStyle(kPopupAnsi[item.selected]);
        }
        last_nav = now;
    } else if (pressed & 0x20) { // Right
        auto& item = g_menu_items[g_menu_selected];
        item.selected = (item.selected + 1) % item.options.size();
        if (g_menu_selected == 8) {
            static const char* const kPopupAnsi[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
            hle::trophy::TrophyManager::Instance().SetPopupStyle(kPopupAnsi[item.selected]);
        }
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
    const bool next = !g_achievements_overlay_open.load();
    if (next) {
        g_video_menu_open = false;
        g_logs_overlay_open = false;
    }
    g_achievements_overlay_open = next;
    log::Tagged("ACHIEVEMENTS-MENU", "%s", g_achievements_overlay_open ? "open" : "closed");
}

bool IsAchievementsMenuOpen() {
    return g_achievements_overlay_open.load();
}

namespace {

void SetLogsBanner(const std::string& msg) {
    g_logs_banner_msg = msg;
    g_logs_banner_timer = 210;
}

void LogsSyncBounds(int count) {
    if (count <= 0) {
        g_logs_selected = 0;
        g_logs_scroll = 0;
        return;
    }
    if (g_logs_auto_follow) {
        g_logs_selected = count - 1;
        g_logs_scroll = std::max(0, count - g_logs_visible_count);
    } else {
        g_logs_selected = std::clamp(g_logs_selected, 0, count - 1);
        if (g_logs_selected < g_logs_scroll) {
            g_logs_scroll = g_logs_selected;
        }
        if (g_logs_selected >= g_logs_scroll + g_logs_visible_count) {
            g_logs_scroll = std::max(0, g_logs_selected - g_logs_visible_count + 1);
        }
        g_logs_scroll = std::clamp(g_logs_scroll, 0, std::max(0, count - g_logs_visible_count));
    }
}

void LogsMoveSelection(int delta) {
    const auto entries = hle::log::GetCapturedLogs(g_logs_filter);
    const int count = static_cast<int>(entries.size());
    if (count <= 0) {
        g_logs_selected = 0;
        g_logs_scroll = 0;
        return;
    }
    int cur = g_logs_auto_follow ? (count - 1) : std::clamp(g_logs_selected, 0, count - 1);
    cur = std::clamp(cur + delta, 0, count - 1);
    g_logs_auto_follow = (cur == count - 1 && delta > 0);
    g_logs_selected = cur;
    LogsSyncBounds(count);
}

void LogsSelectFilter(int step) {
    const int n = static_cast<int>(hle::log::LogFilter::kCount);
    const int cur = static_cast<int>(g_logs_filter);
    g_logs_filter = static_cast<hle::log::LogFilter>((cur + step + n) % n);
    g_logs_auto_follow = true;
    const auto entries = hle::log::GetCapturedLogs(g_logs_filter);
    LogsSyncBounds(static_cast<int>(entries.size()));
}

void LogsSaveReportAction() {
    hle::log::FlushCapture();
    const auto entries = hle::log::GetCapturedLogs(g_logs_filter);
    const int count = static_cast<int>(entries.size());
    const hle::log::LogEntry* sel = nullptr;
    if (count > 0) {
        const int idx = g_logs_auto_follow ? (count - 1) : std::clamp(g_logs_selected, 0, count - 1);
        sel = &entries[idx];
    }
    const std::string saved_path = hle::log::SaveDiagnosticReport(g_logs_filter, sel, true, lang::Pt());
    SetLogsBanner(lang::Pt() ? ("RELATORIO SALVO EM " + saved_path + " E COPIADO!") : ("SAVED TO " + saved_path + " & COPIED!"));
}

void LogsSnapshotAction() {
    hle::gnm::CaptureWatchdogSnapshot("MANUAL SNAPSHOT (F9)");
    hle::log::FlushCapture();
    SetLogsBanner(lang::Pt() ? "SNAPSHOT DE THREADS, VOZES E AVPLAYER GRAVADO NO LOG!" : "THREAD, VOICE & AVPLAYER SNAPSHOT CAPTURED TO LOG!");
}

void LogsForceStopAvAction() {
    const int n = hle::avplayer::ForceStopActive();
    hle::log::FlushCapture();
    if (n > 0) {
        SetLogsBanner(lang::Pt() ? "PARADA FORCADA ENVIADA AO AVPLAYER ATIVO!" : "FORCE-STOP QUEUED FOR ACTIVE AVPLAYER!");
    } else {
        SetLogsBanner(lang::Pt() ? "NENHUM VIDEO AVPLAYER ATIVO NO MOMENTO." : "NO ACTIVE AVPLAYER INSTANCE RUNNING.");
    }
}

} // namespace

void ToggleLogsMenu() {
    const bool next = !g_logs_overlay_open.load();
    if (next) {
        g_video_menu_open = false;
        g_achievements_overlay_open = false;
        hle::log::FlushCapture();
        g_logs_auto_follow = true;
        const auto entries = hle::log::GetCapturedLogs(g_logs_filter);
        LogsSyncBounds(static_cast<int>(entries.size()));
    }
    g_logs_overlay_open = next;
}

bool IsLogsMenuOpen() {
    return g_logs_overlay_open.load();
}

void HandleLogsMenuController(u32 pressed) {
    if (!g_logs_overlay_open.load()) return;
    static auto last_nav = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float, std::milli>(now - last_nav).count() < 150.0f) {
        return;
    }
    if (pressed & 0x10) { // Up
        LogsMoveSelection(-1);
        last_nav = now;
    } else if (pressed & 0x40) { // Down
        LogsMoveSelection(1);
        last_nav = now;
    } else if (pressed & 0x80) { // Left -> Page Up
        LogsMoveSelection(-g_logs_visible_count);
        last_nav = now;
    } else if (pressed & 0x20) { // Right -> Page Down
        LogsMoveSelection(g_logs_visible_count);
        last_nav = now;
    } else if (pressed & 0x400) { // LB
        LogsSelectFilter(-1);
        last_nav = now;
    } else if (pressed & 0x800) { // RB
        LogsSelectFilter(1);
        last_nav = now;
    } else if (pressed & 0x4000) { // Cross/A -> Save + Copy
        LogsSaveReportAction();
        last_nav = now;
    } else if (pressed & 0x8000) { // Square/X -> Snapshot
        LogsSnapshotAction();
        last_nav = now;
    } else if (pressed & 0x1000) { // Triangle/Y -> Pause/Resume
        const bool next = !hle::log::IsCapturePaused();
        hle::log::SetCapturePaused(next);
        SetLogsBanner(next ? (lang::Pt() ? "CAPTURA PAUSADA." : "CAPTURE PAUSED.") : (lang::Pt() ? "CAPTURA RETOMADA." : "CAPTURE RESUMED."));
        last_nav = now;
    } else if (pressed & (0x2000 | 0x8)) { // Circle/B or Options -> Close
        g_logs_overlay_open = false;
        last_nav = now;
    }
}

void HandleAchievementsMenuController(u32 pressed) {
    if (!g_achievements_overlay_open) return;
    static auto last_nav = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float, std::milli>(now - last_nav).count() < 160.0f) {
        return;
    }

    const auto filtered = GetFilteredAchievements(g_achievements_filter);
    const int total_items = static_cast<int>(filtered.size());

    // kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80
    // kL1 = 0x400 (LB), kR1 = 0x800 (RB)
    // kCircle = 0x2000 (B), kCross = 0x4000 (A), kSquare = 0x8000 (X), kOptions = 0x8
    if (pressed & 0x10) { // Up
        if (g_achievements_selected > 0) --g_achievements_selected;
        last_nav = now;
    } else if (pressed & 0x40) { // Down
        if (g_achievements_selected + 1 < total_items) ++g_achievements_selected;
        last_nav = now;
    } else if (pressed & 0x400) { // LB: Page Up
        g_achievements_selected = std::max(0, g_achievements_selected - g_achievements_visible_count);
        last_nav = now;
    } else if (pressed & 0x800) { // RB: Page Down
        g_achievements_selected = std::min(std::max(0, total_items - 1), g_achievements_selected + g_achievements_visible_count);
        last_nav = now;
    } else if (pressed & 0x8000) { // Square / X: Cycle Filter
        g_achievements_filter = (g_achievements_filter + 1) % 3;
        g_achievements_selected = 0;
        g_achievements_scroll = 0;
        last_nav = now;
    } else if (pressed & (0x2000 | 0x8)) { // Circle / B or Options: Close
        g_achievements_overlay_open = false;
        last_nav = now;
    }

    if (g_achievements_selected < g_achievements_scroll) {
        g_achievements_scroll = g_achievements_selected;
    }
    if (g_achievements_selected >= g_achievements_scroll + g_achievements_visible_count) {
        g_achievements_scroll = std::max(0, g_achievements_selected - g_achievements_visible_count + 1);
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
        if (wparam == VK_F1 && !(lparam & (1 << 30))) { // 2.0.2: era F2 (agora o F2 e o do jogador 2, como nos jogos LEGO de PC)
            ToggleVideoSettingsMenu();
            return 0;
        }
        if (wparam == VK_F2 && !(lparam & (1 << 30)) && !g_video_menu_open.load()) {
            TogglePadKeyboardPlayer();
            return 0;
        }
        if (wparam == VK_F8 && !(lparam & (1 << 30))) {
            ToggleAchievementsMenu();
            return 0;
        }
        if (wparam == VK_F9 && !(lparam & (1 << 30))) {
            ToggleLogsMenu();
            return 0;
        }
        if (g_logs_overlay_open.load()) {
            const bool fresh = !(lparam & (1 << 30));
            if (wparam == VK_UP) {
                LogsMoveSelection(-1);
            } else if (wparam == VK_DOWN) {
                LogsMoveSelection(1);
            } else if (wparam == VK_PRIOR) {
                LogsMoveSelection(-g_logs_visible_count);
            } else if (wparam == VK_NEXT) {
                LogsMoveSelection(g_logs_visible_count);
            } else if (wparam == VK_HOME && fresh) {
                g_logs_auto_follow = false;
                g_logs_selected = 0;
                g_logs_scroll = 0;
            } else if (wparam == VK_END && fresh) {
                g_logs_auto_follow = true;
                LogsSyncBounds(static_cast<int>(hle::log::GetCapturedLogs(g_logs_filter).size()));
            } else if ((wparam == VK_TAB || wparam == VK_RIGHT) && fresh) {
                LogsSelectFilter(1);
            } else if (wparam == VK_LEFT && fresh) {
                LogsSelectFilter(-1);
            } else if (wparam == 'F' && fresh) {
                g_logs_auto_follow = !g_logs_auto_follow;
                if (g_logs_auto_follow) {
                    LogsSyncBounds(static_cast<int>(hle::log::GetCapturedLogs(g_logs_filter).size()));
                }
                SetLogsBanner(g_logs_auto_follow
                    ? (lang::Pt() ? "AUTO-ROLAGEM (AO VIVO) LIGADA." : "AUTO-FOLLOW (LIVE) ON.")
                    : (lang::Pt() ? "AUTO-ROLAGEM DESLIGADA." : "AUTO-FOLLOW OFF."));
            } else if (wparam == 'P' && fresh) {
                const bool next = !hle::log::IsCapturePaused();
                hle::log::SetCapturePaused(next);
                SetLogsBanner(next ? (lang::Pt() ? "CAPTURA DE LOGS PAUSADA." : "LOG CAPTURE PAUSED.") : (lang::Pt() ? "CAPTURA DE LOGS RETOMADA." : "LOG CAPTURE RESUMED."));
            } else if (wparam == 'C' && fresh) {
                hle::log::ClearCapture();
                g_logs_selected = 0;
                g_logs_scroll = 0;
                SetLogsBanner(lang::Pt() ? "PAINEL LIMPO (ARQUIVO DE SESSAO PRESERVADO)." : "PANEL CLEARED (SESSION LOG FILE KEPT).");
            } else if (wparam == 'V' && fresh) {
                hle::log::g_verbose = !hle::log::g_verbose;
                SetLogsBanner(hle::log::g_verbose
                    ? (lang::Pt() ? "MODO VERBOSE HLE ATIVADO (MAIS DETALHES)." : "VERBOSE HLE ENABLED.")
                    : (lang::Pt() ? "MODO VERBOSE HLE DESATIVADO." : "VERBOSE HLE DISABLED."));
            } else if (wparam == 'D' && fresh) {
                LogsSnapshotAction();
            } else if (wparam == 'X' && fresh) {
                LogsForceStopAvAction();
            } else if ((wparam == 'S' || wparam == VK_RETURN) && fresh) {
                LogsSaveReportAction();
            } else if (wparam == VK_ESCAPE && fresh) {
                g_logs_overlay_open = false;
            }
            return 0;
        }
        if (g_achievements_overlay_open.load()) {
            const auto filtered = GetFilteredAchievements(g_achievements_filter);
            const int total_items = static_cast<int>(filtered.size());
            if (wparam == VK_UP) {
                if (g_achievements_selected > 0) --g_achievements_selected;
            } else if (wparam == VK_DOWN) {
                if (g_achievements_selected + 1 < total_items) ++g_achievements_selected;
            } else if (wparam == VK_PRIOR) {
                g_achievements_selected = std::max(0, g_achievements_selected - g_achievements_visible_count);
            } else if (wparam == VK_NEXT) {
                g_achievements_selected = std::min(std::max(0, total_items - 1), g_achievements_selected + g_achievements_visible_count);
            } else if (wparam == 'X' || wparam == VK_TAB) {
                g_achievements_filter = (g_achievements_filter + 1) % 3;
                g_achievements_selected = 0;
                g_achievements_scroll = 0;
            } else if (wparam == VK_ESCAPE) {
                g_achievements_overlay_open = false;
            }
            if (g_achievements_selected < g_achievements_scroll) g_achievements_scroll = g_achievements_selected;
            if (g_achievements_selected >= g_achievements_scroll + g_achievements_visible_count) g_achievements_scroll = std::max(0, g_achievements_selected - g_achievements_visible_count + 1);
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
                if (g_menu_selected == 8) {
                    static const char* const kPopupAnsi[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
                    hle::trophy::TrophyManager::Instance().SetPopupStyle(kPopupAnsi[item.selected]);
                }
                return 0;
            }
            if (wparam == VK_RIGHT) {
                auto& item = g_menu_items[g_menu_selected];
                item.selected = (item.selected + 1) % item.options.size();
                if (g_menu_selected == 8) {
                    static const char* const kPopupAnsi[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
                    hle::trophy::TrophyManager::Instance().SetPopupStyle(kPopupAnsi[item.selected]);
                }
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
    case WM_MOUSEWHEEL: {
        const short delta = static_cast<short>(HIWORD(wparam));
        const int steps = delta > 0 ? -3 : (delta < 0 ? 3 : 0);
        if (steps != 0 && g_logs_overlay_open.load()) {
            LogsMoveSelection(steps);
            return 0;
        }
        if (steps != 0 && g_achievements_overlay_open.load()) {
            const auto filtered = GetFilteredAchievements(g_achievements_filter);
            const int total_items = static_cast<int>(filtered.size());
            g_achievements_selected = std::clamp(g_achievements_selected + steps, 0, std::max(0, total_items - 1));
            if (g_achievements_selected < g_achievements_scroll) g_achievements_scroll = g_achievements_selected;
            if (g_achievements_selected >= g_achievements_scroll + g_achievements_visible_count) {
                g_achievements_scroll = std::max(0, g_achievements_selected - g_achievements_visible_count + 1);
            }
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
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
        hle::log::StopCapture();
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
    static const u8 kPipe[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const u8 kBackslash[7] = {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01};
    static const u8 kHash[7] = {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00};
    static const u8 kAmp[7] = {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D};
    static const u8 kSemi[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08};
    static const u8 kTilde[7] = {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00};
    static const u8 kAt[7] = {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E};
    static const u8 kDollar[7] = {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04};
    static const u8 kLeftBrace[7] = {0x06, 0x08, 0x08, 0x10, 0x08, 0x08, 0x06};
    static const u8 kRightBrace[7] = {0x0C, 0x02, 0x02, 0x01, 0x02, 0x02, 0x0C};

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
    case '\\': return kBackslash;
    case '%': return kPercent;
    case ':': return kColon;
    case ';': return kSemi;
    case '.': return kDot;
    case '-': return kDash;
    case '|': return kPipe;
    case '#': return kHash;
    case '&': return kAmp;
    case '~': return kTilde;
    case '@': return kAt;
    case '$': return kDollar;
    case '{': return kLeftBrace;
    case '}': return kRightBrace;
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

constexpr u32 Rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

struct TrophyThumbnail {
    static constexpr int kSize = 28; // crisp 28x28 downscaled PNG thumbnail
    u32 pixels[kSize * kSize] = {};
    bool loaded = false;
    bool tried = false;
};

static TrophyThumbnail g_trophy_thumbs[129]; // 0..127 = trophies, 128 = ICON0.PNG
static bool g_gdiplus_inited = false;
static std::mutex g_thumb_mutex;

static const TrophyThumbnail* GetTrophyThumbnail(int slot_id, const char* icon_name) {
    if (!icon_name || !*icon_name) return nullptr;
    const int idx = (slot_id < 0) ? 128 : slot_id;
    if (idx < 0 || idx >= 129) return nullptr;

    std::lock_guard lock(g_thumb_mutex);
    auto& thumb = g_trophy_thumbs[idx];
    if (thumb.loaded) return &thumb;
    if (thumb.tried) return nullptr;

    const auto root = hle::trophy::TrophyManager::Instance().GetRootPath();
    if (root.empty()) return nullptr;
    thumb.tried = true;

    if (!g_gdiplus_inited) {
        static ULONG_PTR s_token = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&s_token, &input, nullptr);
        g_gdiplus_inited = true;
    }

    const std::filesystem::path icon_paths[] = {
        root / "assets" / "trophy" / "NPWR07423_00" / "Icons" / icon_name,
        root / "assets" / "trophy" / icon_name,
        root / "assets" / "trophy" / "Icons" / icon_name
    };

    for (const auto& p : icon_paths) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue;

        Gdiplus::Bitmap bmp(p.c_str());
        if (bmp.GetLastStatus() != Gdiplus::Ok) continue;

        Gdiplus::Bitmap small_bmp(TrophyThumbnail::kSize, TrophyThumbnail::kSize, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&small_bmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&bmp, 0, 0, TrophyThumbnail::kSize, TrophyThumbnail::kSize);

        for (int y = 0; y < TrophyThumbnail::kSize; ++y) {
            for (int x = 0; x < TrophyThumbnail::kSize; ++x) {
                Gdiplus::Color c;
                small_bmp.GetPixel(x, y, &c);
                thumb.pixels[y * TrophyThumbnail::kSize + x] = Rgba(c.GetR(), c.GetG(), c.GetB(), c.GetA());
            }
        }
        thumb.loaded = true;
        return &thumb;
    }
    return nullptr;
}

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
        const float box_w = x1 - x0;
        const float box_h = y1 - y0;
        if (box_w <= 0.0f || box_h <= 0.0f) return;
        if (box_w >= box_h) {
            float r = box_h * 0.5f;
            Rect(x0 + r, y0, x1 - r, y1, rgba);
            const int steps = static_cast<int>(r);
            for (int dy = -steps; dy <= steps; ++dy) {
                float fdy = static_cast<float>(dy);
                float dx = std::sqrt(std::max(0.0f, r * r - fdy * fdy));
                Rect(x0 + r - dx, y0 + r + fdy, x0 + r, y0 + r + fdy + 1.0f, rgba);
                Rect(x1 - r, y0 + r + fdy, x1 - r + dx, y0 + r + fdy + 1.0f, rgba);
            }
        } else {
            float r = box_w * 0.5f;
            Rect(x0, y0 + r, x1, y1 - r, rgba);
            const int steps = static_cast<int>(r);
            for (int dx = -steps; dx <= steps; ++dx) {
                float fdx = static_cast<float>(dx);
                float dy = std::sqrt(std::max(0.0f, r * r - fdx * fdx));
                Rect(x0 + r + fdx, y0 + r - dy, x0 + r + fdx + 1.0f, y0 + r, rgba);
                Rect(x0 + r + fdx, y1 - r, x0 + r + fdx + 1.0f, y1 - r + dy, rgba);
            }
        }
    }
    void DrawThumbnail(const TrophyThumbnail* thumb, float x0, float y0, float size, float brightness = 1.0f) {
        if (!thumb || !thumb->loaded) return;
        const float px = size / static_cast<float>(TrophyThumbnail::kSize);
        for (int y = 0; y < TrophyThumbnail::kSize; ++y) {
            int x = 0;
            while (x < TrophyThumbnail::kSize) {
                u32 col = thumb->pixels[y * TrophyThumbnail::kSize + x];
                u8 a = static_cast<u8>((col >> 24) & 0xFF);
                if (a < 16) {
                    ++x;
                    continue;
                }
                int end_x = x + 1;
                while (end_x < TrophyThumbnail::kSize) {
                    if (thumb->pixels[y * TrophyThumbnail::kSize + end_x] != col) break;
                    ++end_x;
                }
                u8 r = static_cast<u8>(col & 0xFF);
                u8 g = static_cast<u8>((col >> 8) & 0xFF);
                u8 b = static_cast<u8>((col >> 16) & 0xFF);
                if (brightness < 0.99f) {
                    r = static_cast<u8>(r * brightness);
                    g = static_cast<u8>(g * brightness);
                    b = static_cast<u8>(b * brightness);
                }
                Rect(x0 + x * px, y0 + y * px, x0 + end_x * px, y0 + (y + 1) * px, Rgba(r, g, b, a));
                x = end_x;
            }
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
    void DrawBitmap(const u8* rows, int num_rows, float x, float y, float pixel, u32 rgba) {
        for (int r = 0; r < num_rows; ++r) {
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
    }
};

constexpr u8 kTrophyCup[7] = {0x1F, 0x1F, 0x0E, 0x04, 0x04, 0x0E, 0x1F};
constexpr u8 kPadlock[5] = {0x0E, 0x0A, 0x1F, 0x1F, 0x1F};
constexpr u8 kTriangleUp[3] = {0x04, 0x0E, 0x1F};

static void RenderTrophyToast(OverlayCanvas& c, const RECT& client, float scale_ui,
                              const std::string& style, int trophy_id, char type,
                              const std::string& title, const std::string& desc,
                              float anim, bool is_preview) {
    u32 tier_col = Rgba(205, 127, 50); // Bronze
    const char* tier_en = "BRONZE";
    const char* tier_pt = "BRONZE";
    if (type == 'P') {
        tier_col = Rgba(0, 229, 255); // Platinum
        tier_en = "PLATINUM";
        tier_pt = "PLATINA";
    } else if (type == 'G') {
        tier_col = Rgba(255, 215, 0); // Gold
        tier_en = "GOLD";
        tier_pt = "OURO";
    } else if (type == 'S') {
        tier_col = Rgba(208, 212, 224); // Silver
        tier_en = "SILVER";
        tier_pt = "PRATA";
    }
    const char* tier_name = lang::Pt() ? tier_pt : tier_en;
    const TrophyDef* tdef = hle::trophy::TrophyManager::Instance().FindDef(trophy_id);
    const TrophyThumbnail* thumb = GetTrophyThumbnail(trophy_id >= 0 ? trophy_id : 16, tdef ? tdef->icon : "TROP016.PNG");

    if (style == "xbox") {
        const float box_w = 480.0f * scale_ui;
        const float box_h = 66.0f * scale_ui;
        const float cur_x = (client.right - box_w) * 0.5f;
        const float cur_y = is_preview ? (24.0f * scale_ui) : (24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui));

        c.Pill(cur_x, cur_y + 3.0f * scale_ui, cur_x + box_w, cur_y + box_h + 3.0f * scale_ui, Rgba(0, 0, 0, 102));
        c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, Rgba(16, 124, 16));

        const float cr = (box_h - 16.0f * scale_ui) * 0.5f;
        const float cx = cur_x + 14.0f * scale_ui + cr;
        const float cy = cur_y + box_h * 0.5f;
        c.Circle(cx, cy, cr, Rgba(12, 98, 12));

        const float cup_p = scale_ui * 2.0f;
        c.DrawBitmap(kTrophyCup, 7, cx - 2.5f * cup_p, cy - 3.5f * cup_p, cup_p, Rgba(255, 255, 255));

        const float text_x = cx + cr + 14.0f * scale_ui;
        std::string hdr = lang::Pt() ? "Conquista desbloqueada" : "Achievement unlocked";
        c.Write(hdr, text_x, cur_y + 11.0f * scale_ui, 1.8f * scale_ui, Rgba(255, 255, 255));
        c.Write(title, text_x, cur_y + 35.0f * scale_ui, 1.6f * scale_ui, Rgba(216, 255, 216));

        if (is_preview) {
            const float lbl_w = 210.0f * scale_ui;
            const float lbl_x = cur_x + (box_w - lbl_w) * 0.5f;
            const float lbl_y = cur_y + box_h + 8.0f * scale_ui;
            c.Pill(lbl_x, lbl_y, lbl_x + lbl_w, lbl_y + 22.0f * scale_ui, Rgba(16, 20, 30, 238));
            std::string p_txt = lang::Pt() ? "[ PRE-VISUALIZACAO: XBOX ]" : "[ PREVIEW: XBOX ]";
            c.Write(p_txt, lbl_x + 10.0f * scale_ui, lbl_y + 5.0f * scale_ui, 1.25f * scale_ui, Rgba(80, 220, 80));
        }
    } else if (style == "xbox360") {
        const float box_w = 460.0f * scale_ui;
        const float box_h = 64.0f * scale_ui;
        const float cur_x = (client.right - box_w) * 0.5f;
        const float cur_y = is_preview ? (24.0f * scale_ui) : (24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui));

        c.Pill(cur_x - 1.5f * scale_ui, cur_y - 1.5f * scale_ui, cur_x + box_w + 1.5f * scale_ui, cur_y + box_h + 1.5f * scale_ui, Rgba(80, 80, 84));
        c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, Rgba(28, 28, 31, 242));

        const float cr = (box_h - 16.0f * scale_ui) * 0.5f;
        const float cx = cur_x + 14.0f * scale_ui + cr;
        const float cy = cur_y + box_h * 0.5f;
        c.Circle(cx, cy, cr, Rgba(53, 53, 56));
        c.Circle(cx, cy, cr - 3.0f * scale_ui, Rgba(125, 125, 133));

        for (float dy = -cr + 1.0f; dy <= 0.0f; dy += 1.0f) {
            float dx = std::sqrt(std::max(0.0f, (cr - 1.0f) * (cr - 1.0f) - dy * dy));
            c.Rect(cx - dx, cy + dy, cx, cy + dy + 1.0f, Rgba(82, 176, 34));
        }

        const float text_x = cx + cr + 14.0f * scale_ui;
        std::string hdr = lang::Pt() ? "Conquista desbloqueada" : "Achievement unlocked";
        c.Write(hdr, text_x, cur_y + 11.0f * scale_ui, 1.8f * scale_ui, Rgba(224, 224, 224));
        c.Write(title, text_x, cur_y + 34.0f * scale_ui, 1.6f * scale_ui, Rgba(255, 255, 255));

        if (is_preview) {
            const float lbl_w = 230.0f * scale_ui;
            const float lbl_x = cur_x + (box_w - lbl_w) * 0.5f;
            const float lbl_y = cur_y + box_h + 8.0f * scale_ui;
            c.Pill(lbl_x, lbl_y, lbl_x + lbl_w, lbl_y + 22.0f * scale_ui, Rgba(16, 20, 30, 238));
            std::string p_txt = lang::Pt() ? "[ PRE-VISUALIZACAO: XBOX 360 ]" : "[ PREVIEW: XBOX 360 ]";
            c.Write(p_txt, lbl_x + 10.0f * scale_ui, lbl_y + 5.0f * scale_ui, 1.25f * scale_ui, Rgba(82, 176, 34));
        }
    } else if (style == "ps4") {
        const float box_w = 460.0f * scale_ui;
        const float box_h = 72.0f * scale_ui;
        const float cur_x = client.right - box_w - 24.0f * scale_ui;
        const float cur_y = is_preview ? (24.0f * scale_ui) : (24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui));

        c.Rect(cur_x - 2.0f * scale_ui, cur_y - 2.0f * scale_ui, cur_x + box_w + 2.0f * scale_ui, cur_y + box_h + 2.0f * scale_ui, Rgba(54, 56, 66));
        c.Rect(cur_x, cur_y, cur_x + box_w, cur_y + box_h, Rgba(22, 24, 30, 238));

        const float badge_sz = box_h - 16.0f * scale_ui;
        const float badge_x = cur_x + 8.0f * scale_ui;
        const float badge_y = cur_y + 8.0f * scale_ui;
        c.Rect(badge_x, badge_y, badge_x + badge_sz, badge_y + badge_sz, Rgba(36, 38, 48));

        if (thumb && thumb->loaded) {
            c.DrawThumbnail(thumb, badge_x + 2.0f * scale_ui, badge_y + 2.0f * scale_ui, badge_sz - 4.0f * scale_ui, 1.0f);
        } else {
            const float cup_p = scale_ui * 2.2f;
            c.DrawBitmap(kTrophyCup, 7, badge_x + (badge_sz - 5.0f * cup_p) * 0.5f, badge_y + (badge_sz - 7.0f * cup_p) * 0.5f, cup_p, tier_col);
        }

        const float text_x = badge_x + badge_sz + 12.0f * scale_ui;
        c.Write(title, text_x, cur_y + 14.0f * scale_ui, 2.0f * scale_ui, Rgba(255, 255, 255));
        const float mini_p = scale_ui * 1.4f;
        c.DrawBitmap(kTrophyCup, 7, text_x, cur_y + 40.0f * scale_ui, mini_p, tier_col);
        std::string sub = lang::Pt() ? "Trofeu conquistado!" : "Trophy earned!";
        c.Write(sub, text_x + 12.0f * scale_ui, cur_y + 40.0f * scale_ui, 1.4f * scale_ui, Rgba(160, 160, 178));

        if (is_preview) {
            const float lbl_w = 200.0f * scale_ui;
            const float lbl_x = cur_x + (box_w - lbl_w) * 0.5f;
            const float lbl_y = cur_y + box_h + 8.0f * scale_ui;
            c.Pill(lbl_x, lbl_y, lbl_x + lbl_w, lbl_y + 22.0f * scale_ui, Rgba(16, 20, 30, 238));
            std::string p_txt = lang::Pt() ? "[ PRE-VISUALIZACAO: PS4 ]" : "[ PREVIEW: PS4 ]";
            c.Write(p_txt, lbl_x + 10.0f * scale_ui, lbl_y + 5.0f * scale_ui, 1.25f * scale_ui, Rgba(0, 162, 255));
        }
    } else if (style == "steam") {
        const float box_w = 420.0f * scale_ui;
        const float box_h = 72.0f * scale_ui;
        const float cur_x = client.right - box_w - 24.0f * scale_ui;
        const float cur_y = is_preview ? (client.bottom - box_h - 24.0f * scale_ui) : (client.bottom - box_h - 24.0f * scale_ui + (1.0f - anim) * (box_h + 35.0f * scale_ui));

        c.Rect(cur_x - 2.0f * scale_ui, cur_y - 2.0f * scale_ui, cur_x + box_w + 2.0f * scale_ui, cur_y + box_h + 2.0f * scale_ui, Rgba(43, 71, 93));
        c.Rect(cur_x, cur_y, cur_x + box_w, cur_y + box_h, Rgba(23, 26, 33, 250));

        const float badge_sz = box_h - 16.0f * scale_ui;
        const float badge_x = cur_x + 8.0f * scale_ui;
        const float badge_y = cur_y + 8.0f * scale_ui;
        c.Rect(badge_x, badge_y, badge_x + badge_sz, badge_y + badge_sz, Rgba(14, 20, 27));

        if (thumb && thumb->loaded) {
            c.DrawThumbnail(thumb, badge_x + 2.0f * scale_ui, badge_y + 2.0f * scale_ui, badge_sz - 4.0f * scale_ui, 1.0f);
        } else {
            const float cup_p = scale_ui * 2.2f;
            c.DrawBitmap(kTrophyCup, 7, badge_x + (badge_sz - 5.0f * cup_p) * 0.5f, badge_y + (badge_sz - 7.0f * cup_p) * 0.5f, cup_p, Rgba(102, 192, 244));
        }

        const float text_x = badge_x + badge_sz + 12.0f * scale_ui;
        std::string hdr = lang::Pt() ? "Steam - Conquista Desbloqueada" : "Steam - Achievement Unlocked";
        c.Write(hdr, text_x, cur_y + 9.0f * scale_ui, 1.4f * scale_ui, Rgba(102, 192, 244));
        c.Write(title, text_x, cur_y + 26.0f * scale_ui, 1.8f * scale_ui, Rgba(255, 255, 255));
        c.Write(desc, text_x, cur_y + 48.0f * scale_ui, 1.2f * scale_ui, Rgba(143, 152, 160));

        if (is_preview) {
            const float lbl_w = 210.0f * scale_ui;
            const float lbl_x = cur_x + (box_w - lbl_w) * 0.5f;
            const float lbl_y = cur_y - 26.0f * scale_ui;
            c.Pill(lbl_x, lbl_y, lbl_x + lbl_w, lbl_y + 22.0f * scale_ui, Rgba(16, 20, 30, 238));
            std::string p_txt = lang::Pt() ? "[ PRE-VISUALIZACAO: STEAM ]" : "[ PREVIEW: STEAM ]";
            c.Write(p_txt, lbl_x + 10.0f * scale_ui, lbl_y + 5.0f * scale_ui, 1.25f * scale_ui, Rgba(102, 192, 244));
        }
    } else {
        // PS5 style (qualified tier)
        const float box_w = 500.0f * scale_ui;
        const float box_h = 78.0f * scale_ui;
        const float cur_x = (client.right - box_w) * 0.5f;
        const float cur_y = is_preview ? (24.0f * scale_ui) : (24.0f * scale_ui - (1.0f - anim) * (box_h + 35.0f * scale_ui));

        c.Pill(cur_x - 2.5f * scale_ui, cur_y - 2.5f * scale_ui, cur_x + box_w + 2.5f * scale_ui, cur_y + box_h + 2.5f * scale_ui, tier_col);
        c.Pill(cur_x, cur_y, cur_x + box_w, cur_y + box_h, Rgba(15, 18, 24, 244));

        // Left vertical accent pill strictly inside rounded cap
        c.Pill(cur_x + 15.0f * scale_ui, cur_y + 18.0f * scale_ui, cur_x + 19.0f * scale_ui, cur_y + box_h - 18.0f * scale_ui, tier_col);

        const float badge_sz = box_h - 22.0f * scale_ui;
        const float bx = cur_x + 26.0f * scale_ui;
        const float by = cur_y + 11.0f * scale_ui;
        c.Rect(bx - 1.0f, by - 1.0f, bx + badge_sz + 1.0f, by + badge_sz + 1.0f, tier_col);
        c.Rect(bx, by, bx + badge_sz, by + badge_sz, Rgba(22, 26, 36));
        if (thumb && thumb->loaded) {
            c.DrawThumbnail(thumb, bx + 2.0f * scale_ui, by + 2.0f * scale_ui, badge_sz - 4.0f * scale_ui, 1.0f);
        } else {
            const float cup_p = scale_ui * 2.2f;
            c.DrawBitmap(kTrophyCup, 7, bx + (badge_sz - 5.0f * cup_p) * 0.5f, by + (badge_sz - 7.0f * cup_p) * 0.5f, cup_p, tier_col);
        }

        const float text_x = bx + badge_sz + 14.0f * scale_ui;
        const float mini_p = scale_ui * 1.4f;
        c.DrawBitmap(kTrophyCup, 7, text_x, cur_y + 10.0f * scale_ui, mini_p, tier_col);
        std::string hdr = std::string(lang::Pt() ? "TROFEU DE " : "") + tier_name + (lang::Pt() ? " CONQUISTADO!" : " TROPHY EARNED!");
        c.Write(hdr, text_x + 12.0f * scale_ui, cur_y + 10.0f * scale_ui, 1.5f * scale_ui, tier_col);
        c.Write(title, text_x, cur_y + 29.0f * scale_ui, 2.0f * scale_ui, Rgba(255, 255, 255));
        c.Write(desc, text_x, cur_y + 53.0f * scale_ui, 1.3f * scale_ui, Rgba(170, 175, 185));

        if (is_preview) {
            const float lbl_w = 260.0f * scale_ui;
            const float lbl_x = cur_x + (box_w - lbl_w) * 0.5f;
            const float lbl_y = cur_y + box_h + 8.0f * scale_ui;
            c.Pill(lbl_x, lbl_y, lbl_x + lbl_w, lbl_y + 22.0f * scale_ui, Rgba(16, 20, 30, 238));
            std::string p_txt = lang::Pt() ? "[ PRE-VISUALIZACAO: PS5 ]" : "[ PREVIEW: PS5 ]";
            c.Write(p_txt, lbl_x + 12.0f * scale_ui, lbl_y + 5.0f * scale_ui, 1.25f * scale_ui, tier_col);
        }
    }
}

static std::vector<std::string> WrapText(const std::string& text, size_t max_chars, size_t max_lines) {
    std::vector<std::string> lines;
    if (text.empty() || max_chars == 0 || max_lines == 0) return lines;
    size_t pos = 0;
    while (pos < text.size() && lines.size() < max_lines) {
        if (text.size() - pos <= max_chars) {
            lines.push_back(text.substr(pos));
            pos = text.size();
            break;
        }
        if (lines.size() + 1 == max_lines) {
            std::string tail = text.substr(pos, max_chars > 3 ? max_chars - 3 : max_chars);
            if (max_chars > 3) tail += "...";
            lines.push_back(std::move(tail));
            break;
        }
        size_t cut = text.rfind(' ', pos + max_chars);
        if (cut == std::string::npos || cut <= pos + max_chars / 2) {
            cut = pos + max_chars;
            lines.push_back(text.substr(pos, cut - pos));
            pos = cut;
        } else {
            lines.push_back(text.substr(pos, cut - pos));
            pos = cut + 1;
        }
    }
    return lines;
}

static void RenderLogsOverlay(OverlayCanvas& c, const RECT& client) {
    const float scale_ui = client.bottom >= 1400 ? 1.35f : (client.bottom >= 1000 ? 1.0f : 0.75f);
    const bool pt = lang::Pt();

    const float pad_x = client.right * 0.03f;
    const float pad_y = client.bottom * 0.035f;
    const float box_x0 = pad_x;
    const float box_x1 = client.right - pad_x;
    const float box_y0 = pad_y;
    const float box_y1 = client.bottom - pad_y;
    const float box_w = box_x1 - box_x0;

    // Backdrop & outer frame
    c.Rect(0, 0, client.right, client.bottom, Rgba(6, 9, 14, 240));
    c.Rect(box_x0 - 2.0f, box_y0 - 2.0f, box_x1 + 2.0f, box_y1 + 2.0f, Rgba(56, 130, 210, 220));
    c.Rect(box_x0, box_y0, box_x1, box_y1, Rgba(13, 16, 23, 248));

    hle::log::LogStats stats{};
    hle::log::GetCaptureStats(&stats);
    const auto entries = hle::log::GetCapturedLogs(g_logs_filter);
    const int count = static_cast<int>(entries.size());

    // Header strip
    const float hdr_y = box_y0 + 12.0f * scale_ui;
    const std::string title = pt ? "PAINEL DE LOGS E DIAGNOSTICO [F9]" : "LOGS & DIAGNOSTICS PANEL [F9]";
    c.Write(title, box_x0 + 16.0f * scale_ui, hdr_y, 2.0f * scale_ui, Rgba(92, 198, 255));

    // Live counters & status pills on header right
    char cnt_buf[180];
    std::snprintf(cnt_buf, sizeof(cnt_buf),
                  pt ? "ERROS: %llu  |  AVISOS: %llu  |  CUTSCENE/AV: %llu  |  STUBS: %llu  |  LINHAS: %llu"
                     : "ERRORS: %llu  |  WARNINGS: %llu  |  CUTSCENE/AV: %llu  |  STUBS: %llu  |  LINES: %llu",
                  static_cast<unsigned long long>(stats.error_count),
                  static_cast<unsigned long long>(stats.warning_count),
                  static_cast<unsigned long long>(stats.cutscene_count),
                  static_cast<unsigned long long>(stats.stub_count),
                  static_cast<unsigned long long>(stats.total_lines));
    const float cnt_font = 1.35f * scale_ui;
    c.Write(cnt_buf, box_x0 + 16.0f * scale_ui, hdr_y + 20.0f * scale_ui, cnt_font,
            stats.error_count > 0 ? Rgba(255, 118, 118) : Rgba(180, 190, 210));

    // Badges top right: [PAUSADO / AO VIVO] [VERBOSE]
    std::string state_badge = stats.paused ? (pt ? "[PAUSADO]" : "[PAUSED]") : (g_logs_auto_follow ? (pt ? "[AO VIVO]" : "[LIVE]") : (pt ? "[INSPECIONANDO]" : "[INSPECTING]"));
    std::string verb_badge = hle::log::g_verbose ? "[VERBOSE: ON]" : "[VERBOSE: OFF]";
    const float b_font = 1.35f * scale_ui;
    const float vw = TextWidth(verb_badge, b_font);
    const float sw = TextWidth(state_badge, b_font);
    c.Write(verb_badge, box_x1 - vw - 16.0f * scale_ui, hdr_y + 2.0f * scale_ui, b_font,
            hle::log::g_verbose ? Rgba(255, 210, 75) : Rgba(130, 140, 160));
    c.Write(state_badge, box_x1 - vw - sw - 34.0f * scale_ui, hdr_y + 2.0f * scale_ui, b_font,
            stats.paused ? Rgba(255, 175, 65) : Rgba(95, 235, 145));

    // Cutscene / AVPlayer live diagnostics bar
    const float av_y0 = hdr_y + 38.0f * scale_ui;
    const float av_h = 26.0f * scale_ui;
    c.Rect(box_x0 + 14.0f * scale_ui, av_y0, box_x1 - 14.0f * scale_ui, av_y0 + av_h, Rgba(20, 26, 38, 235));
    const std::string av_diag = hle::avplayer::GetDiagnosticsSummary(pt);
    const float av_font = 1.3f * scale_ui;
    const size_t av_max_chars = static_cast<size_t>(std::max(30.0f, (box_w - 44.0f * scale_ui) / (6.0f * av_font)));
    std::string av_shown = av_diag.size() > av_max_chars ? (av_diag.substr(0, av_max_chars - 3) + "...") : av_diag;
    const bool av_has_err = (av_diag.find("ERRO:") != std::string::npos || av_diag.find("ERR:") != std::string::npos);
    c.Write(av_shown, box_x0 + 22.0f * scale_ui, av_y0 + 8.0f * scale_ui, av_font,
            av_has_err ? Rgba(255, 110, 110) : Rgba(125, 220, 255));

    // Filter tabs row
    const float tab_y0 = av_y0 + av_h + 8.0f * scale_ui;
    const float tab_h = 24.0f * scale_ui;
    const float tab_font = 1.3f * scale_ui;
    struct TabDef {
        hle::log::LogFilter id;
        std::string label;
        u32 accent;
    };
    const TabDef tabs[] = {
        {hle::log::LogFilter::kAll,      pt ? "TODAS"       : "ALL",         Rgba(110, 185, 255)},
        {hle::log::LogFilter::kErrors,   (pt ? "ERROS ("     : "ERRORS (")    + std::to_string(stats.error_count) + ")", Rgba(255, 95, 95)},
        {hle::log::LogFilter::kWarnings, (pt ? "AVISOS ("    : "WARNINGS (")  + std::to_string(stats.warning_count) + ")", Rgba(255, 195, 70)},
        {hle::log::LogFilter::kCutscene, (pt ? "CUTSCENE/AV (" : "CUTSCENE/AV (") + std::to_string(stats.cutscene_count) + ")", Rgba(80, 225, 255)},
        {hle::log::LogFilter::kStubs,    "STUBS (" + std::to_string(stats.stub_count) + ")", Rgba(215, 170, 255)},
        {hle::log::LogFilter::kGame,     pt ? "JOGO [GAME]" : "GAME [GAME]", Rgba(130, 235, 155)},
    };
    float tab_x = box_x0 + 16.0f * scale_ui;
    for (const auto& t : tabs) {
        const bool active = (t.id == g_logs_filter);
        const float tw = TextWidth(t.label, tab_font) + 18.0f * scale_ui;
        c.Pill(tab_x, tab_y0, tab_x + tw, tab_y0 + tab_h, active ? Rgba(42, 68, 108, 255) : Rgba(24, 29, 40, 220));
        if (active) {
            c.Rect(tab_x + 8.0f * scale_ui, tab_y0 + tab_h - 3.0f * scale_ui, tab_x + tw - 8.0f * scale_ui, tab_y0 + tab_h, t.accent);
        }
        c.Write(t.label, tab_x + 9.0f * scale_ui, tab_y0 + 7.0f * scale_ui, tab_font, active ? Rgba(255, 255, 255) : Rgba(155, 165, 185));
        tab_x += tw + 8.0f * scale_ui;
    }

    // Geometry for list vs bottom Inspector Box
    const float footer_y = box_y1 - 22.0f * scale_ui;
    const float insp_h = 114.0f * scale_ui;
    const float insp_y1 = footer_y - 8.0f * scale_ui;
    const float insp_y0 = insp_y1 - insp_h;
    const float list_y0 = tab_y0 + tab_h + 10.0f * scale_ui;
    const float list_y1 = insp_y0 - 8.0f * scale_ui;
    const float list_h = std::max(60.0f * scale_ui, list_y1 - list_y0);

    c.Rect(box_x0 + 14.0f * scale_ui, list_y0, box_x1 - 14.0f * scale_ui, list_y1, Rgba(10, 12, 18, 230));

    const float row_h = 20.0f * scale_ui;
    const float row_font = 1.35f * scale_ui;
    g_logs_visible_count = std::clamp(static_cast<int>(list_h / row_h), 6, 36);
    LogsSyncBounds(count);

    const float text_left = box_x0 + 22.0f * scale_ui;
    const float text_right = box_x1 - 24.0f * scale_ui;
    const size_t row_max_chars = static_cast<size_t>(std::max(30.0f, (text_right - text_left) / (6.0f * row_font)));

    for (int r = 0; r < g_logs_visible_count; ++r) {
        const int idx = g_logs_scroll + r;
        if (idx >= count) break;

        const auto& e = entries[idx];
        const bool is_sel = (idx == g_logs_selected);
        const float ry0 = list_y0 + 4.0f * scale_ui + r * row_h;
        const float ry1 = ry0 + row_h - 1.0f;

        if (is_sel) {
            c.Rect(box_x0 + 15.0f * scale_ui, ry0, box_x1 - 20.0f * scale_ui, ry1, Rgba(44, 62, 94, 240));
            c.Rect(box_x0 + 15.0f * scale_ui, ry0, box_x0 + 18.0f * scale_ui, ry1, Rgba(92, 198, 255));
        } else if (e.severity == 2) {
            c.Rect(box_x0 + 15.0f * scale_ui, ry0, box_x1 - 20.0f * scale_ui, ry1, Rgba(64, 18, 22, 130));
        } else if (e.severity == 1 && (r % 2 == 0)) {
            c.Rect(box_x0 + 15.0f * scale_ui, ry0, box_x1 - 20.0f * scale_ui, ry1, Rgba(52, 40, 14, 90));
        }

        const char* tag = e.severity == 2 ? "[ERR]" : (e.severity == 1 ? "[WRN]" : (e.is_cutscene ? "[CUT]" : "[INF]"));
        u32 col = Rgba(210, 216, 228);
        if (e.severity == 2) col = Rgba(255, 115, 115);
        else if (e.severity == 1) col = Rgba(255, 208, 90);
        else if (e.is_cutscene) col = Rgba(105, 225, 255);
        else if (e.is_game) col = Rgba(145, 232, 165);
        else if (is_sel) col = Rgba(255, 255, 255);

        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "%6.1fs %s ", e.time_s, tag);
        std::string line_str = std::string(prefix) + e.text;
        if (line_str.size() > row_max_chars) {
            line_str.resize(row_max_chars > 3 ? row_max_chars - 3 : row_max_chars);
            if (row_max_chars > 3) line_str += "...";
        }
        c.Write(line_str, text_left, ry0 + 5.0f * scale_ui, row_font, col);
    }

    if (count == 0) {
        const std::string empty_msg = pt ? "Nenhum log nesta categoria ainda." : "No log entries in this category yet.";
        c.Write(empty_msg, text_left + 12.0f * scale_ui, list_y0 + 28.0f * scale_ui, 1.6f * scale_ui, Rgba(140, 148, 168));
    }

    // Scrollbar on list right edge
    const float sb_x0 = box_x1 - 12.0f * scale_ui;
    const float sb_x1 = box_x1 - 7.0f * scale_ui;
    c.Pill(sb_x0, list_y0 + 4.0f * scale_ui, sb_x1, list_y1 - 4.0f * scale_ui, Rgba(24, 28, 38));
    if (count > g_logs_visible_count) {
        const float track_h = (list_y1 - 4.0f * scale_ui) - (list_y0 + 4.0f * scale_ui);
        const float thumb_h = std::max(24.0f * scale_ui, track_h * g_logs_visible_count / count);
        const float thumb_y0 = (list_y0 + 4.0f * scale_ui) +
                               (track_h - thumb_h) * (static_cast<float>(g_logs_scroll) / (count - g_logs_visible_count));
        c.Pill(sb_x0, thumb_y0, sb_x1, thumb_y0 + thumb_h, Rgba(92, 138, 198));
    }

    // Bottom Inspector Box: full unwrapped line + human-readable Reason / Diagnosis
    c.Rect(box_x0 + 14.0f * scale_ui, insp_y0, box_x1 - 14.0f * scale_ui, insp_y1, Rgba(18, 22, 32, 245));
    c.Rect(box_x0 + 14.0f * scale_ui, insp_y0, box_x1 - 14.0f * scale_ui, insp_y0 + 2.0f * scale_ui, Rgba(56, 130, 210));

    const float insp_font = 1.3f * scale_ui;
    const size_t insp_max_chars = static_cast<size_t>(std::max(35.0f, (box_w - 46.0f * scale_ui) / (6.0f * insp_font)));
    if (count > 0) {
        const auto& sel = entries[std::clamp(g_logs_selected, 0, count - 1)];
        const auto wrapped_raw = WrapText(sel.text, insp_max_chars, 2);
        float iy = insp_y0 + 10.0f * scale_ui;
        for (const auto& wl : wrapped_raw) {
            c.Write(wl, text_left, iy, insp_font, Rgba(240, 244, 252));
            iy += 16.0f * scale_ui;
        }
        const std::string reason = hle::log::ExplainLogEntry(sel, pt);
        const auto wrapped_reason = WrapText(reason, insp_max_chars, 2);
        float ry = insp_y0 + 50.0f * scale_ui;
        for (const auto& rl : wrapped_reason) {
            c.Write(rl, text_left, ry, insp_font, Rgba(255, 212, 92));
            ry += 16.0f * scale_ui;
        }
    } else {
        c.Write(pt ? "DICA: Use [D] para capturar threads/cutscene ou [V] para ligar Verbose HLE."
                   : "TIP: Press [D] to dump thread/cutscene snapshot or [V] for Verbose HLE.",
                text_left, insp_y0 + 16.0f * scale_ui, insp_font, Rgba(165, 175, 195));
    }

    if (g_logs_banner_timer > 0) {
        --g_logs_banner_timer;
        const float bw = TextWidth(g_logs_banner_msg, insp_font) + 20.0f * scale_ui;
        const float bx = std::max(text_left, box_x1 - 18.0f * scale_ui - bw);
        const float by = insp_y1 - 24.0f * scale_ui;
        c.Pill(bx, by, bx + bw, by + 20.0f * scale_ui, Rgba(22, 82, 44, 240));
        c.Write(g_logs_banner_msg, bx + 10.0f * scale_ui, by + 5.0f * scale_ui, insp_font, Rgba(150, 255, 175));
    }

    // Footer shortcuts
    const std::string footer = pt
        ? "[CIMA/BAIXO] ROLAR  [TAB/ESQ/DIR] FILTRO  [F] AO VIVO  [P] PAUSAR  [C] LIMPAR  [V] VERBOSE  [D] DUMP  [X] PARAR AV  [S/ENTER] SALVAR+COPIAR  [F9/ESC] FECHAR"
        : "[UP/DOWN] SCROLL  [TAB/L/R] FILTER  [F] LIVE  [P] PAUSE  [C] CLEAR  [V] VERBOSE  [D] SNAPSHOT  [X] STOP AV  [S/ENTER] SAVE+COPY  [F9/ESC] CLOSE";
    const float font_foot = 1.2f * scale_ui;
    const float foot_w = TextWidth(footer, font_foot);
    const float eff_foot = (foot_w > box_w - 24.0f * scale_ui) ? (font_foot * (box_w - 24.0f * scale_ui) / foot_w) : font_foot;
    c.Write(footer, box_x0 + (box_w - TextWidth(footer, eff_foot)) * 0.5f, footer_y, eff_foot, Rgba(130, 142, 165));
}

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
    if (g_logs_overlay_open.load()) {
        static int s_log_flush_div = 0;
        if (++s_log_flush_div >= 15) {
            s_log_flush_div = 0;
            hle::log::FlushCapture();
        }
    }
    if (!g_stats_overlay.load() && !g_video_menu_open.load() && !g_achievements_overlay_open.load() &&
        !g_logs_overlay_open.load() && !has_toast) {
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
            const std::string footer = lang::Pt() ? "[CIMA/BAIXO] NAVEGAR  [ESQ/DIR] ALTERAR  [ENTER] SALVAR  [F1/ESC] SAIR" : "[UP/DOWN] SELECT  [LEFT/RIGHT] CHANGE  [ENTER] SAVE  [F1/ESC] CLOSE";
            float font_foot = 1.7f * scale_ui;
            c.Write(footer, box_x0 + (box_w - TextWidth(footer, font_foot)) / 2.0f, box_y1 - 40.0f * scale_ui, font_foot, kGray);
        }
    }

    if (g_video_menu_open.load() && g_menu_selected == 8) {
        static const char* const kStyles[] = {"ps5", "ps4", "xbox", "xbox360", "steam"};
        int sel = g_menu_items[8].selected;
        if (sel >= 0 && sel < 5) {
            float scale_toast = client.bottom >= 1400 ? 1.4f : (client.bottom >= 1000 ? 1.0f : 0.8f);
            RenderTrophyToast(c, client, scale_toast, kStyles[sel], 16, 'G',
                              lang::Pt() ? "Exemplo de Conquista" : "Sample Achievement",
                              lang::Pt() ? "Pre-visualizacao do estilo selecionado" : "Selected popup style preview",
                              1.0f, true);
        }
    }

    if (g_achievements_overlay_open.load()) {
        const float scale_ui = client.bottom >= 1400 ? 1.4f : (client.bottom >= 1000 ? 1.0f : 0.75f);
        const float pad_x = client.right * 0.05f;
        const float pad_y = client.bottom * 0.045f;
        const float box_x0 = pad_x;
        const float box_x1 = client.right - pad_x;
        const float box_y0 = pad_y;
        const float box_y1 = client.bottom - pad_y;
        const float box_w = box_x1 - box_x0;

        // Dark full backdrop with PS5 atmospheric ambient radiance
        c.Rect(0, 0, client.right, client.bottom, Rgba(10, 12, 17, 245));
        c.Rect(client.right * 0.55f, 0, client.right, client.bottom * 0.45f, Rgba(122, 42, 24, 18));
        c.Rect(client.right * 0.70f, client.bottom * 0.45f, client.right, client.bottom, Rgba(80, 26, 16, 14));

        int unlocked = 0, total = 77, n_plat = 0, n_gold = 0, n_silv = 0, n_brz = 0;
        hle::trophy::TrophyManager::Instance().GetProgress(&unlocked, &total, &n_plat, &n_gold, &n_silv, &n_brz);
        const int pct = total > 0 ? (unlocked * 100 / total) : 0;

        // Top Header
        const float header_y = box_y0 + 10.0f * scale_ui;

        // Game Thumbnail (ICON0.PNG)
        const float thumb_sz = 48.0f * scale_ui;
        const float thumb_x = box_x0 + 14.0f * scale_ui;
        const float thumb_y = header_y;
        c.Rect(thumb_x - 1.0f, thumb_y - 1.0f, thumb_x + thumb_sz + 1.0f, thumb_y + thumb_sz + 1.0f, Rgba(42, 49, 66));
        c.Rect(thumb_x, thumb_y, thumb_x + thumb_sz, thumb_y + thumb_sz, Rgba(22, 26, 36));
        const TrophyThumbnail* game_thumb = GetTrophyThumbnail(-1, "ICON0.PNG");
        if (game_thumb && game_thumb->loaded) {
            c.DrawThumbnail(game_thumb, thumb_x + 2.0f * scale_ui, thumb_y + 2.0f * scale_ui, thumb_sz - 4.0f * scale_ui, 1.0f);
        } else {
            c.Circle(thumb_x + thumb_sz * 0.5f, thumb_y + thumb_sz * 0.5f, thumb_sz * 0.35f, Rgba(255, 215, 0, 102));
            c.Circle(thumb_x + thumb_sz * 0.5f, thumb_y + thumb_sz * 0.5f, thumb_sz * 0.22f, Rgba(0, 229, 255, 153));
        }

        // Game Title
        const float title_x = thumb_x + thumb_sz + 16.0f * scale_ui;
        c.Write("LEGO(R) DIMENSIONS(TM)", title_x, thumb_y + 14.0f * scale_ui, 2.3f * scale_ui, Rgba(255, 255, 255));

        // Header Right: "Progresso" and "Conquistados"
        const float p_x = box_x1 - 430.0f * scale_ui;
        c.Write(lang::Pt() ? "Progresso" : "Progress", p_x, header_y, 1.35f * scale_ui, Rgba(142, 149, 165));
        std::string p_str = std::to_string(pct) + "%";
        c.Write(p_str, p_x, header_y + 16.0f * scale_ui, 2.1f * scale_ui, Rgba(255, 255, 255));
        const float u_w = 48.0f * scale_ui;
        c.Rect(p_x, header_y + 36.0f * scale_ui, p_x + u_w, header_y + 38.0f * scale_ui, Rgba(46, 53, 68));
        if (pct > 0) {
            c.Rect(p_x, header_y + 36.0f * scale_ui, p_x + u_w * (pct / 100.0f), header_y + 38.0f * scale_ui, Rgba(255, 255, 255));
        }

        const float c_x = box_x1 - 320.0f * scale_ui;
        c.Write(lang::Pt() ? "Conquistados" : "Earned", c_x, header_y, 1.35f * scale_ui, Rgba(142, 149, 165));
        std::string r_str = std::to_string(unlocked) + "/" + std::to_string(total);
        float rx = c.Write(r_str, c_x, header_y + 16.0f * scale_ui, 2.0f * scale_ui, Rgba(255, 255, 255)) + 16.0f * scale_ui;

        // 4 trophy cups in header
        auto draw_header_cup = [&](u32 col, int count_val) {
            const float cp = scale_ui * 1.8f;
            c.DrawBitmap(kTrophyCup, 7, rx, header_y + 18.0f * scale_ui, cp, col);
            rx += 6.0f * cp + 4.0f * scale_ui;
            rx = c.Write(std::to_string(count_val), rx, header_y + 17.0f * scale_ui, 1.6f * scale_ui, Rgba(255, 255, 255)) + 12.0f * scale_ui;
        };
        draw_header_cup(Rgba(0, 229, 255), n_plat);   // Platinum
        draw_header_cup(Rgba(255, 215, 0), n_gold);   // Gold
        draw_header_cup(Rgba(208, 212, 224), n_silv); // Silver
        draw_header_cup(Rgba(205, 127, 50), n_brz);   // Bronze

        // Subheader row
        const float sub_y = header_y + 54.0f * scale_ui;
        const float fx = box_x0 + 14.0f * scale_ui;
        c.DrawBitmap(kTriangleUp, 3, fx, sub_y + 3.0f * scale_ui, scale_ui * 1.5f, Rgba(142, 149, 165));

        std::string filter_lbl;
        if (g_achievements_filter == 1) {
            filter_lbl = (lang::Pt() ? "Nao conquistados: " : "Unearned: ") + std::to_string(total - unlocked);
        } else if (g_achievements_filter == 2) {
            filter_lbl = (lang::Pt() ? "Conquistados: " : "Earned: ") + std::to_string(unlocked);
        } else {
            filter_lbl = (lang::Pt() ? "Todas as conquistas: " : "All trophies: ") + std::to_string(total);
        }
        c.Write(filter_lbl, fx + 16.0f * scale_ui, sub_y, 1.4f * scale_ui, Rgba(142, 149, 165));

        std::string sort_str;
        if (g_achievements_filter == 1) {
            sort_str = lang::Pt() ? "Classificar por: Nao conquistado" : "Sort by: Unearned";
        } else if (g_achievements_filter == 2) {
            sort_str = lang::Pt() ? "Classificar por: Conquistado" : "Sort by: Earned";
        } else {
            sort_str = lang::Pt() ? "Classificar por: Padrao" : "Sort by: Default";
        }
        const float sort_w = TextWidth(sort_str, 1.4f * scale_ui);
        c.Write(sort_str, box_x1 - sort_w - 14.0f * scale_ui, sub_y, 1.4f * scale_ui, Rgba(142, 149, 165));

        // Filtered items
        const auto filtered = GetFilteredAchievements(g_achievements_filter);
        const int filtered_count = static_cast<int>(filtered.size());
        size_t count = 0;
        const TrophyDef* defs = GetTrophyDefs(&count);

        const float card_y0 = sub_y + 24.0f * scale_ui;
        const float card_w = box_w - 28.0f * scale_ui;
        const float card_h = 84.0f * scale_ui;
        const float card_gap = 10.0f * scale_ui;
        const float footer_y = box_y1 - 24.0f * scale_ui;
        const float avail_cards_h = footer_y - card_y0 - 8.0f * scale_ui;
        const int kVisibleCards = std::clamp(static_cast<int>((avail_cards_h + card_gap) / (card_h + card_gap)), 5, 12);
        g_achievements_visible_count = kVisibleCards;

        // Clamp scroll if screen grew or filter changed
        if (g_achievements_scroll > std::max(0, filtered_count - kVisibleCards)) {
            g_achievements_scroll = std::max(0, filtered_count - kVisibleCards);
        }

        for (int v = 0; v < kVisibleCards; ++v) {
            const int f_idx = g_achievements_scroll + v;
            if (f_idx >= filtered_count) break;

            const int d_idx = filtered[f_idx];
            if (d_idx < 0 || d_idx >= static_cast<int>(count)) continue;

            const TrophyDef& def = defs[d_idx];
            const bool is_unlocked = hle::trophy::TrophyManager::Instance().IsUnlocked(def.id);
            const bool is_sel = (f_idx == g_achievements_selected);

            const float cx0 = box_x0 + 14.0f * scale_ui;
            const float cx1 = cx0 + card_w;
            const float cy0 = card_y0 + v * (card_h + card_gap);
            const float cy1 = cy0 + card_h;

            u32 tier_color = Rgba(205, 127, 50); // Bronze
            const char* tier_label = lang::Pt() ? "Bronze" : "Bronze";
            float rarity_pct = 28.3f;
            const char* rarity_tier = lang::Pt() ? "Raro" : "Rare";

            if (def.type == 'P') {
                tier_color = Rgba(0, 229, 255); // Platinum
                tier_label = lang::Pt() ? "Platina" : "Platinum";
                rarity_pct = 1.8f;
                rarity_tier = lang::Pt() ? "Ultra Raro" : "Ultra Rare";
            } else if (def.type == 'G') {
                tier_color = Rgba(255, 215, 0); // Gold
                tier_label = lang::Pt() ? "Ouro" : "Gold";
                rarity_pct = 6.4f;
                rarity_tier = lang::Pt() ? "Muito Raro" : "Very Rare";
            } else if (def.type == 'S') {
                tier_color = Rgba(208, 212, 224); // Silver
                tier_label = lang::Pt() ? "Prata" : "Silver";
                rarity_pct = 16.2f;
                rarity_tier = lang::Pt() ? "Raro" : "Rare";
            }

            // Card background
            if (is_sel) {
                c.Rect(cx0 - 1.5f, cy0 - 1.5f, cx1 + 1.5f, cy1 + 1.5f, Rgba(255, 255, 255));
                c.Rect(cx0, cy0, cx1, cy1, Rgba(34, 39, 52, 250));
                c.Rect(cx1 - card_w * 0.32f, cy0, cx1, cy1, Rgba(128, 56, 34, 34));
            } else {
                c.Rect(cx0, cy0, cx1, cy1, Rgba(21, 24, 32, 212));
                c.Rect(cx1 - card_w * 0.32f, cy0, cx1, cy1, Rgba(112, 45, 30, 20));
            }

            // Left Badge (Square Tile with trophy artwork)
            const float badge_sz = card_h - 18.0f * scale_ui;
            const float bx0 = cx0 + 10.0f * scale_ui;
            const float by0 = cy0 + 9.0f * scale_ui;
            c.Rect(bx0 - 1.0f, by0 - 1.0f, bx0 + badge_sz + 1.0f, by0 + badge_sz + 1.0f, is_unlocked ? tier_color : Rgba(34, 40, 54));
            c.Rect(bx0, by0, bx0 + badge_sz, by0 + badge_sz, Rgba(25, 29, 39));

            const TrophyThumbnail* thumb = GetTrophyThumbnail(def.id, def.icon);
            if (thumb && thumb->loaded) {
                if (is_unlocked) {
                    c.DrawThumbnail(thumb, bx0 + 2.0f * scale_ui, by0 + 2.0f * scale_ui, badge_sz - 4.0f * scale_ui, 1.0f);
                } else {
                    c.DrawThumbnail(thumb, bx0 + 2.0f * scale_ui, by0 + 2.0f * scale_ui, badge_sz - 4.0f * scale_ui, 0.35f);
                    // Silver padlock badge on bottom right of badge
                    const float lock_p = scale_ui * 1.5f;
                    const float lock_x = bx0 + badge_sz - 5.0f * lock_p - 4.0f * scale_ui;
                    const float lock_y = by0 + badge_sz - 5.0f * lock_p - 4.0f * scale_ui;
                    c.DrawBitmap(kPadlock, 5, lock_x, lock_y, lock_p, Rgba(213, 219, 232));
                }
            } else {
                // Fallback to cup icon
                const float cup_p = scale_ui * 2.3f;
                const float cup_x = bx0 + (badge_sz - 5.0f * cup_p) * 0.5f;
                const float cup_y = by0 + (badge_sz - 7.0f * cup_p) * 0.5f;
                if (is_unlocked) {
                    c.DrawBitmap(kTrophyCup, 7, cup_x, cup_y, cup_p, tier_color);
                } else {
                    c.DrawBitmap(kTrophyCup, 7, cup_x, cup_y, cup_p, Rgba(77, 83, 99));
                    const float lock_p = scale_ui * 1.5f;
                    const float lock_x = bx0 + badge_sz - 5.0f * lock_p - 4.0f * scale_ui;
                    const float lock_y = by0 + badge_sz - 5.0f * lock_p - 4.0f * scale_ui;
                    c.DrawBitmap(kPadlock, 5, lock_x, lock_y, lock_p, Rgba(213, 219, 232));
                }
            }

            // Middle Column
            const float col_x = bx0 + badge_sz + 18.0f * scale_ui;
            const std::string trophy_name = lang::Pt() ? def.name_pt : def.name_en;
            c.Write(trophy_name, col_x, cy0 + 12.0f * scale_ui, 2.0f * scale_ui, Rgba(255, 255, 255));

            // Tier line: mini cup + Tier name
            const float mini_p = scale_ui * 1.5f;
            c.DrawBitmap(kTrophyCup, 7, col_x, cy0 + 36.0f * scale_ui, mini_p, tier_color);
            c.Write(tier_label, col_x + 14.0f * scale_ui, cy0 + 35.0f * scale_ui, 1.45f * scale_ui, Rgba(175, 180, 195));

            // Rarity line: triangle + rarity text
            c.DrawBitmap(kTriangleUp, 3, col_x, cy0 + 58.0f * scale_ui, scale_ui * 1.3f, Rgba(142, 149, 165));
            char r_buf[128];
            std::snprintf(r_buf, sizeof(r_buf), lang::Pt() ? "%s | %.1f%% dos jogadores conquistaram" : "%s | %.1f%% of players earned this",
                          rarity_tier, rarity_pct);
            c.Write(r_buf, col_x + 12.0f * scale_ui, cy0 + 55.0f * scale_ui, 1.25f * scale_ui, Rgba(142, 149, 165));

            // Right Column: Description
            const float desc_x = cx0 + card_w * 0.53f;
            const std::string trophy_desc = lang::Pt() ? def.desc_pt : def.desc_en;
            if (trophy_desc.size() > 42) {
                size_t split_pos = trophy_desc.rfind(' ', 42);
                if (split_pos == std::string::npos) split_pos = 42;
                std::string line1 = trophy_desc.substr(0, split_pos);
                std::string line2 = trophy_desc.substr(split_pos + 1);
                c.Write(line1, desc_x, cy0 + 22.0f * scale_ui, 1.4f * scale_ui, Rgba(174, 180, 196));
                c.Write(line2, desc_x, cy0 + 44.0f * scale_ui, 1.4f * scale_ui, Rgba(174, 180, 196));
            } else {
                c.Write(trophy_desc, desc_x, cy0 + 33.0f * scale_ui, 1.45f * scale_ui, Rgba(174, 180, 196));
            }
        }

        if (filtered.empty()) {
            const std::string empty_msg = lang::Pt() ? "Nenhuma conquista encontrada nesta categoria." : "No achievements found in this category.";
            c.Write(empty_msg, box_x0 + 40.0f * scale_ui, card_y0 + 60.0f * scale_ui, 1.8f * scale_ui, Rgba(142, 149, 165));
        }

        // Minimalist Scrollbar on right edge (uses corrected vertical Pill)
        const float sb_x0 = box_x1 - 9.0f * scale_ui;
        const float sb_x1 = box_x1 - 5.0f * scale_ui;
        const float sb_y0 = card_y0;
        const float sb_y1 = card_y0 + kVisibleCards * (card_h + card_gap) - card_gap;
        c.Pill(sb_x0, sb_y0, sb_x1, sb_y1, Rgba(24, 28, 38));
        if (filtered_count > kVisibleCards) {
            const float thumb_h = std::max(28.0f * scale_ui, (sb_y1 - sb_y0) * kVisibleCards / filtered_count);
            const float thumb_y0 = sb_y0 + (sb_y1 - sb_y0 - thumb_h) * (static_cast<float>(g_achievements_scroll) / (filtered_count - kVisibleCards));
            c.Pill(sb_x0, thumb_y0, sb_x1, thumb_y0 + thumb_h, Rgba(101, 112, 136));
        }

        // Footer at bottom
        const std::string footer = lang::Pt()
            ? "[D-PAD / ANALOGICO] NAVEGAR   [LB / RB] PAGINA   [X / TAB] FILTRAR   [B / ESC / F8] FECHAR   [RB+LB+RT] FECHAR"
            : "[D-PAD / STICK] NAVIGATE   [LB / RB] PAGE   [X / TAB] FILTER   [B / ESC / F8] CLOSE   [RB+LB+RT] CLOSE";
        const float font_foot = 1.35f * scale_ui;
        c.Write(footer, box_x0 + (box_w - TextWidth(footer, font_foot)) * 0.5f, box_y1 - 18.0f * scale_ui, font_foot, Rgba(117, 125, 142));
    }

    if (g_logs_overlay_open.load()) {
        RenderLogsOverlay(c, client);
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
        RenderTrophyToast(c, client, scale_ui, toast.popup_style.empty() ? "ps5" : toast.popup_style,
                          toast.trophy_id, toast.type, toast.title, toast.desc, anim, false);
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
