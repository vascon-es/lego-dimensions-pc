#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ui.h"
#include "pad_hid.h"
#include "../../src/common/toypad_catalog_protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")

namespace {
namespace fs = std::filesystem;
namespace wire = toypad_catalog;
using Gdiplus::Graphics;
using Gdiplus::RectF;
using Bytes = std::vector<std::uint8_t>;

constexpr int kWidth = 964, kHeight = 642, kColumns = 12;
constexpr float kGridX = 35, kGridY = 145, kGridWidth = kWidth - 2 * kGridX, kGridHeight = 378;
constexpr float kVehicleRow = 58.0f;
constexpr int kRosterTile = 192;
constexpr ULONGLONG kScrollMs = 165;
constexpr COLORREF kFocus = RGB(255, 214, 104), kQuiet = RGB(219, 229, 238);
constexpr float kRise = 62;
constexpr ULONGLONG kRiseMs = 560;
constexpr DWORD kWindowStyle = WS_POPUP | WS_SYSMENU;
constexpr int kAppearanceRows = 5, kTransparencyRow = 2, kTextBackdropRow = 3, kMaxTransparency = 100;
constexpr float kAppearanceTop = 196.0f, kAppearanceStep = 60.0f;
constexpr ULONGLONG kCatalogRiseMs = 340, kTabSlideMs = 260;

enum class Glyph { Cross, Circle, Square, Triangle, L1, R1 };
struct Artwork {
    std::unique_ptr<Gdiplus::Bitmap> roster;
    std::array<std::unique_ptr<Gdiplus::Bitmap>, 2> buttons;
};

// Geometria de kPadCells do LegoToypad (MIT), com a margem da tela de referência.
struct PadPosition { float x, y, w, h; int zone; };
constexpr PadPosition OriginalPad(float left, float top, float right, float bottom, int zone) {
    return {39 + (left + right) / 2, 20 + (top + bottom) / 2, right - left, bottom - top, zone};
}
constexpr PadPosition kPositions[] = {
    OriginalPad(356, 103, 544, 287, 1),
    OriginalPad(100, 220, 240, 356, 2),
    OriginalPad(100, 364, 236, 500, 2),
    OriginalPad(244, 364, 408, 500, 2),
    OriginalPad(660, 220, 800, 356, 3),
    OriginalPad(492, 364, 656, 500, 3),
    OriginalPad(664, 364, 800, 500, 3),
};
static_assert(std::size(kPositions) == wire::kSlots);
constexpr float PadWidth(int position) { return kPositions[position].w; }
constexpr float PadHeight(int position) { return kPositions[position].h; }
constexpr wchar_t kOriginalSkin[] = L"skin-harrysof";

struct Surface {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ previous = nullptr;
    BYTE* pixels = nullptr;
    int width = 0, height = 0;

    bool Init(int w, int h) {
        width = w;
        height = h;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) return false;
        pixels = static_cast<BYTE*>(bits);
        previous = SelectObject(dc, dib);
        return previous && previous != HGDI_ERROR;
    }

    ~Surface() {
        Reset();
    }

    void Reset() {
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        previous = nullptr;
        dib = nullptr;
        dc = nullptr;
        pixels = nullptr;
        width = height = 0;
    }
};

struct Figure {
    std::uint32_t id;
    std::wstring name;
    COLORREF color;
    std::unique_ptr<Gdiplus::Bitmap> icon;
    bool tried = false;
    Gdiplus::Rect source{};
};

struct PortalReply {
    wire::Snapshot slots{};
    bool connected = false;
};

struct Catalog {
    fs::path root, cache, images;
    HWND window = nullptr;
    HWND game_window = nullptr;
    HWND previous_window = nullptr;
    HANDLE extraction = nullptr;
    RECT placement{}, desktop{};
    float desktop_scale = 1.0f;
    int port = wire::kPort;
    int selected = 0, position = 0, slot = 0, zone = 1;
    int move_from = -1;
    bool vehicles = false, loaded = false, extraction_error = false, browsing = false, command_in_flight = false;
    bool appearance = false, choosing_image = false;
    bool nonactivating = false;
    bool text_backdrop = false;
    int appearance_item = 0, theme = 0, transparency = kMaxTransparency;
    std::wstring background_path;
    std::wstring background_error;
    std::unique_ptr<Gdiplus::Bitmap> background;
    std::unique_ptr<Gdiplus::Bitmap> backdrop;
    std::unique_ptr<Gdiplus::Bitmap> original_background, original_wordmark, original_credit, original_settings;
    std::array<std::unique_ptr<Gdiplus::Bitmap>, wire::kSlots> original_pads;
    ULONGLONG started = 0, next_query = 0, repeat = 0;
    ULONGLONG open_after = 0;
    ULONGLONG panel_opened = 0;
    ULONGLONG catalog_opened = 0;
    ULONGLONG tab_started = 0;
    ULONGLONG scroll_started = 0;
    int previous_selected = 0;
    float scroll = 0, scroll_from = 0, scroll_target = 0, previous_scroll = 0;
    std::uint32_t previous = 0;
    int previous_dir = 0;
    std::vector<Figure> figures;
    std::vector<size_t> list;
    std::vector<size_t> previous_list;
    Artwork art;
    std::future<Artwork> art_loading;
    std::array<std::vector<std::unique_ptr<Gdiplus::Bitmap>>, 2> grid_rows;
    float grid_scale = 0.0f;
    PortalReply portal;
    std::future<PortalReply> request;
    std::optional<Bytes> action;
    std::array<int, wire::kSlots> positions{0, 1, 2, 3, 4, 5, 6};
    Surface surface;
} g;

using GetPad = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
using GetPadKey = DWORD (WINAPI*)(DWORD, DWORD, XINPUT_KEYSTROKE*);
GetPad g_get_pad = nullptr;
GetPadKey g_get_pad_key = nullptr;
std::atomic<XINPUT_GAMEPAD> g_sony_pad{};
std::atomic<bool> g_sony_back{};
std::array<ULONGLONG, XUSER_MAX_COUNT> g_pad_retry{};

std::wstring Wide(const std::string& text) {
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), n);
    return result;
}

std::wstring Quote(const std::wstring& value) {
    // Uma barra imediatamente antes da aspa de fechamento precisa ser duplicada.
    const auto end = value.find_last_not_of(L'\\');
    const auto trailing = end == std::wstring::npos ? value.size() : value.size() - end - 1;
    return L"\"" + value + std::wstring(trailing, L'\\') + L"\"";
}

std::vector<size_t> CatalogList(bool vehicles) {
    std::vector<size_t> list;
    for (size_t i = 0; i < g.figures.size(); ++i)
        if ((g.figures[i].id >= 1000) == vehicles) list.push_back(i);
    return list;
}

void RefreshList() {
    g.list = CatalogList(g.vehicles);
    g.selected = 0;
}

bool ReadCatalog() {
    std::error_code ec;
    const auto path = g.cache / L"catalog-v4.tsv";
    if (fs::file_size(path, ec) > 256 * 1024 || ec) return false;
    std::ifstream in(path);
    std::string version, row;
    if (!std::getline(in, version) || version.size() != 64 ||
        version.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    std::vector<Figure> figures;
    while (std::getline(in, row) && figures.size() < 1024) {
        const auto tab = row.find('\t');
        if (tab == std::string::npos || row.size() > 512) continue;
        const auto english = row.find('\t', tab + 1);
        if (english == std::string::npos) continue;
        const auto color_at = row.find('\t', english + 1);
        if (color_at == std::string::npos) continue;
        std::uint32_t id = 0;
        const auto parsed = std::from_chars(row.data(), row.data() + tab, id);
        if (parsed.ec != std::errc() || !id || id > 65535 || parsed.ptr != row.data() + tab) continue;
        auto name = Wide(ui::Pt() ? row.substr(tab + 1, english - tab - 1) : row.substr(english + 1, color_at - english - 1));
        std::uint32_t rgb = 0x76B460;
        const auto color = std::from_chars(row.data() + color_at + 1, row.data() + row.size(), rgb, 16);
        if (color.ec != std::errc() || color.ptr != row.data() + row.size()) rgb = 0x76B460;
        if (!name.empty()) figures.push_back({id, std::move(name), RGB(rgb >> 16, rgb >> 8, rgb)});
    }
    if (figures.empty() || !fs::is_directory(g.cache / Wide(version), ec)) return false;
    const auto selected_id = g.list.empty() ? 0 : g.figures[g.list[g.selected]].id;
    g.previous_list.clear();
    g.tab_started = 0;
    g.grid_rows = {};
    g.figures = std::move(figures);
    g.images = g.cache / Wide(version);
    RefreshList();
    for (int i = 0; i < static_cast<int>(g.list.size()); ++i)
        if (g.figures[g.list[i]].id == selected_id) g.selected = i;
    return true;
}

std::unique_ptr<Gdiplus::Bitmap> LoadSkinImage(const wchar_t* filename, UINT max_width = 0) {
    auto source = std::make_unique<Gdiplus::Bitmap>((g.cache / kOriginalSkin / filename).c_str());
    if (source->GetLastStatus() != Gdiplus::Ok || !source->GetWidth() || !source->GetHeight()) return {};
    if (!max_width || source->GetWidth() <= max_width) return source;
    const UINT height = source->GetHeight() * max_width / source->GetWidth();
    auto scaled = std::make_unique<Gdiplus::Bitmap>(max_width, height, PixelFormat32bppPARGB);
    if (scaled->GetLastStatus() != Gdiplus::Ok) return source;
    {
        Graphics paint(scaled.get());
        paint.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        paint.DrawImage(source.get(), 0, 0, static_cast<int>(max_width), static_cast<int>(height));
    }
    return scaled;
}

void ReadOriginalSkin() {
    g.original_background = LoadSkinImage(L"background.jpg");
    g.original_wordmark = LoadSkinImage(L"wordmark.png", 400);
    g.original_credit = LoadSkinImage(L"by_harrysof.png");
    g.original_settings = LoadSkinImage(L"settings.png");
    constexpr const wchar_t* pads[] = {
        L"Center.png", L"left_upper.png", L"left_lower_left.png", L"left_lower_right.png",
        L"right_upper.png", L"right_lower_left.png", L"right_lower_right.png",
    };
    for (int i = 0; i < wire::kSlots; ++i) g.original_pads[i] = LoadSkinImage(pads[i]);
    g.backdrop.reset();
}

Artwork ReadArtwork(const fs::path& root) {
    Artwork art;
    // A cópia de renderização contém a célula inteira, inclusive o que passa do aro.
    Gdiplus::Bitmap source((root / L"tools/setup/toypad_roster.png").c_str());
    if (source.GetLastStatus() == Gdiplus::Ok && source.GetWidth() >= 5120 && source.GetHeight() >= 4264) {
        art.roster = std::make_unique<Gdiplus::Bitmap>(10 * kRosterTile, 8 * kRosterTile, PixelFormat32bppPARGB);
        Graphics paint(art.roster.get());
        paint.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        paint.DrawImage(&source, Gdiplus::Rect(0, 0, 10 * kRosterTile, 8 * kRosterTile),
                        0, 168, 5120, 4096, Gdiplus::UnitPixel);
    }
    constexpr int side = 608;
    const auto folder = root / L"assets/buttons";
    const auto load_buttons = [&](const wchar_t* name) -> std::unique_ptr<Gdiplus::Bitmap> {
        std::ifstream in(folder / name, std::ios::binary);
        std::vector<BYTE> pixels(side * side * 4);
        if (!in.read(reinterpret_cast<char*>(pixels.data()), pixels.size())) return {};
        auto sheet = std::make_unique<Gdiplus::Bitmap>(side, side, PixelFormat32bppARGB);
        Gdiplus::Rect bounds(0, 0, side, side);
        Gdiplus::BitmapData locked{};
        if (sheet->LockBits(&bounds, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &locked) != Gdiplus::Ok) return {};
        for (int y = 0; y < side; ++y) {
            auto* dest = static_cast<BYTE*>(locked.Scan0) + static_cast<std::ptrdiff_t>(y) * locked.Stride;
            for (int x = 0; x < side * 4; x += 4) {
                const auto* rgba = pixels.data() + y * side * 4 + x;
                dest[x] = rgba[2];
                dest[x + 1] = rgba[1];
                dest[x + 2] = rgba[0];
                dest[x + 3] = rgba[3];
            }
        }
        sheet->UnlockBits(&locked);
        return sheet;
    };
    art.buttons[0] = load_buttons(L"ps4.rgba");
    art.buttons[1] = load_buttons(L"xboxone.rgba");
    if (!art.buttons[1]) art.buttons[1] = load_buttons(L"360.rgba");
    return art;
}

void PreparePortraits() {
    if ((g.loaded && g.original_background && g.original_wordmark && g.original_credit && g.original_settings &&
        std::all_of(g.original_pads.begin(), g.original_pads.end(), [](const auto& image) { return bool(image); })) ||
        g.extraction) return;
    const auto python = g.root / L"tools/python/python.exe";
    std::error_code ec;
    std::wstring command = fs::is_regular_file(python, ec) ? Quote(python.wstring()) : L"python";
    command += L" -I " + Quote((g.root / L"tools/setup/toypad_catalog.py").wstring()) + L" --output " + Quote(g.cache.wstring());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, g.root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        g.extraction = pi.hProcess;
    } else {
        g.extraction_error = true;
    }
}

PortalReply Exchange(Bytes bytes, int port) {
    PortalReply result;
    const wire::Header snapshot{wire::kSnapshot};
    bytes.insert(bytes.end(), snapshot.begin(), snapshot.end());
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return result;
    struct SocketGuard { SOCKET handle; ~SocketGuard() { closesocket(handle); } } guard{socket_handle};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD timeout = 1800;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (connect(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) return result;
    for (size_t offset = 0; offset < bytes.size();) {
        const int count = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + offset),
                               static_cast<int>(bytes.size() - offset), 0);
        if (count <= 0) return result;
        offset += count;
    }
    shutdown(socket_handle, SD_SEND);
    for (size_t offset = 0; offset < result.slots.size();) {
        const int count = recv(socket_handle, reinterpret_cast<char*>(result.slots.data() + offset),
                               static_cast<int>(result.slots.size() - offset), 0);
        if (count <= 0) return result;
        offset += count;
    }
    result.connected = result.slots[0] == 'T' && result.slots[1] == 1;
    return result;
}

int SlotPad(int slot) { return g.portal.connected ? g.portal.slots[2 + slot * 5] : 0; }
std::uint32_t SlotId(int slot) { return SlotPad(slot) ? wire::ReadId(g.portal.slots.data() + 3 + slot * 5) : 0; }

const wchar_t* ZoneName(int pad) {
    switch (pad) {
    case 1: return ui::T(L"Centro", L"Centre");
    case 2: return ui::T(L"Esquerda", L"Left");
    case 3: return ui::T(L"Direita", L"Right");
    default: return L"—";
    }
}

fs::path CatalogIni() { return g.root / L"tools" / L"LegoToypadCatalog.ini"; }

void EnsureCatalogIni(const fs::path& ini) {
    const HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    constexpr wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(file);
}

void ReadPositionMapping() {
    const auto ini = CatalogIni();
    std::array<wchar_t, wire::kSlots + 2> value{};
    if (GetPrivateProfileStringW(L"ToyPad", L"Posicoes", L"", value.data(),
                                 static_cast<DWORD>(value.size()), ini.c_str()) != wire::kSlots) return;
    std::array<int, wire::kSlots> mapped{};
    std::array<bool, wire::kSlots> used{};
    for (int p = 0; p < wire::kSlots; ++p) {
        const int slot = value[p] - L'0';
        if (slot < 0 || slot >= wire::kSlots || used[slot]) return;
        mapped[p] = slot;
        used[slot] = true;
    }
    g.positions = mapped;
}

void MapSlots() {
    std::array<int, wire::kSlots> mapped;
    std::array<bool, wire::kSlots> used{};
    mapped.fill(-1);
    for (int p = 0; p < wire::kSlots; ++p) {
        const int slot = g.positions[p];
        if (SlotPad(slot) == kPositions[p].zone) {
            mapped[p] = slot;
            used[slot] = true;
        }
    }
    for (int slot = 0; slot < wire::kSlots; ++slot) {
        if (used[slot] || !SlotPad(slot)) continue;
        for (int p = 0; p < wire::kSlots; ++p) {
            if (mapped[p] >= 0 || kPositions[p].zone != SlotPad(slot)) continue;
            mapped[p] = slot;
            used[slot] = true;
            break;
        }
    }
    for (int p = 0; p < wire::kSlots; ++p) {
        if (mapped[p] >= 0) continue;
        const int current = g.positions[p];
        const int slot = !used[current] ? current : static_cast<int>(std::find(used.begin(), used.end(), false) - used.begin());
        mapped[p] = slot;
        used[slot] = true;
    }
    if (g.positions == mapped) return;
    g.positions = mapped;
    // Uma figura vinda do app clássico pode usar um índice associado a outra zona no catálogo.
    // Mantemos o mapeamento para que ela reapareça no mesmo espaço visual ao reabrir.
    std::array<wchar_t, wire::kSlots + 1> value{};
    for (int p = 0; p < wire::kSlots; ++p) value[p] = static_cast<wchar_t>(L'0' + mapped[p]);
    const auto ini = CatalogIni();
    EnsureCatalogIni(ini);
    WritePrivateProfileStringW(L"ToyPad", L"Posicoes", value.data(), ini.c_str());
}

bool ReadyToChange() { return g.portal.connected && !g.action && !g.command_in_flight; }

void NavSound() {
    struct Wave {
        char riff[4]{'R','I','F','F'};
        DWORD size = 36 + 704 * sizeof(short);
        char wave[8]{'W','A','V','E','f','m','t',' '};
        DWORD format_size = 16;
        WORD pcm = 1, channels = 1;
        DWORD rate = 22050, bytes_per_second = 44100;
        WORD align = 2, bits = 16;
        char data[4]{'d','a','t','a'};
        DWORD data_size = 704 * sizeof(short);
        std::array<short, 704> samples{};
    };
    static const auto tick = [] {
        Wave wave;
        for (size_t i = 0; i < wave.samples.size(); ++i) {
            const double envelope = std::min(1.0, i / 35.0) * std::exp(-7.0 * i / wave.samples.size());
            wave.samples[i] = static_cast<short>(2600.0 * envelope * std::sin(.19 * i));
        }
        return wave;
    }();
    PlaySoundW(reinterpret_cast<LPCWSTR>(&tick), nullptr, SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
}

ULONGLONG AnimationNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float Ease(ULONGLONG start, ULONGLONG duration, ULONGLONG now = AnimationNow()) {
    if (now < start) return 0.0f;
    if (!start || now - start >= duration) return 1.0f;
    const float t = static_cast<float>(now - start) / duration;
    return t * t * (3.0f - 2.0f * t);
}

void SetFigure(int slot, int zone, std::uint32_t id) {
    Bytes command{wire::kSelect, static_cast<std::uint8_t>(zone), static_cast<std::uint8_t>(slot), 0, 0, 0, 0, 0, 0};
    wire::WriteId(command.data() + 5, id);
    g.action = std::move(command);
}

void ClearPosition() {
    const int slot = g.positions[g.position];
    if (!g.browsing && ReadyToChange() && SlotPad(slot)) {
        SetFigure(slot, SlotPad(slot), 0);
        g.move_from = -1;
    }
}

void PickUp() {
    const int slot = g.positions[g.position];
    if (!g.browsing && ReadyToChange() && SlotPad(slot)) g.move_from = slot;
}

void FocusPosition(int dx, int dy) {
    const auto& current = kPositions[g.position];
    int result = g.position;
    float best = 1e9f;
    for (int i = 0; i < wire::kSlots; ++i) {
        const float x = kPositions[i].x - current.x, y = kPositions[i].y - current.y;
        const float along = x * dx + y * dy, across = y * dx + x * dy;
        if (along <= 0) continue;
        const float score = along * along + across * across * 2;
        if (score < best) {
            best = score;
            result = i;
        }
    }
    if (g.position != result) {
        g.position = result;
        NavSound();
    }
}

void FocusFigure(int index) {
    if (g.list.empty()) return;
    const int selected = std::clamp(index, 0, static_cast<int>(g.list.size()) - 1);
    if (selected != g.selected) NavSound();
    g.selected = selected;
    if (!g.vehicles) return;
    const float top = (selected / kColumns) * kVehicleRow;
    const float bottom = top + kVehicleRow;
    const int rows = (static_cast<int>(g.list.size()) + kColumns - 1) / kColumns;
    const float nearest = std::clamp(g.scroll_target, bottom - kGridHeight + 12.0f, top);
    const float target = std::clamp(nearest, 0.0f, std::max(0.0f, rows * kVehicleRow - kGridHeight));
    if (g.scroll_target != target) {
        g.scroll_from = g.scroll;
        g.scroll_target = target;
        g.scroll_started = AnimationNow();
    }
}

void Select(int amount) {
    FocusFigure(g.selected + amount);
}

void Page(bool vehicles) {
    if (!g.browsing || g.vehicles == vehicles) return;
    g.previous_list = g.list;
    g.previous_selected = g.selected;
    g.previous_scroll = g.scroll;
    g.vehicles = vehicles;
    RefreshList();
    g.scroll = g.scroll_target = 0;
    g.scroll_started = 0;
    g.tab_started = AnimationNow();
    NavSound();
}

// Foco adaptado do LegoToypad de harrysof. Ver LICENSE-LegoToypad.txt (MIT).
void ActivateWindow(HWND target) {
    if (!IsWindow(target) || GetForegroundWindow() == target) return;
    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD current_thread = GetCurrentThreadId();
    const bool attached = foreground_thread && foreground_thread != current_thread &&
                          AttachThreadInput(current_thread, foreground_thread, TRUE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (attached) AttachThreadInput(current_thread, foreground_thread, FALSE);
}

bool IsGameWindow(HWND window) {
    wchar_t type[64]{};
    return window && GetClassNameW(window, type, static_cast<int>(std::size(type))) > 0 &&
           std::wcscmp(type, wire::kGameWindowClass) == 0;
}

bool GameCoversMonitor(HWND hwnd) {
    if (!hwnd || !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP)) return false;
    MONITORINFO monitor{sizeof(monitor)};
    RECT rect{};
    return GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
           GetWindowRect(hwnd, &rect) && EqualRect(&rect, &monitor.rcMonitor);
}

bool GameFocusLost() {
    if (!g.game_window) return false;
    if (!IsGameWindow(g.game_window) || IsIconic(g.game_window)) return true;
    const HWND foreground = GetForegroundWindow();
    const HWND owner = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
    return foreground && foreground != g.game_window && foreground != g.window &&
           owner != g.game_window && owner != g.window;
}

void SetInputOwnership(bool active) {
    if (active && IsGameWindow(g.game_window)) SetPropW(g.window, wire::kInputOwnerProperty, g.game_window);
    else RemovePropW(g.window, wire::kInputOwnerProperty);
}

void StartRise() {
    g.started = AnimationNow();
    SetWindowPos(g.window, nullptr, g.placement.left, g.placement.top + ui::S(kRise), g.placement.right, g.placement.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Confirm() {
    if (g.browsing) {
        if (g.list.empty() || !ReadyToChange()) return;
        SetFigure(g.slot, g.zone, g.figures[g.list[g.selected]].id);
        g.browsing = false;
        return;
    }
    if (g.action || g.command_in_flight) return;
    const int slot = g.positions[g.position];
    const int zone = kPositions[g.position].zone;
    if (g.move_from >= 0) {
        const int source = SlotPad(g.move_from);
        if (ReadyToChange() && source) {
            g.action = Bytes{wire::kSwap, static_cast<std::uint8_t>(zone), static_cast<std::uint8_t>(slot),
                             static_cast<std::uint8_t>(source), static_cast<std::uint8_t>(g.move_from)};
        }
        g.move_from = -1;
        return;
    }
    g.slot = slot;
    g.zone = zone;
    g.browsing = true;
    g.vehicles = false;
    RefreshList();
    g.scroll = g.scroll_target = 0;
    g.scroll_started = 0;
    g.tab_started = 0;
    g.previous_list.clear();
    g.catalog_opened = AnimationNow();
    NavSound();
}

void RenderWindow();

void ShowPanel() {
    g.open_after = 0;
    if (GameFocusLost()) { SetInputOwnership(false); return; }
    const bool visible = IsWindowVisible(g.window) && !IsIconic(g.window);
    constexpr LONG_PTR mask = WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    const LONG_PTR mode = WS_EX_LAYERED | (g.game_window ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW) |
                          (g.nonactivating ? WS_EX_NOACTIVATE : 0);
    const LONG_PTR current = GetWindowLongPtrW(g.window, GWL_EXSTYLE);
    if ((current & mask) != mode) {
        SetWindowLongPtrW(g.window, GWL_EXSTYLE, (current & ~mask) | mode);
        SetWindowPos(g.window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (visible) {
        SetInputOwnership(true);
        if (!g.nonactivating) ActivateWindow(g.window);
        return;
    }

    g.previous_window = g.game_window ? g.game_window : GetForegroundWindow();
    g.placement = g.desktop;
    ui::g_scale = g.desktop_scale;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(g.previous_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const auto& area = g.nonactivating ? monitor.rcMonitor : monitor.rcWork;
        const UINT dpi = GetDpiForWindow(g.previous_window ? g.previous_window : g.window);
        RECT frame{};
        AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, 0, dpi ? dpi : 96);
        const auto inset_w = frame.right - frame.left, inset_h = frame.bottom - frame.top;
        const auto available_w = area.right - area.left, available_h = area.bottom - area.top;
        ui::g_scale = std::min({dpi ? dpi / 96.0f : g.desktop_scale, static_cast<float>(available_w - inset_w) / kWidth,
                              static_cast<float>(available_h - inset_h) / kHeight});
        const auto outer_w = ui::S(kWidth) + inset_w, outer_h = ui::S(kHeight) + inset_h;
        g.placement = {area.left + (available_w - outer_w) / 2, area.top + (available_h - outer_h) / 2, outer_w, outer_h};
    }
    const int width = ui::S(kWidth), height = ui::S(kHeight);
    if (!g.surface.pixels || width != g.surface.width || height != g.surface.height) {
        g.surface.Reset();
        if (!g.surface.Init(width, height)) { SetInputOwnership(false); return; }
    }

    g.browsing = false;
    g.appearance = false;
    g.move_from = -1;
    g.next_query = 0;
    SetInputOwnership(true);
    StartRise();
    g.panel_opened = GetTickCount64();
    if (g.nonactivating || (g.game_window && (GetWindowLongPtrW(g.game_window, GWL_EXSTYLE) & WS_EX_TOPMOST)))
        SetWindowPos(g.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RenderWindow();
    ShowWindow(g.window, g.nonactivating ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!g.nonactivating) ActivateWindow(g.window);
}

void RequestPanel(bool from_launcher = false) {
    const HWND foreground = GetForegroundWindow();
    g.game_window = IsGameWindow(foreground) ? foreground : FindWindowW(wire::kGameWindowClass, nullptr);
    g.nonactivating = GameCoversMonitor(g.game_window);
    if (from_launcher && GameFocusLost() && g.game_window && !IsIconic(g.game_window)) ActivateWindow(g.game_window);
    if (GameFocusLost()) return;
    if (g.game_window) {
        SetInputOwnership(true);
        if (PostMessageW(g.game_window, wire::kOpenRequest, reinterpret_cast<WPARAM>(g.window), 0)) {
            g.open_after = GetTickCount64() + 150;
            return;
        }
    }
    ShowPanel();
}

void OpenForGame(HWND game, bool fullscreen = false) {
    if (IsWindowVisible(g.window) || !IsGameWindow(game) || GetForegroundWindow() != game) return;
    g.game_window = game;
    g.nonactivating = fullscreen || GameCoversMonitor(game);
    PostMessageW(game, wire::kOpenRequest, reinterpret_cast<WPARAM>(g.window), 0);
    ShowPanel();
}

void HidePanel(bool restore_focus = true) {
    g.open_after = 0;
    SetInputOwnership(false);
    if (!restore_focus && g.choosing_image) {
        const HWND dialog = GetLastActivePopup(g.window);
        if (dialog && dialog != g.window) PostMessageW(dialog, WM_CLOSE, 0, 0);
    }
    ShowWindow(g.window, SW_HIDE);
    SetWindowPos(g.window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (restore_focus && !g.nonactivating) ActivateWindow(g.previous_window);
}

void HideOutOfGame() {
    if ((g.open_after || IsWindowVisible(g.window)) && GameFocusLost()) HidePanel(false);
}

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    HideOutOfGame();
}

void Back() {
    if (g.appearance) g.appearance = false;
    else if (g.browsing) g.browsing = false;
    else if (g.move_from >= 0) g.move_from = -1;
    else HidePanel();
}

RectF Rect(float x, float y, float w, float h) {
    return {static_cast<float>(ui::S(x)), static_cast<float>(ui::S(y)),
            static_cast<float>(ui::S(w)), static_cast<float>(ui::S(h))};
}

void Label(Graphics& paint, const std::wstring& value, float x, float y, float width,
           float size = 15, COLORREF color = ui::kText, bool bold = false, bool center = false, BYTE alpha = 255) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, static_cast<float>(ui::S(size)), bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    if (center) format.SetAlignment(Gdiplus::StringAlignmentCenter);
    const auto bounds = Rect(x, y, width, size + 8);
    if (g.text_backdrop && g.transparency >= 65 && !value.empty()) {
        RectF measured;
        paint.MeasureString(value.c_str(), -1, &font, bounds, &format, &measured);
        Gdiplus::SolidBrush readable(Gdiplus::Color(static_cast<BYTE>(220 * alpha / 255), 8, 14, 25));
        measured.Inflate(static_cast<float>(ui::S(4)), static_cast<float>(ui::S(1)));
        paint.FillRectangle(&readable, measured);
    }
    Gdiplus::SolidBrush shadow(Gdiplus::Color(static_cast<BYTE>(245 * alpha / 255), 5, 9, 14));
    Gdiplus::SolidBrush brush(ui::C(color, alpha));
    paint.DrawString(value.c_str(), -1, &font, Rect(x + 1, y + 2, width, size + 8), &format, &shadow);
    paint.DrawString(value.c_str(), -1, &font, bounds, &format, &brush);
}

void ButtonHint(Graphics& paint, Glyph glyph, const wchar_t* key, const wchar_t* caption,
                float x, float y, float width) {
    static const Gdiplus::Rect cells[] = {
        {487, 479, 108, 108}, {487, 369, 108, 108}, {249, 491, 107, 108},
        {372, 247, 108, 107}, {0, 494, 125, 75}, {372, 0, 124, 75},
    };
    constexpr const wchar_t* fallback[][6] = {{L"×", L"○", L"□", L"△", L"L1", L"R1"},
                                              {L"A", L"B", L"X", L"Y", L"LB", L"RB"}};
    const auto index = static_cast<size_t>(glyph);
    const auto& src = cells[index];
    for (int i = 0; i < 2; ++i) {
        if (g.art.buttons[i]) {
            const auto button = Rect(x + i * 24, y, 22, 22);
            const float ratio = static_cast<float>(src.Height) / src.Width;
            const RectF dest(button.X, button.Y + button.Height * (1 - ratio) * .5f, button.Width, button.Height * ratio);
            paint.DrawImage(g.art.buttons[i].get(), dest, static_cast<float>(src.X), static_cast<float>(src.Y),
                            static_cast<float>(src.Width), static_cast<float>(src.Height), Gdiplus::UnitPixel);
        } else {
            Label(paint, fallback[i][index], x + i * 24, y + 2, 22, 13, kQuiet, true, true);
        }
    }
    const float key_width = wcslen(key) > 1 ? 39.0f : 22.0f;
    Gdiplus::SolidBrush key_fill(Gdiplus::Color(205, 22, 28, 45));
    paint.FillRectangle(&key_fill, Rect(x + 50, y + 2, key_width, 18));
    Label(paint, key, x + 50, y + 2, key_width, 12, kQuiet, true, true);
    Label(paint, caption, x + 56 + key_width, y + 2, width - 56 - key_width, 15, kQuiet, true);
}

void ClickArea(Graphics& paint, float x, float y, float w, float h) {
    // Um alvo quase transparente mantém o texto clicável, inclusive nos vãos entre letras.
    Gdiplus::SolidBrush brush(Gdiplus::Color(2, 0, 0, 0));
    paint.FillRectangle(&brush, Rect(x, y, w, h));
}

struct Theme { const wchar_t* pt; const wchar_t* en; COLORREF top, bottom; };
constexpr Theme kThemes[] = {
    {L"Original do LegoToypad", L"Original LegoToypad", RGB(25, 66, 77), RGB(8, 15, 33)},
    {L"Violeta", L"Violet", RGB(71, 45, 93), RGB(16, 12, 37)},
    {L"Grafite", L"Graphite", RGB(49, 57, 68), RGB(11, 16, 25)},
};

bool LoadBackground(const std::wstring& path) {
    std::error_code ec;
    if (fs::file_size(path, ec) > 32 * 1024 * 1024 || ec) return false;
    auto image = std::make_unique<Gdiplus::Bitmap>(path.c_str());
    if (image->GetLastStatus() != Gdiplus::Ok || !image->GetWidth() || !image->GetHeight() ||
        image->GetWidth() > 8192 || image->GetHeight() > 8192) return false;
    g.background = std::move(image);
    g.background_path = path;
    g.backdrop.reset();
    return true;
}

void ReadAppearance() {
    const auto ini = CatalogIni();
    const auto index = GetPrivateProfileIntW(L"Aparencia", L"Tema", 0, ini.c_str());
    if (index < std::size(kThemes)) g.theme = static_cast<int>(index);
    const auto transparency = GetPrivateProfileIntW(L"Aparencia", L"Transparencia", kMaxTransparency, ini.c_str());
    if (transparency <= kMaxTransparency) g.transparency = static_cast<int>(transparency);
    g.text_backdrop = GetPrivateProfileIntW(L"Aparencia", L"FundoDosTextos", 0, ini.c_str()) == 1;
    wchar_t path[32768]{};
    if (GetPrivateProfileStringW(L"Aparencia", L"Imagem", L"", path, static_cast<DWORD>(std::size(path)), ini.c_str()))
        LoadBackground(path);
}

void SaveAppearance() {
    const auto ini = CatalogIni();
    EnsureCatalogIni(ini);
    if (!WritePrivateProfileStringW(L"Aparencia", L"Tema", std::to_wstring(g.theme).c_str(), ini.c_str()) ||
        !WritePrivateProfileStringW(L"Aparencia", L"Imagem", g.background_path.empty() ? L"" : g.background_path.c_str(), ini.c_str()) ||
        !WritePrivateProfileStringW(L"Aparencia", L"Transparencia", std::to_wstring(g.transparency).c_str(), ini.c_str()) ||
        !WritePrivateProfileStringW(L"Aparencia", L"FundoDosTextos", g.text_backdrop ? L"1" : L"0", ini.c_str())) {
        g.background_error = ui::T(L"Não foi possível salvar a aparência.", L"Could not save the appearance.");
    }
}

void ChangeTransparency(int step) {
    const int value = std::clamp(g.transparency + step * 5, 0, kMaxTransparency);
    if (value == g.transparency) return;
    g.transparency = value;
    g.background_error.clear();
    SaveAppearance();
}

void PickBackground() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g.window;
    dialog.lpstrTitle = ui::T(L"Escolher fundo do Toy Pad", L"Choose Toy Pad background");
    dialog.lpstrFilter = ui::T(L"Imagens\0*.png;*.jpg;*.jpeg;*.bmp\0\0", L"Images\0*.png;*.jpg;*.jpeg;*.bmp\0\0");
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    g.choosing_image = true;
    const bool selected = GetOpenFileNameW(&dialog) != FALSE;
    g.choosing_image = false;
    if (g.nonactivating && GetForegroundWindow() == g.window) ActivateWindow(g.game_window);
    if (!selected) return;
    if (!LoadBackground(path)) {
        g.background_error = ui::T(L"Não foi possível abrir essa imagem.", L"Could not open that image.");
        return;
    }
    g.background_error.clear();
    SaveAppearance();
}

void AppearanceAction(int index) {
    if (index == 0) PickBackground();
    else if (index == 1) {
        if (!g.background) g.theme = (g.theme + 1) % static_cast<int>(std::size(kThemes));
        g.background.reset();
        g.background_path.clear();
        g.backdrop.reset();
        g.background_error.clear();
        SaveAppearance();
    } else if (index == kTransparencyRow) {
        ChangeTransparency(g.transparency == kMaxTransparency ? -kMaxTransparency / 5 : 1);
    } else if (index == kTextBackdropRow) {
        g.text_backdrop = !g.text_backdrop;
        g.grid_rows = {};
        g.background_error.clear();
        SaveAppearance();
    } else {
        g.appearance = false;
    }
}

void OpenAppearance() {
    g.appearance = true;
    g.appearance_item = 0;
    g.background_error.clear();
}

void ChangeAppearanceItem(int amount) {
    g.appearance_item = (g.appearance_item + amount + kAppearanceRows) % kAppearanceRows;
    NavSound();
}

void PaintBackdrop(Graphics& paint) {
    if (g.transparency == kMaxTransparency) return;
    const auto view = Rect(0, 0, kWidth, kHeight);
    auto* background = g.background ? g.background.get() : g.theme == 0 ? g.original_background.get() : nullptr;
    if (background) {
        const int w = ui::S(kWidth), h = ui::S(kHeight);
        if (!g.backdrop || g.backdrop->GetWidth() != static_cast<UINT>(w) || g.backdrop->GetHeight() != static_cast<UINT>(h)) {
            auto scaled = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            if (scaled->GetLastStatus() == Gdiplus::Ok) {
                Graphics layer(scaled.get());
                layer.Clear(ui::C(kThemes[g.theme].bottom));
                layer.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                const float source_w = static_cast<float>(background->GetWidth()), source_h = static_cast<float>(background->GetHeight());
                const float scale = std::max(view.Width / source_w, view.Height / source_h);
                const float dest_w = source_w * scale, dest_h = source_h * scale;
                layer.DrawImage(background, RectF((view.Width - dest_w) / 2, (view.Height - dest_h) / 2, dest_w, dest_h));
                Gdiplus::SolidBrush dim(g.background ? Gdiplus::Color(130, 8, 14, 28) : Gdiplus::Color(40, 8, 10, 16));
                layer.FillRectangle(&dim, view);
                g.backdrop = std::move(scaled);
            }
        }
        if (g.backdrop) {
            if (g.transparency == 0) {
                paint.DrawImage(g.backdrop.get(), 0, 0);
            } else {
                const Gdiplus::ColorMatrix opacity = {{
                    {1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0},
                    {0, 0, 0, (kMaxTransparency - g.transparency) / 100.0f, 0}, {0, 0, 0, 0, 1}
                }};
                Gdiplus::ImageAttributes attributes;
                attributes.SetColorMatrix(&opacity);
                paint.DrawImage(g.backdrop.get(), Gdiplus::Rect(0, 0, w, h), 0, 0, w, h, Gdiplus::UnitPixel, &attributes);
            }
            return;
        }
    }
    const auto& theme = kThemes[g.theme];
    const auto alpha = static_cast<BYTE>(((kMaxTransparency - g.transparency) * 255 + 50) / 100);
    Gdiplus::LinearGradientBrush gradient(view, ui::C(theme.top, alpha), ui::C(theme.bottom, alpha),
                                          Gdiplus::LinearGradientModeForwardDiagonal);
    paint.FillRectangle(&gradient, view);
}

// AddRoundedRectPath / AddPadPath, adaptadas da tela original (MIT).
void AddPadShape(Gdiplus::GraphicsPath& shape, const RectF& rect, float radius, bool circular = false) {
    if (circular) {
        shape.AddEllipse(rect);
        return;
    }
    const float arc = static_cast<float>(ui::S(radius * 2));
    shape.AddArc(rect.X, rect.Y, arc, arc, 180, 90);
    shape.AddArc(rect.GetRight() - arc, rect.Y, arc, arc, 270, 90);
    shape.AddArc(rect.GetRight() - arc, rect.GetBottom() - arc, arc, arc, 0, 90);
    shape.AddArc(rect.X, rect.GetBottom() - arc, arc, arc, 90, 90);
    shape.CloseFigure();
}

void Card(Graphics& paint, float x, float y, float w, float h, bool active, COLORREF accent = RGB(111, 167, 191)) {
    const auto rect = Rect(x, y, w, h);
    Gdiplus::GraphicsPath shape;
    AddPadShape(shape, rect, 6);
    Gdiplus::SolidBrush fill(active ? Gdiplus::Color(235, 31, 40, 61) : Gdiplus::Color(177, 12, 24, 42));
    Gdiplus::Pen edge(ui::C(active ? kFocus : accent, active ? 255 : 165), static_cast<float>(ui::S(active ? 2.5f : 1)));
    paint.FillPath(&fill, &shape);
    paint.DrawPath(&edge, &shape);
}

Gdiplus::Rect PortraitBounds(Gdiplus::Bitmap& icon) {
    const Gdiplus::Rect full(0, 0, static_cast<int>(icon.GetWidth()), static_cast<int>(icon.GetHeight()));
    Gdiplus::BitmapData data{};
    if (icon.LockBits(&full, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return full;
    int left = full.Width, top = full.Height, right = 0, bottom = 0;
    for (int y = 0; y < full.Height; ++y) {
        const auto* row = static_cast<const BYTE*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < full.Width; ++x) {
            if (row[x * 4 + 3] < 32) continue;
            left = std::min(left, x);
            right = std::max(right, x + 1);
            top = std::min(top, y);
            bottom = y + 1;
        }
    }
    icon.UnlockBits(&data);
    return right > left && bottom > top ? Gdiplus::Rect(left, top, right - left, bottom - top) : full;
}

void DrawSprite(Graphics& paint, Gdiplus::Bitmap& image, const RectF& dest, const Gdiplus::Rect& src, BYTE alpha) {
    if (alpha == 255) {
        paint.DrawImage(&image, dest, static_cast<float>(src.X), static_cast<float>(src.Y),
                        static_cast<float>(src.Width), static_cast<float>(src.Height), Gdiplus::UnitPixel);
        return;
    }
    const Gdiplus::ColorMatrix opacity = {{
        {1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha / 255.0f, 0}, {0, 0, 0, 0, 1}
    }};
    Gdiplus::ImageAttributes attributes;
    attributes.SetColorMatrix(&opacity);
    paint.DrawImage(&image, dest, static_cast<float>(src.X), static_cast<float>(src.Y),
                    static_cast<float>(src.Width), static_cast<float>(src.Height), Gdiplus::UnitPixel, &attributes);
}

void Portrait(Graphics& paint, Figure& figure, float x, float y, float diameter, COLORREF color, bool selected, bool map,
              BYTE alpha = 255) {
    const auto id = figure.id;
    if (!map && g.art.roster && id >= 1 && id <= 77 && id != 58 && id != 73) {
        const int index = static_cast<int>(id) - 1 - (id > 58) - (id > 73);
        constexpr float ring_fraction = 472.0f / 512.0f;
        const float tile = diameter / ring_fraction, margin = (tile - diameter) * .5f;
        if (selected) {
            Gdiplus::Pen focus(ui::C(kFocus, alpha), static_cast<float>(ui::S(2.0f)));
            paint.DrawEllipse(&focus, Rect(x - 3, y - 3, diameter + 6, diameter + 6));
        }
        DrawSprite(paint, *g.art.roster, Rect(x - margin, y - margin, tile, tile),
                   {index % 10 * kRosterTile, index / 10 * kRosterTile, kRosterTile, kRosterTile}, alpha);
        return;
    }
    const auto opacity = [alpha](int base) { return static_cast<BYTE>(base * alpha / 255); };
    const auto ring = Rect(x, y, diameter, diameter);
    if (!map) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(opacity(178), 8, 16, 29));
        paint.FillEllipse(&fill, ring);
    }
    Gdiplus::LinearGradientBrush gloss(ring, Gdiplus::Color(opacity(40), 255, 255, 255),
                                     Gdiplus::Color(0, 255, 255, 255), Gdiplus::LinearGradientModeForwardDiagonal);
    paint.FillEllipse(&gloss, ring);
    Gdiplus::Pen rim(ui::C(color, opacity(selected ? 255 : 170)), static_cast<float>(ui::S(selected ? 2.5f : map ? 2.0f : 1.0f)));
    paint.DrawEllipse(&rim, ring);

    if (!figure.tried) {
        figure.tried = true;
        auto icon = std::make_unique<Gdiplus::Bitmap>((g.images / (std::to_wstring(figure.id) + L".png")).c_str());
        if (icon->GetLastStatus() == Gdiplus::Ok) {
            if (figure.id < 1000) figure.source = PortraitBounds(*icon);
            figure.icon = std::move(icon);
        }
    }
    if (!figure.icon) {
        Label(paint, figure.name.substr(0, 2), x, y + diameter * .23f, diameter, diameter * .33f, kFocus, true, true, alpha);
        return;
    }
    // Chapéus, orelhas, coroas e laços que ultrapassam o aro na referência.
    constexpr std::uint32_t overhang_ids[] = {1, 2, 17, 27, 32, 37, 41, 42, 57, 64, 69, 74, 75};
    const bool overhang = std::find(std::begin(overhang_ids), std::end(overhang_ids), figure.id) != std::end(overhang_ids);
    const auto src = figure.id >= 1000
        ? Gdiplus::Rect(0, 0, static_cast<int>(figure.icon->GetWidth()), static_cast<int>(figure.icon->GetHeight())) : figure.source;
    const float width = static_cast<float>(src.Width);
    const float height = static_cast<float>(src.Height);
    if (width <= 0 || height <= 0) return;
    const float scale = std::min(diameter * (overhang ? 1.14f : .92f) / height,
                                 diameter * (overhang ? 1.25f : .92f) / width);
    const float w = width * scale, h = height * scale;
    constexpr float inset = 1.5f;
    const auto inside = Rect(x + inset, y + inset, diameter - 2 * inset, diameter - 2 * inset);
    Gdiplus::GraphicsPath mask;
    if (overhang) {
        const float margin = diameter * .19f;
        const float left = static_cast<float>(ui::S(x - margin));
        const float right = static_cast<float>(ui::S(x + diameter + margin));
        const float top = static_cast<float>(ui::S(y - diameter * .18f));
        const float shoulders = static_cast<float>(ui::S(y + diameter * .38f));
        const float middle = inside.Y + inside.Height * .5f;
        mask.AddLine(left, top, right, top);
        mask.AddLine(right, top, right, shoulders);
        mask.AddLine(right, shoulders, inside.GetRight(), middle);
        mask.AddArc(inside, 0, 180);
        mask.AddLine(inside.X, middle, left, shoulders);
        mask.CloseFigure();
    } else {
        mask.AddEllipse(inside);
    }
    const float draw_y = overhang ? y + diameter * 1.025f - h : y + (diameter - h) * .5f;
    const auto state = paint.Save();
    paint.SetClip(&mask, Gdiplus::CombineModeIntersect);
    DrawSprite(paint, *figure.icon, Rect(x + (diameter - w) * .5f, draw_y, w, h), src, alpha);
    paint.Restore(state);
}

struct FigureGrid {
    int columns = kColumns;
    int count = 0, rows = 0;
    float scroll = 0;
    float cell_w = 0, cell_h = 0, diameter = 0;

    FigureGrid(bool vehicles = g.vehicles, int total = static_cast<int>(g.list.size()), float offset = g.scroll)
        : count(total), scroll(vehicles ? offset : 0) {
        // A grade de personagens cresce com o elenco, mantendo todos na mesma tela.
        while (!vehicles && count > columns * (columns / 2 + 1)) ++columns;
        rows = (count + columns - 1) / columns;
        cell_w = kGridWidth / columns;
        cell_h = vehicles ? kVehicleRow : kGridHeight / std::max(rows, 1);
        diameter = std::min(46.0f, std::min(cell_w, cell_h) - 7);
    }

    float Left(int row) const {
        const int visible = std::min(columns, count - row * columns);
        return kGridX + (kGridWidth - visible * cell_w) * .5f;
    }

    int Hit(float x, float y) const {
        if (!count || x < kGridX || y < kGridY || y >= kGridY + kGridHeight) return -1;
        const int row = static_cast<int>((y - kGridY + scroll) / cell_h);
        if (row >= rows) return -1;
        const float left = Left(row);
        if (x < left) return -1;
        const int column = static_cast<int>((x - left) / cell_w);
        const int item = row * columns + column;
        return column < columns && item < count ? item : -1;
    }
};

Figure* FindFigure(std::uint32_t id) {
    if (!id) return nullptr;
    const auto it = std::find_if(g.figures.begin(), g.figures.end(), [id](const Figure& f) { return f.id == id; });
    return it == g.figures.end() ? nullptr : &*it;
}

// RenderPad e DrawPadOccupant, adaptadas da tela PadViewer do LegoToypad (MIT).
void PaintOriginalPad(Graphics& paint, int position) {
    const auto& at = kPositions[position];
    const float x = at.x - at.w / 2, y = at.y - at.h / 2;
    const auto rect = Rect(x, y, at.w, at.h);
    Gdiplus::GraphicsPath shape;
    AddPadShape(shape, rect, 14, position == 0);

    const int slot = g.positions[position];
    auto* figure = FindFigure(SlotId(slot));
    const bool selected = position == g.position;
    const bool moving = slot == g.move_from;
    constexpr COLORREF focus_color = RGB(255, 72, 72);
    const COLORREF color = figure ? figure->color : RGB(106, 90, 82);
    if (selected || moving) {
        const auto glow_color = moving ? RGB(65, 141, 255) : focus_color;
        for (int width = 22; width >= 4; width -= 2) {
            Gdiplus::Pen glow(ui::C(glow_color, static_cast<BYTE>(8 + (22 - width) * 2)),
                              static_cast<float>(ui::S(static_cast<float>(width))));
            glow.SetLineJoin(Gdiplus::LineJoinRound);
            paint.DrawPath(&glow, &shape);
        }
    }
    if (g.original_pads[position]) {
        const auto state = paint.Save();
        paint.SetClip(&shape);
        paint.DrawImage(g.original_pads[position].get(), rect);
        paint.Restore(state);
    } else {
        Gdiplus::SolidBrush fallback(Gdiplus::Color(230, 37, 53, 91));
        paint.FillPath(&fallback, &shape);
    }
    if (figure) {
        Gdiplus::SolidBrush tint(ui::C(color, 42));
        paint.FillPath(&tint, &shape);
    }
    if (selected) {
        Gdiplus::Pen wide(ui::C(focus_color, 80), static_cast<float>(ui::S(7.5f)));
        wide.SetLineJoin(Gdiplus::LineJoinRound);
        paint.DrawPath(&wide, &shape);
    }
    Gdiplus::Pen border(ui::C(selected ? focus_color : moving ? RGB(65, 141, 255) : color, selected ? 225 : 255),
                        static_cast<float>(ui::S(selected ? 2.4f : 2.0f)));
    paint.DrawPath(&border, &shape);
    if (figure) {
        const float diameter = std::clamp(std::min(at.w, at.h) * .80f, 84.0f, 148.0f);
        const float px = at.x - diameter / 2, py = at.y - diameter / 2;
        Portrait(paint, *figure, px, py, diameter, color, false, true);
    }
}

void PaintPadMap(Graphics& paint) {
    if (g.original_wordmark) {
        const float width = 90.0f * g.original_wordmark->GetWidth() / g.original_wordmark->GetHeight();
        paint.DrawImage(g.original_wordmark.get(), Rect(59, 40, width, 90));
    } else {
        Label(paint, L"Toy Pad", 59, 60, 260, 26, ui::kText, true);
    }
    for (int i = 0; i < wire::kSlots; ++i) PaintOriginalPad(paint, i);
    if (g.original_credit) {
        const float width = 30.0f * g.original_credit->GetWidth() / g.original_credit->GetHeight();
        paint.DrawImage(g.original_credit.get(), Rect(919 - width, 70, width, 30));
    } else {
        Label(paint, L"By Harrysof", 811, 70, 108, 20, ui::kText, true);
    }
    ClickArea(paint, 742, 567, 184, 49);
    ButtonHint(paint, Glyph::Triangle, L"S", L"Settings", 749, 580, 176);
    ButtonHint(paint, Glyph::Square, L"M", ui::T(L"Mover", L"Move"), 59, 580, 155);
    ButtonHint(paint, Glyph::L1, L"Del", ui::T(L"Retirar", L"Remove"), 223, 580, 166);
}

void PaintAppearance(Graphics& paint) {
    Label(paint, ui::T(L"Aparência do Toy Pad", L"Toy Pad appearance"), 193, 143, 580, 28, kFocus, true, true);
    const auto& theme = kThemes[g.theme];
    const std::wstring mode = g.background ? ui::T(L"Usar tema sem imagem", L"Use theme without image")
        : ui::T(L"Trocar tema · ", L"Cycle theme · ") + std::wstring(ui::T(theme.pt, theme.en));
    const auto transparency = ui::T(L"Transparência do fundo · ", L"Background transparency · ") +
                              std::to_wstring(g.transparency) + L"%";
    const std::wstring readability = g.text_backdrop
        ? ui::T(L"Fundo dos textos · Ligado", L"Text backing · On")
        : ui::T(L"Fundo dos textos · Desligado", L"Text backing · Off");
    const std::array<std::wstring, kAppearanceRows> options{
        ui::T(L"Escolher imagem…", L"Choose image…"), mode, transparency, readability, ui::T(L"Voltar", L"Back")};
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const float y = kAppearanceTop + i * kAppearanceStep;
        const bool focused = g.appearance_item == i;
        Card(paint, 288, y, 390, 53, focused);
        Label(paint, options[i], 303, y + 13, 360, 19, focused ? kFocus : kQuiet, true, true);
        if (i == kTransparencyRow) {
            Label(paint, L"‹", 301, y + 9, 37, 25, g.transparency > 0 ? kFocus : ui::kMuted, true, true);
            Label(paint, L"›", 628, y + 9, 37, 25, g.transparency < kMaxTransparency ? kFocus : ui::kMuted, true, true);
        }
    }
    if (!g.background_error.empty()) Label(paint, g.background_error, 183, 504, 600, 15, kQuiet, true, true);
    ButtonHint(paint, Glyph::Cross, L"Enter", ui::T(L"Escolher", L"Select"), 308, 559, 180);
    ButtonHint(paint, Glyph::Circle, L"Esc", ui::T(L"Voltar", L"Back"), 505, 559, 165);
    Label(paint, L"harrysof · João Pedro Favoni", 42, 603, 390, 13, kQuiet);
}

float CatalogOffsetY(ULONGLONG now = AnimationNow()) {
    return 58.0f * (1.0f - Ease(g.catalog_opened, kCatalogRiseMs, now));
}

constexpr float kGridRowPadding = 4.0f;

Gdiplus::Bitmap* GridRow(bool vehicles, const FigureGrid& grid, const std::vector<size_t>& list, int row) {
    if (g.grid_scale != ui::g_scale) {
        g.grid_rows = {};
        g.grid_scale = ui::g_scale;
    }
    auto& rows = g.grid_rows[vehicles ? 1 : 0];
    if (rows.size() != static_cast<size_t>(grid.rows)) rows.resize(grid.rows);
    auto& image = rows[row];
    if (image) return image.get();
    auto layer = std::make_unique<Gdiplus::Bitmap>(ui::S(kWidth), ui::S(grid.cell_h + 2 * kGridRowPadding),
                                                   PixelFormat32bppPARGB);
    if (layer->GetLastStatus() != Gdiplus::Ok) return nullptr;
    {
        Graphics raster(layer.get());
        raster.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        raster.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        raster.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        const int first = row * grid.columns, end = std::min(first + grid.columns, grid.count);
        const float left = grid.Left(row);
        for (int i = first; i < end; ++i) {
            auto& figure = g.figures[list[i]];
            const float x = left + (i - first + .5f) * grid.cell_w - grid.diameter * .5f;
            const float y = kGridRowPadding + (grid.cell_h - grid.diameter) * .5f;
            Portrait(raster, figure, x, y, grid.diameter, figure.color, false, false);
        }
    }
    image = std::move(layer);
    return image.get();
}

void WarmGridRow() {
    if (g.art_loading.valid() || !g.loaded) return;
    for (const bool vehicles : {false, true}) {
        auto& cached = g.grid_rows[vehicles ? 1 : 0];
        if (g.grid_scale == ui::g_scale && !cached.empty() &&
            std::all_of(cached.begin(), cached.end(), [](const auto& row) { return row != nullptr; })) continue;
        const auto list = CatalogList(vehicles);
        const FigureGrid grid(vehicles, static_cast<int>(list.size()), 0);
        for (int row = 0; row < grid.rows; ++row) {
            if (g.grid_scale != ui::g_scale || row >= static_cast<int>(cached.size()) || !cached[row]) {
                GridRow(vehicles, grid, list, row);
                return;
            }
        }
    }
}

float VehicleRowOpacity(float pixels_below, bool more_below) {
    if (!more_below) return 1.0f;
    // A borda mostra a próxima fileira e atenua discretamente a que vem antes.
    return std::clamp(.22f + pixels_below / (1.65f * kVehicleRow), .22f, 1.0f);
}

void PaintBrowseList(Graphics& paint, const std::vector<size_t>& list, bool vehicles, int selected, float scroll, BYTE alpha) {
    const FigureGrid grid(vehicles, static_cast<int>(list.size()), scroll);
    if (!grid.count || alpha == 0) return;
    auto& current = g.figures[list[selected]];
    constexpr float center = kWidth * .5f;
    Portrait(paint, current, center - 40, 16, 80, kFocus, true, false, alpha);
    Label(paint, current.name, 242, 101, 482, 25, kFocus, true, true, alpha);
    const auto state = paint.Save();
    if (vehicles) paint.SetClip(Rect(kGridX - 3, kGridY, kGridWidth + 6, kGridHeight), Gdiplus::CombineModeIntersect);
    paint.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    const int first_row = vehicles ? std::max(0, static_cast<int>(scroll / grid.cell_h) - 1) : 0;
    const bool more_below = vehicles && grid.rows * grid.cell_h - scroll > kGridHeight + 1.0f;
    for (int row = first_row; row < grid.rows; ++row) {
        const float y = kGridY + (row + .5f) * grid.cell_h - grid.scroll;
        if (vehicles && y - grid.cell_h * .5f >= kGridY + kGridHeight) break;
        if (vehicles && y + grid.cell_h * .5f < kGridY) continue;
        const auto row_alpha = static_cast<BYTE>(alpha * VehicleRowOpacity(kGridY + kGridHeight - y, more_below));
        if (auto* image = GridRow(vehicles, grid, list, row)) {
            const int width = static_cast<int>(image->GetWidth()), height = static_cast<int>(image->GetHeight());
            const auto top = static_cast<float>(ui::S(y - grid.cell_h * .5f - kGridRowPadding));
            DrawSprite(paint, *image, {0, top, static_cast<float>(width), static_cast<float>(height)},
                       {0, 0, width, height}, row_alpha);
        }
        if (row == selected / grid.columns) {
            const float x = grid.Left(row) + (selected % grid.columns + .5f) * grid.cell_w;
            const float diameter = grid.diameter + 5.0f;
            Gdiplus::Pen focus(ui::C(kFocus, row_alpha), static_cast<float>(ui::S(2.0f)));
            paint.DrawEllipse(&focus, Rect(x - diameter * .5f, y - diameter * .5f, diameter, diameter));
        }
    }
    paint.Restore(state);
}

void Paint(Graphics& paint) {
    paint.Clear(Gdiplus::Color(0, 0, 0, 0));
    PaintBackdrop(paint);
    paint.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    paint.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    paint.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    ClickArea(paint, 927, 8, 35, 35);
    Label(paint, L"×", 927, 11, 35, 21, kQuiet, true, true);
    if (g.appearance) {
        PaintAppearance(paint);
        return;
    }
    if (!g.browsing) {
        PaintPadMap(paint);
        return;
    }
    const auto content = paint.Save();
    paint.TranslateTransform(0, static_cast<float>(ui::S(CatalogOffsetY())));
    if (!g.portal.connected)
        Label(paint, ui::T(L"Aguardando o jogo", L"Waiting for the game"), 764, 69, 163, 14, kQuiet, false, true);
    else if (g.command_in_flight || g.action)
        Label(paint, ui::T(L"Atualizando portal…", L"Updating portal…"), 764, 69, 163, 14, kQuiet, false, true);
    Card(paint, 764, 30, 163, 33, false);
    ButtonHint(paint, Glyph::Triangle, L"S", L"Settings", 773, 35, 149);
    ClickArea(paint, 32, 25, 200, 30);
    ClickArea(paint, 32, 58, 113, 29);
    ClickArea(paint, 148, 58, 144, 29);
    Label(paint, ui::T(L"‹ Toy Pad", L"‹ Toy Pad"), 33, 27, 200, 18, kQuiet, true);
    Label(paint, ui::T(L"Personagens", L"Characters"), 33, 61, 113, 16, g.vehicles ? kQuiet : kFocus, true);
    Label(paint, ui::T(L"Veículos e itens", L"Vehicles & items"), 151, 61, 150, 16, g.vehicles ? kFocus : kQuiet, true);
    ButtonHint(paint, Glyph::R1, L"Tab", ui::T(L"Alternar", L"Switch"), 33, 88, 174);

    if (g.list.empty()) {
        const auto text = g.loaded ? ui::T(L"Nenhum item encontrado.", L"No items found.")
            : g.extraction_error ? ui::T(L"Falha ao preparar os retratos. Reabra o Toy Pad.", L"Portrait setup failed. Reopen the Toy Pad.")
            : ui::T(L"Baixando os retratos do LegoToypad…", L"Downloading LegoToypad portraits…");
        Label(paint, text, 80, 282, kWidth - 160, 19, kQuiet, true, true);
    } else {
        const float t = g.previous_list.empty() ? 1.0f : Ease(g.tab_started, kTabSlideMs);
        const float direction = g.vehicles ? 1.0f : -1.0f;
        if (t < 1.0f) {
            const auto outgoing = paint.Save();
            paint.SetClip(Rect(0, 0, kWidth, kGridY + kGridHeight), Gdiplus::CombineModeIntersect);
            paint.TranslateTransform(static_cast<float>(ui::S(-direction * kWidth * t)), 0);
            PaintBrowseList(paint, g.previous_list, !g.vehicles, g.previous_selected, g.previous_scroll,
                            255);
            paint.Restore(outgoing);
        }
        const auto incoming = paint.Save();
        if (t < 1.0f) paint.SetClip(Rect(0, 0, kWidth, kGridY + kGridHeight), Gdiplus::CombineModeIntersect);
        paint.TranslateTransform(static_cast<float>(ui::S(direction * kWidth * (1 - t))), 0);
        PaintBrowseList(paint, g.list, g.vehicles, g.selected, g.scroll, 255);
        paint.Restore(incoming);
    }

    Label(paint, std::wstring(ZoneName(g.zone)) + L" · " + std::to_wstring(g.position + 1),
          35, 565, 315, 17, kQuiet, true);
    ClickArea(paint, 690, 559, 175, 31);
    ButtonHint(paint, Glyph::Cross, L"Enter", ui::T(L"Colocar", L"Place"), 690, 565, 175);
    ButtonHint(paint, Glyph::Circle, L"Esc", ui::T(L"Voltar", L"Back"), 780, 594, 145);
    paint.Restore(content);
}

void RenderWindow() {
    auto& surface = g.surface;
    if (!surface.pixels) return;
    {
        Gdiplus::Bitmap bitmap(surface.width, surface.height, surface.width * 4,
                               PixelFormat32bppPARGB, surface.pixels);
        Graphics paint(&bitmap);
        Paint(paint);
    }
    SIZE size{surface.width, surface.height};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.window, nullptr, nullptr, &size, surface.dc, &origin, 0, &blend, ULW_ALPHA);
}

void Poll() {
    HideOutOfGame();
    const auto now = GetTickCount64();
    const auto animation_now = AnimationNow();
    if (g.open_after && now >= g.open_after) ShowPanel();
    if (g.art_loading.valid() && g.art_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g.art = g.art_loading.get();
        g.grid_rows = {};
        InvalidateRect(g.window, nullptr, FALSE);
    }
    if (g.extraction && WaitForSingleObject(g.extraction, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(g.extraction, &code);
        CloseHandle(g.extraction);
        g.extraction = nullptr;
        g.loaded = code == 0 && ReadCatalog();
        g.extraction_error = !g.loaded;
        if (code == 0) ReadOriginalSkin();
        InvalidateRect(g.window, nullptr, FALSE);
    }
    if (g.request.valid() && g.request.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto previous = g.portal;
        g.portal = g.request.get();
        if (previous.connected != g.portal.connected || previous.slots != g.portal.slots || g.command_in_flight) {
            MapSlots();
            if (g.move_from >= 0 && !SlotPad(g.move_from)) g.move_from = -1;
            InvalidateRect(g.window, nullptr, FALSE);
        }
        g.command_in_flight = false;
    }
    if ((g.action || (IsWindowVisible(g.window) && now >= g.next_query)) && !g.request.valid()) {
        g.command_in_flight = g.action.has_value();
        g.request = std::async(std::launch::async, Exchange, g.action.value_or(Bytes{}), g.port);
        g.action.reset();
        g.next_query = now + 750;
    }
    XINPUT_GAMEPAD pad = g_sony_pad.load(std::memory_order_relaxed);
    bool back_tapped = g_sony_back.exchange(false, std::memory_order_relaxed);
    for (DWORD i = 0; g_get_pad && i < XUSER_MAX_COUNT; ++i) {
        if (now < g_pad_retry[i]) continue;
        XINPUT_STATE state{};
        if (g_get_pad(i, &state) != ERROR_SUCCESS) {
            g_pad_retry[i] = now + 250;
            continue;
        }
        for (int remaining = 16; g_get_pad_key && remaining > 0; --remaining) {
            XINPUT_KEYSTROKE key{};
            if (g_get_pad_key(i, 0, &key) != ERROR_SUCCESS) break;
            if (key.VirtualKey == VK_PAD_BACK && (key.Flags & (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT)) ==
                                                     XINPUT_KEYSTROKE_KEYDOWN) back_tapped = true;
        }
        pad.wButtons |= state.Gamepad.wButtons;
        pad.bLeftTrigger = std::max(pad.bLeftTrigger, state.Gamepad.bLeftTrigger);
        pad.bRightTrigger = std::max(pad.bRightTrigger, state.Gamepad.bRightTrigger);
        if (pad.sThumbLX >= -17000 && pad.sThumbLX <= 17000 && pad.sThumbLY >= -17000 && pad.sThumbLY <= 17000) {
            pad.sThumbLX = state.Gamepad.sThumbLX;
            pad.sThumbLY = state.Gamepad.sThumbLY;
        }
        break;
    }
    const WORD buttons = pad.wButtons;
    WORD pressed = static_cast<WORD>(buttons & ~g.previous);
    if (back_tapped) pressed |= XINPUT_GAMEPAD_BACK;
    g.previous = buttons;
    const HWND foreground = GetForegroundWindow();
    if (!IsWindowVisible(g.window) || IsIconic(g.window)) {
        if ((pressed & XINPUT_GAMEPAD_BACK) && !g.open_after && IsGameWindow(foreground)) RequestPanel();
        else if (!g.open_after) WarmGridRow();
        return;
    }
    if (g.started) {
        const float distance = 1.0f - Ease(g.started, kRiseMs, animation_now);
        SetWindowPos(g.window, nullptr, g.placement.left, g.placement.top + ui::S(kRise * distance),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (distance == 0.0f) g.started = 0;
    }
    if (g.browsing && !g.appearance) {
        bool redraw = animation_now - g.catalog_opened <= kCatalogRiseMs + 32;
        if (g.tab_started) {
            if (Ease(g.tab_started, kTabSlideMs, animation_now) == 1.0f) {
                g.tab_started = 0;
                g.previous_list.clear();
            }
            redraw = true;
        }
        if (g.scroll_started) {
            const float t = Ease(g.scroll_started, kScrollMs, animation_now);
            g.scroll = std::lerp(g.scroll_from, g.scroll_target, t);
            if (t == 1.0f) g.scroll_started = 0;
            redraw = true;
        }
        if (redraw) InvalidateRect(g.window, nullptr, FALSE);
    }
    if (g.choosing_image || (foreground != g.window && (!g.game_window || foreground != g.game_window))) return;
    if ((pressed & XINPUT_GAMEPAD_BACK) && now - g.panel_opened >= 250) { HidePanel(); return; }
    if (pressed & XINPUT_GAMEPAD_B) { Back(); InvalidateRect(g.window, nullptr, FALSE); return; }
    if (pressed & XINPUT_GAMEPAD_Y) {
        if (g.appearance) Back(); else OpenAppearance();
        InvalidateRect(g.window, nullptr, FALSE);
        return;
    }
    if (!g.appearance) {
        if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) Page(!g.vehicles);
        if (pressed & XINPUT_GAMEPAD_X) PickUp();
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ClearPosition();
    }
    const int dir = (buttons & XINPUT_GAMEPAD_DPAD_UP) || pad.sThumbLY > 17000 ? -2
        : (buttons & XINPUT_GAMEPAD_DPAD_DOWN) || pad.sThumbLY < -17000 ? 2
        : (buttons & XINPUT_GAMEPAD_DPAD_LEFT) || pad.sThumbLX < -17000 ? -1
        : (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || pad.sThumbLX > 17000 ? 1 : 0;
    if (dir && (dir != g.previous_dir || now >= g.repeat)) {
        const int dx = std::abs(dir) == 1 ? dir : 0, dy = std::abs(dir) == 2 ? dir / 2 : 0;
        if (g.appearance && dy) ChangeAppearanceItem(dy);
        else if (g.appearance && g.appearance_item == kTransparencyRow && dx) ChangeTransparency(dx);
        else if (g.browsing && !g.appearance) Select(dx + dy * FigureGrid{}.columns);
        else if (!g.appearance) FocusPosition(dx, dy);
        g.repeat = now + (dir != g.previous_dir ? 260 : 75);
        InvalidateRect(g.window, nullptr, FALSE);
    }
    g.previous_dir = dir;
    if (pressed & XINPUT_GAMEPAD_A) {
        if (g.appearance) AppearanceAction(g.appearance_item); else Confirm();
    }
    if (pressed) InvalidateRect(g.window, nullptr, FALSE);
    else if (!g.browsing && !g.appearance) WarmGridRow();
}

LRESULT CALLBACK Window(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case wire::kControllerOpen:
        OpenForGame(reinterpret_cast<HWND>(wp), lp != 0);
        return 0;
    case wire::kShowMessage:
        if (!wp) RequestPanel(true);
        else if (g.open_after && reinterpret_cast<HWND>(wp) == g.game_window) {
            g.nonactivating = lp != 0 || GameCoversMonitor(g.game_window);
            ShowPanel();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        if (g.nonactivating) return MA_NOACTIVATE;
        break;
    case WM_NCHITTEST:
        if (!g.nonactivating) {
            POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &at);
            if (at.x >= 0 && at.x < ui::S(927) && at.y >= 0 && at.y < ui::S(16)) return HTCAPTION;
        }
        break;
    case WM_SIZE:
        if (IsWindowVisible(hwnd)) SetInputOwnership(wp != SIZE_MINIMIZED);
        return 0;
    case WM_HOTKEY:
        if (g.choosing_image) return 0;
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) HidePanel();
        else RequestPanel();
        return 0;
    case WM_TIMER: Poll(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        RenderWindow();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (g.appearance) {
            switch (wp) {
            case VK_ESCAPE: case 'S': Back(); break;
            case VK_UP: ChangeAppearanceItem(-1); break;
            case VK_DOWN: ChangeAppearanceItem(1); break;
            case VK_LEFT: if (g.appearance_item == kTransparencyRow) ChangeTransparency(-1); break;
            case VK_RIGHT: if (g.appearance_item == kTransparencyRow) ChangeTransparency(1); break;
            case VK_RETURN: if (!(lp & (1LL << 30))) AppearanceAction(g.appearance_item); break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        switch (wp) {
        case VK_ESCAPE: Back(); break;
        case 'S': OpenAppearance(); break;
        case VK_TAB: Page(!g.vehicles); break;
        case VK_LEFT: if (g.browsing) Select(-1); else FocusPosition(-1, 0); break;
        case VK_RIGHT: if (g.browsing) Select(1); else FocusPosition(1, 0); break;
        case VK_UP: if (g.browsing) Select(-FigureGrid{}.columns); else FocusPosition(0, -1); break;
        case VK_DOWN: if (g.browsing) Select(FigureGrid{}.columns); else FocusPosition(0, 1); break;
        case VK_PRIOR: if (g.browsing && g.vehicles) Select(-kColumns * 6); break;
        case VK_NEXT: if (g.browsing && g.vehicles) Select(kColumns * 6); break;
        case VK_RETURN: if (!(lp & (1LL << 30))) Confirm(); break;
        case VK_DELETE: ClearPosition(); break;
        case 'M': PickUp(); break;
        default:
            if (!g.browsing && wp >= '1' && wp <= '7' && g.position != static_cast<int>(wp - '1')) {
                g.position = static_cast<int>(wp - '1');
                NavSound();
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const float x = GET_X_LPARAM(lp) / ui::g_scale;
        const float mouse_y = GET_Y_LPARAM(lp) / ui::g_scale;
        const float y = mouse_y - (g.browsing && !g.appearance ? CatalogOffsetY() : 0.0f);
        if (x >= 927 && mouse_y >= 8 && mouse_y < 43) {
            if (message == WM_LBUTTONDOWN) HidePanel();
            return 0;
        }
        if (g.appearance) {
            if (message == WM_LBUTTONDOWN && x >= 288 && x < 678) {
                for (int i = 0; i < kAppearanceRows; ++i) {
                    const float top = kAppearanceTop + i * kAppearanceStep;
                    if (y >= top && y < top + 53) {
                        g.appearance_item = i;
                        if (i == kTransparencyRow) {
                            if (x < 348) ChangeTransparency(-1);
                            else if (x >= 618) ChangeTransparency(1);
                        } else {
                            AppearanceAction(i);
                        }
                        break;
                    }
                }
            }
        } else if (!g.browsing && x >= 742 && x < 926 && y >= 566 && y < 618) {
            if (message == WM_LBUTTONDOWN) OpenAppearance();
        } else if (g.browsing && x >= 764 && x < 927 && y >= 30 && y < 63) {
            if (message == WM_LBUTTONDOWN) OpenAppearance();
        } else if (g.browsing) {
            if (message == WM_LBUTTONDBLCLK && AnimationNow() - g.catalog_opened < GetDoubleClickTime()) return 0;
            if (y >= 25 && y < 55 && x >= 32 && x < 232) Back();
            if (y >= 58 && y < 87 && x >= 32 && x < 292) Page(x >= 148);
            const float offset_x = (g.vehicles ? 1.0f : -1.0f) * kWidth * (1.0f - Ease(g.tab_started, kTabSlideMs));
            if (const int item = FigureGrid{}.Hit(x - offset_x, y); item >= 0) {
                FocusFigure(item);
                if (message == WM_LBUTTONDBLCLK) Confirm();
            }
            if (y >= 559 && y < 590 && x >= 690 && x < 865) Confirm();
        } else if (message == WM_LBUTTONDOWN) {
            for (int p = 0; p < wire::kSlots; ++p) {
                const float dx = std::abs(x - kPositions[p].x), dy = std::abs(y - kPositions[p].y);
                if (dx > PadWidth(p) * .5f || dy > PadHeight(p) * .5f) continue;
                g.position = p;
                Confirm();
                break;
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (g.appearance || g.browsing) return 0;
        const float x = GET_X_LPARAM(lp) / ui::g_scale, y = GET_Y_LPARAM(lp) / ui::g_scale;
        for (int p = 0; p < wire::kSlots; ++p) {
            const auto& at = kPositions[p];
            if (std::abs(x - at.x) > at.w / 2 || std::abs(y - at.y) > at.h / 2) continue;
            g.position = p;
            const UINT enabled = SlotPad(g.positions[p]) ? MF_ENABLED : MF_GRAYED;
            if (HMENU menu = CreatePopupMenu()) {
                AppendMenuW(menu, MF_STRING, 1, ui::T(L"Abrir catálogo", L"Open catalog"));
                AppendMenuW(menu, MF_STRING | enabled, 2, ui::T(L"Mover", L"Move"));
                AppendMenuW(menu, MF_STRING | enabled, 3, ui::T(L"Retirar", L"Remove"));
                POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &screen);
                const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (action == 1) Confirm();
                else if (action == 2) PickUp();
                else if (action == 3) ClearPosition();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!g.browsing || g.appearance) return 0;
        const int columns = FigureGrid{}.columns;
        Select(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -columns : columns);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        HidePanel(false);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        SetInputOwnership(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    g.root = fs::absolute(fs::path(argv[0])).parent_path().parent_path();
    bool hidden = false;
    HWND open_for_game = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--background") hidden = true;
        else if (option == L"--open-for-game" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const auto value = std::wcstoull(argv[++i], &end, 10);
            if (!*end && value) open_for_game = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
            hidden = true;
        }
        else if (option == L"--port" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const long port = std::wcstol(argv[++i], &end, 10);
            if (!*end && port >= 1 && port <= 65535) g.port = static_cast<int>(port);
        }
    }
    LocalFree(argv);
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\LegoDimensionsToypadCatalog.Instance");
    const bool already_running = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    const std::unique_ptr<void, decltype(&CloseHandle)> instance_lock(mutex, CloseHandle);
    if (HWND existing = FindWindowW(wire::kWindowClass, nullptr); existing || already_running) {
        if (hidden && !open_for_game) return 0;
        const auto deadline = GetTickCount64() + 2000;
        while (!existing && GetTickCount64() < deadline) {
            Sleep(20);
            existing = FindWindowW(wire::kWindowClass, nullptr);
        }
        if (existing && open_for_game)
            PostMessageW(existing, wire::kControllerOpen, reinterpret_cast<WPARAM>(open_for_game), 0);
        else if (existing)
            PostMessageW(existing, wire::kShowMessage, 0, 0);
        return 0;
    }
    g.cache = g.root / L"assets/toypad_catalog";
    ui::InitProcess();
    ReadAppearance();
    ReadPositionMapping();
    ReadOriginalSkin();
    g.loaded = ReadCatalog();
    PreparePortraits();
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets)) return 1;
    HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
    if (!xinput) xinput = LoadLibraryW(L"xinput9_1_0.dll");
    if (xinput) {
        g_get_pad = reinterpret_cast<GetPad>(GetProcAddress(xinput, "XInputGetState"));
        g_get_pad_key = reinterpret_cast<GetPadKey>(GetProcAddress(xinput, "XInputGetKeystroke"));
    }
    g.placement = g.desktop = ui::FitWindow(kWidth, kHeight, kWindowStyle);
    g.desktop_scale = ui::g_scale;
    if (!g.surface.Init(ui::S(kWidth), ui::S(kHeight))) { WSACleanup(); return 1; }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Window;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = wire::kWindowClass;
    RegisterClassExW(&wc);
    g.window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, wc.lpszClassName, L"LEGO Dimensions PC - Toy Pad", kWindowStyle,
                               g.placement.left, g.placement.top, g.placement.right, g.placement.bottom,
                               nullptr, nullptr, instance, nullptr);
    if (!g.window) { WSACleanup(); return 1; }
    const HWINEVENTHOOK foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                                         nullptr, ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
    std::thread(pad_hid::Read, &g_sony_pad, &g_sony_back).detach();
    RegisterHotKey(g.window, 1, MOD_CONTROL | MOD_NOREPEAT, 'L');
    SetTimer(g.window, 1, 16, nullptr);
    g.art_loading = std::async(std::launch::async, ReadArtwork, g.root);
    if (open_for_game) OpenForGame(open_for_game);
    else if (!hidden) RequestPanel(true);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    if (g.request.valid()) g.request.wait();
    if (g.extraction) CloseHandle(g.extraction);
    if (xinput) FreeLibrary(xinput);
    WSACleanup();
    return 0;
}
