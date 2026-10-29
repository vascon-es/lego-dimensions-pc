// Fase 50: visual comum do launcher e do instalador (GDI+, tema escuro, botoes arredondados, chaves
// liga/desliga, barra de progresso com degrade, barra de titulo escura do Windows 11, DPI).
#pragma once
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <map>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

namespace ui {

// ---- cores ----
constexpr COLORREF kBg = RGB(17, 19, 28), kPanel = RGB(27, 30, 43), kField = RGB(36, 40, 56), kBorder = RGB(56, 62, 84),
                   kText = RGB(236, 238, 246), kMuted = RGB(150, 158, 182), kAccent = RGB(255, 200, 20), kAccentDark = RGB(232, 168, 0),
                   kBlue = RGB(70, 170, 255), kGreen = RGB(80, 210, 130), kRed = RGB(255, 100, 100), kAmber = RGB(255, 180, 60);

inline Gdiplus::Color C(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// ---- idioma (Fase 60) ----
// Portugues se o Windows estiver em portugues; senao ingles. LD_LANG=pt|en escolhe na mao.
inline bool Pt() {
    static const bool pt = [] {
        wchar_t value[8] = {};
        if (GetEnvironmentVariableW(L"LD_LANG", value, 8)) {
            return _wcsnicmp(value, L"pt", 2) == 0;
        }
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
    }();
    return pt;
}
inline const wchar_t* T(const wchar_t* pt, const wchar_t* en) { return Pt() ? pt : en; }

// Keep script protocol tokens compatible; translate only their displayed labels.
inline std::wstring LogText(const std::wstring& text) {
    if (Pt()) return text;
    for (const auto& pair : {std::pair{L"ERRO ",L"ERROR "},std::pair{L"AVISO ",L"WARNING "},
         std::pair{L"INSTALADO ",L"INSTALLED "},std::pair{L"GRAVADO ",L"WRITTEN "},
         std::pair{L"ATUALIZADO ",L"UPDATED "},std::pair{L"PRONTO ",L"READY "}}) {
        const size_t n=wcslen(pair.first);
        if(text.compare(0,n,pair.first)==0)return std::wstring(pair.second)+text.substr(n);
    }
    return text;
}

// ---- DPI ----
inline float g_scale = 1.0f;
inline int S(float v) { return static_cast<int>(v * g_scale + 0.5f); }

// Fase 61: lugar da janela. Numa tela pequena (ou com escala alta, ex. 1080p a 125%) a escala desce ate a
// janela inteira caber na area de trabalho (sem a barra de tarefas); a janela abre centralizada nela.
// Chamar antes de criar as fontes e os controles. Devolve {x, y, largura, altura}.
inline RECT FitWindow(int w, int h, DWORD style) {
    RECT work{0, 0, 1920, 1040};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    wchar_t test[32] = {}; // LD_UI_WORKAREA=LxA: so para testes (simula uma tela menor)
    if (int tw = 0, th = 0; GetEnvironmentVariableW(L"LD_UI_WORKAREA", test, 32) && swscanf_s(test, L"%dx%d", &tw, &th) == 2) {
        work = RECT{0, 0, tw, th};
    }
    RECT frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, style, FALSE);
    const float room_w = static_cast<float>((work.right - work.left) - (frame.right - frame.left));
    const float room_h = static_cast<float>((work.bottom - work.top) - (frame.bottom - frame.top));
    const float fit = std::min(room_w / w, room_h / h);
    if (fit < g_scale) {
        g_scale = std::max(0.5f, fit);
    }
    RECT r{0, 0, S(static_cast<float>(w)), S(static_cast<float>(h))};
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    return RECT{work.left + std::max(0, (static_cast<int>(work.right - work.left) - ww) / 2),
                work.top + std::max(0, (static_cast<int>(work.bottom - work.top) - wh) / 2), ww, wh};
}

inline void InitProcess() {
    using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    using GetDpi = UINT(WINAPI*)();
    if (auto get = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))) {
        g_scale = get() / 96.0f;
    }
    static ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
}

// Barra de titulo escura (Windows 10 20H1+) e da cor do fundo (Windows 11).
inline void DarkTitle(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = kBg, text = kText;
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

inline HFONT Font(float size, int weight = FW_NORMAL, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-S(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

// ---- desenho ----
inline void RoundRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

inline void FillRound(Gdiplus::Graphics& g, const RECT& r, float radius, const Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left - 1),
                  static_cast<float>(r.bottom - r.top - 1), radius);
    g.FillPath(&brush, &path);
}

inline void StrokeRound(Gdiplus::Graphics& g, const RECT& r, float radius, COLORREF color, float width = 1.0f, BYTE alpha = 255) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, r.left + 0.5f, r.top + 0.5f, static_cast<float>(r.right - r.left - 2), static_cast<float>(r.bottom - r.top - 2), radius);
    Gdiplus::Pen pen(C(color, alpha), width);
    g.DrawPath(&pen, &path);
}

inline void Text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

// Desenho com buffer: a janela inteira e pintada numa imagem e copiada de uma vez (sem piscar).
struct Buffered {
    HDC target, dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT rect;
    Buffered(HDC t, const RECT& r) : target(t), rect(r) {
        dc = CreateCompatibleDC(t);
        bitmap = CreateCompatibleBitmap(t, r.right - r.left, r.bottom - r.top);
        old = SelectObject(dc, bitmap);
    }
    ~Buffered() {
        BitBlt(target, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
};

// ---- botoes (BS_OWNERDRAW) com hover ----
enum class Style { kPrimary, kSecondary, kToggle };
struct ButtonInfo {
    Style style = Style::kSecondary;
    bool hover = false, checked = false;
    HFONT font = nullptr;
    COLORREF surface = kBg; // cor de quem esta atras do botao (fundo da janela ou do cartao)
};
inline std::map<HWND, ButtonInfo> g_buttons;

inline LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    auto it = g_buttons.find(h);
    if (it != g_buttons.end()) {
        if (msg == WM_MOUSEMOVE && !it->second.hover) {
            it->second.hover = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        } else if (msg == WM_MOUSELEAVE) {
            it->second.hover = false;
            InvalidateRect(h, nullptr, FALSE);
        } else if (msg == WM_LBUTTONDBLCLK) {
            msg = WM_LBUTTONDOWN; // duplo clique conta como dois cliques
        }
    }
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

inline HWND Button(HWND parent, const wchar_t* text, Style style, int x, int y, int w, int h, int id, HFONT font, COLORREF surface = kBg) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    g_buttons[b] = ButtonInfo{style, false, false, font, surface};
    SetWindowSubclass(b, ButtonSubclass, 0, 0);
    return b;
}

inline bool Checked(HWND b) { return g_buttons[b].checked; }
inline void SetChecked(HWND b, bool on) {
    g_buttons[b].checked = on;
    InvalidateRect(b, nullptr, FALSE);
}

// WM_DRAWITEM dos botoes criados por Button().
inline void DrawButton(const DRAWITEMSTRUCT* item) {
    const auto it = g_buttons.find(item->hwndItem);
    if (it == g_buttons.end()) {
        return;
    }
    const ButtonInfo& info = it->second;
    const RECT r = item->rcItem;
    Buffered buffer(item->hDC, r);
    HDC dc = buffer.dc;
    RECT local{0, 0, r.right - r.left, r.bottom - r.top};
    HBRUSH bg = CreateSolidBrush(info.surface);
    FillRect(dc, &local, bg);
    DeleteObject(bg);
    const bool enabled = IsWindowEnabled(item->hwndItem);
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    wchar_t text[128];
    GetWindowTextW(item->hwndItem, text, 128);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (info.style == Style::kToggle) {
            // chave: trilho arredondado + bolinha; texto a direita
            const int th = S(22), tw = S(40), ty = (local.bottom - th) / 2;
            RECT track{0, ty, tw, ty + th};
            Gdiplus::SolidBrush on(C(info.checked ? kAccent : kField, enabled ? 255 : 120));
            FillRound(g, track, th / 2.0f, on);
            if (!info.checked) {
                StrokeRound(g, track, th / 2.0f, kBorder);
            }
            const int k = th - S(6), kx = info.checked ? tw - k - S(3) : S(3);
            Gdiplus::SolidBrush knob(C(info.checked ? RGB(30, 24, 0) : (info.hover ? kText : kMuted)));
            g.FillEllipse(&knob, kx, ty + S(3), k, k);
        } else {
            COLORREF fill = info.style == Style::kPrimary ? (pressed ? kAccentDark : info.hover ? RGB(255, 214, 70) : kAccent)
                                                          : (pressed ? kBorder : info.hover ? RGB(48, 54, 74) : kField);
            Gdiplus::SolidBrush brush(C(fill, enabled ? 255 : 110));
            FillRound(g, local, S(9), brush);
            if (info.style == Style::kSecondary) {
                StrokeRound(g, local, S(9), info.hover ? kMuted : kBorder);
            }
        }
    }
    if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) { // foco pelo teclado / controle
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        RECT ring{1, 1, local.right - 1, local.bottom - 1};
        StrokeRound(g, ring, S(9), kAccent, 2.0f);
    }
    if (info.style == Style::kToggle) {
        RECT tr{S(52), 0, local.right, local.bottom};
        Text(dc, text, tr, info.font, enabled ? kText : kMuted);
    } else {
        const COLORREF color = info.style == Style::kPrimary ? (enabled ? RGB(28, 22, 0) : RGB(90, 80, 40)) : (enabled ? kText : kMuted);
        Text(dc, text, local, info.font, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// Clique numa chave: inverte (chamar no WM_COMMAND do id da chave).
inline void Toggle(HWND b) { SetChecked(b, !Checked(b)); }

// Campo de texto sem borda propria: o pai desenha a caixa arredondada em volta (FieldBox).
inline HWND Edit(HWND parent, int x, int y, int w, int h, int id, HFONT font, DWORD extra = ES_AUTOHSCROLL) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
    return e;
}

// Caixa arredondada em volta de um controle filho (desenhada no WM_PAINT do pai).
inline void FieldBox(Gdiplus::Graphics& g, HWND parent, HWND child, bool focus) {
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, S(10), S(8));
    Gdiplus::SolidBrush brush(C(kField));
    FillRound(g, r, S(9), brush);
    StrokeRound(g, r, S(9), focus ? kAccent : kBorder, focus ? 1.5f : 1.0f);
}

// Barra de progresso com degrade azul -> amarelo.
inline void Progress(Gdiplus::Graphics& g, const RECT& r, int pct) {
    Gdiplus::SolidBrush track(C(kField));
    FillRound(g, r, (r.bottom - r.top) / 2.0f, track);
    if (pct <= 0) {
        return;
    }
    RECT fill = r;
    fill.right = r.left + std::max<int>(r.bottom - r.top, (r.right - r.left) * std::min(pct, 100) / 100);
    Gdiplus::LinearGradientBrush grad(Gdiplus::Point(r.left, 0), Gdiplus::Point(r.right, 0), C(kBlue), C(kAccent));
    FillRound(g, fill, (r.bottom - r.top) / 2.0f, grad);
}

// "Etiqueta" arredondada (status): ponto colorido + texto.
inline int Pill(HDC dc, Gdiplus::Graphics& g, int x, int y, const std::wstring& text, COLORREF dot, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size;
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    const int h = S(30), w = size.cx + S(40);
    RECT r{x, y, x + w, y + h};
    Gdiplus::SolidBrush brush(C(kField));
    FillRound(g, r, h / 2.0f, brush);
    Gdiplus::SolidBrush dot_brush(C(dot));
    g.FillEllipse(&dot_brush, x + S(13), y + h / 2 - S(5), S(10), S(10));
    g.Flush();
    RECT tr{x + S(30), y, x + w, y + h};
    Text(dc, text, tr, font, kText);
    return w;
}

} // namespace ui
