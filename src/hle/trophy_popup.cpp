// 3.0: pop-ups de conquista no visual de cada console (ver trophy_popup.h).
#include "hle/trophy_popup.h"
#include "hle/trophy_manager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace hle::trophy_popup {

const char* const kStyleKeys[kStyleCount] = {"ps5", "ps4", "ps3", "xbox", "xbox360", "wiiu", "steam", "switch"};
const char* const kStyleNames[kStyleCount] = {"PS5", "PS4", "PS3", "XBOX ONE", "XBOX 360", "WII U", "STEAM", "SWITCH"};

int StyleIndex(const std::string& key) {
    for (int i = 0; i < kStyleCount; ++i) {
        if (!_stricmp(key.c_str(), kStyleKeys[i])) {
            return i;
        }
    }
    if (!_stricmp(key.c_str(), "xboxone") || !_stricmp(key.c_str(), "xboxseries")) {
        return 3;
    }
    if (!_stricmp(key.c_str(), "wii") || !_stricmp(key.c_str(), "nintendo")) {
        return 5;
    }
    return -1;
}

namespace {

using namespace Gdiplus;
enum { kPs5, kPs4, kPs3, kXboxOne, kXbox360, kWiiU, kSteam, kSwitch };

void StartGdiplus() {
    static std::once_flag once;
    std::call_once(once, [] {
        ULONG_PTR token = 0;
        GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
    });
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) { // nao e UTF-8: Windows-1252
        page = 1252;
        flags = 0;
        n = MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) {
        MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), out.data(), n);
    }
    return out;
}

std::unique_ptr<Font> MakeFont(const wchar_t* family, float px, INT style = FontStyleRegular) {
    for (const wchar_t* name : {family, L"Segoe UI", L"Arial"}) {
        FontFamily ff(name);
        if (ff.GetLastStatus() == Ok) {
            auto font = std::make_unique<Font>(&ff, px, style, UnitPixel);
            if (font->GetLastStatus() == Ok) {
                return font;
            }
        }
    }
    return std::make_unique<Font>(L"Arial", px, style, UnitPixel);
}

const StringFormat& Typographic() {
    static StringFormat* format = [] {
        auto* f = StringFormat::GenericTypographic()->Clone();
        f->SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
        f->SetTrimming(StringTrimmingEllipsisCharacter);
        return f;
    }();
    return *format;
}

float TextWidth(Graphics& g, const std::wstring& text, const Font& font) {
    RectF box;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &Typographic(), &box);
    return box.Width;
}

// texto numa linha, cortado com "..." se passar de `width`
void Text(Graphics& g, const std::wstring& text, const Font& font, const Color& color, float x, float y, float width) {
    if (width <= 1.0f || text.empty()) {
        return;
    }
    SolidBrush brush(color);
    g.DrawString(text.c_str(), -1, &font, RectF(x, y, width, font.GetHeight(&g) * 1.4f), &Typographic(), &brush);
}

void RoundRect(GraphicsPath& path, float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) * 0.5f);
    if (r < 0.75f) {
        path.AddRectangle(RectF(x, y, w, h));
        return;
    }
    const float d = 2.0f * r;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Graphics& g, const Brush& brush, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    RoundRect(path, x, y, w, h, r);
    g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, const Color& color, float width, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    RoundRect(path, x, y, w, h, r);
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

// taca de trofeu desenhada (sem imagem de ninguem) dentro de um quadrado de lado s
void Cup(Graphics& g, float x, float y, float s, const Color& color) {
    SolidBrush brush(color);
    GraphicsPath bowl;
    bowl.AddLine(x + 0.24f * s, y + 0.10f * s, x + 0.76f * s, y + 0.10f * s);
    bowl.AddBezier(x + 0.76f * s, y + 0.10f * s, x + 0.76f * s, y + 0.48f * s, x + 0.62f * s, y + 0.60f * s, x + 0.50f * s, y + 0.60f * s);
    bowl.AddBezier(x + 0.50f * s, y + 0.60f * s, x + 0.38f * s, y + 0.60f * s, x + 0.24f * s, y + 0.48f * s, x + 0.24f * s, y + 0.10f * s);
    bowl.CloseFigure();
    g.FillPath(&brush, &bowl);
    Pen pen(color, std::max(1.0f, s * 0.075f));
    g.DrawArc(&pen, x + 0.07f * s, y + 0.15f * s, 0.26f * s, 0.26f * s, 90, 180);
    g.DrawArc(&pen, x + 0.67f * s, y + 0.15f * s, 0.26f * s, 0.26f * s, 270, 180);
    g.FillRectangle(&brush, x + 0.45f * s, y + 0.58f * s, 0.10f * s, 0.20f * s);
    FillRound(g, brush, x + 0.27f * s, y + 0.77f * s, 0.46f * s, 0.13f * s, 0.03f * s);
}

Color Tier(char type, BYTE alpha = 255) {
    switch (type) {
    case 'P': return Color(alpha, 158, 205, 238);
    case 'G': return Color(alpha, 232, 186, 62);
    case 'S': return Color(alpha, 178, 188, 201);
    default: return Color(alpha, 196, 118, 70);
    }
}

int Gamerscore(char type) {
    return type == 'P' ? 180 : type == 'G' ? 90 : type == 'S' ? 30 : 15;
}

std::unique_ptr<Bitmap> LoadIcon(int trophy_id) {
    auto& manager = trophy::TrophyManager::Instance();
    const TrophyDef* def = manager.FindDef(trophy_id);
    const std::filesystem::path path =
        manager.GetRootPath() / "assets" / "trophy" / "NPWR07423_00" / "Icons" / (def && def->icon ? def->icon : "ICON0.PNG");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return nullptr;
    }
    auto bitmap = std::make_unique<Bitmap>(path.c_str());
    if (bitmap->GetLastStatus() != Ok) {
        return nullptr;
    }
    return bitmap;
}

// icone da conquista com cantos arredondados; sem o icone, a taca na cor do grau
void Icon(Graphics& g, Bitmap* icon, float x, float y, float s, float r, char type) {
    GraphicsPath clip;
    RoundRect(clip, x, y, s, s, r);
    if (icon) {
        g.SetClip(&clip);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(icon, RectF(x, y, s, s));
        g.ResetClip();
        return;
    }
    LinearGradientBrush back(PointF(x, y), PointF(x, y + s), Color(255, 54, 58, 70), Color(255, 22, 24, 30));
    g.FillPath(&back, &clip);
    Cup(g, x + s * 0.2f, y + s * 0.18f, s * 0.6f, Tier(type));
}

// "G" do gamerscore num circulo (Xbox One)
void GamerscoreBadge(Graphics& g, float x, float y, float s, const Color& color, const Font& font) {
    Pen pen(color, std::max(1.0f, s * 0.09f));
    g.DrawEllipse(&pen, x, y, s, s);
    SolidBrush brush(color);
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"G", -1, &font, RectF(x, y + s * 0.02f, s, s), &center, &brush);
}

struct Texts {
    std::wstring title, desc;
    bool pt;
    char type;
};

} // namespace

bool Render(const Request& request, Image* out) {
    StartGdiplus();
    int style = StyleIndex(request.style);
    if (style < 0) {
        style = kPs5;
    }
    const float u = std::clamp(request.scale, 0.5f, 2.5f);
    const Texts t{Wide(request.title), Wide(request.desc), request.pt, request.type};
    std::unique_ptr<Bitmap> icon = LoadIcon(request.trophy_id);

    // fontes de cada estilo
    std::unique_ptr<Font> f1, f2, f3;
    float card_w = 0, card_h = 0, orb_d = 0;
    Bitmap probe(1, 1, PixelFormat32bppARGB);
    Graphics measure(&probe);
    measure.SetTextRenderingHint(TextRenderingHintAntiAlias);
    switch (style) {
    case kPs5:
        f1 = MakeFont(L"Segoe UI Semibold", 19 * u), f2 = MakeFont(L"Segoe UI", 15 * u);
        card_w = 440 * u, card_h = 84 * u;
        break;
    case kPs4: {
        f1 = MakeFont(L"Segoe UI Light", 24 * u), f2 = MakeFont(L"Segoe UI", 16.5f * u);
        const std::wstring head = t.pt ? L"Troféu conquistado!" : L"Trophy earned!";
        const float text = std::max(TextWidth(measure, head, *f1), 22 * u + TextWidth(measure, t.title, *f2));
        card_w = std::clamp(std::ceil(60 * u + 52 * u + 18 * u + text + 26 * u + 34 * u + 26 * u), 400 * u, 980 * u);
        card_h = 76 * u;
        break;
    }
    case kPs3:
        f1 = MakeFont(L"Segoe UI", 17 * u), f2 = MakeFont(L"Segoe UI", 15.5f * u);
        card_w = 400 * u, card_h = 72 * u;
        break;
    case kXboxOne:
        f1 = MakeFont(L"Segoe UI Semibold", 20 * u), f2 = MakeFont(L"Segoe UI", 17 * u), f3 = MakeFont(L"Segoe UI Semibold", 10 * u);
        card_w = 470 * u, card_h = 72 * u, orb_d = 72 * u;
        break;
    case kXbox360:
        f1 = MakeFont(L"Segoe UI", 18 * u), f2 = MakeFont(L"Segoe UI", 16 * u);
        card_w = 450 * u, card_h = 64 * u, orb_d = 72 * u;
        break;
    case kWiiU:
        f1 = MakeFont(L"Segoe UI Semibold", 16 * u), f2 = MakeFont(L"Segoe UI Semibold", 19 * u);
        card_w = 480 * u, card_h = 84 * u;
        break;
    case kSteam:
        f1 = MakeFont(L"Arial", 15 * u, FontStyleBold), f2 = MakeFont(L"Arial", 14 * u), f3 = MakeFont(L"Arial", 12 * u);
        card_w = 340 * u, card_h = 88 * u;
        break;
    default: // Switch
        f1 = MakeFont(L"Segoe UI Semibold", 18 * u), f2 = MakeFont(L"Segoe UI", 14.5f * u);
        card_w = 420 * u, card_h = 76 * u;
        break;
    }
    const int cw = static_cast<int>(std::ceil(card_w)), ch = static_cast<int>(std::ceil(card_h));
    const int od = static_cast<int>(std::ceil(orb_d));
    const int width = cw + (od ? od + 4 : 0), height = std::max(ch, od);
    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    const float W = card_w, H = card_h;

    switch (style) {
    case kPs5: { // PS5: cartao cinza escuro no canto de cima a direita; taca do grau + nome; "Trofeu conquistado!" embaixo
        SolidBrush back(Color(238, 30, 30, 33));
        FillRound(g, back, 0.5f, 0.5f, W - 1, H - 1, 14 * u);
        StrokeRound(g, Color(26, 255, 255, 255), 1.0f, 0.5f, 0.5f, W - 1, H - 1, 14 * u);
        Icon(g, icon.get(), 14 * u, 14 * u, 56 * u, 7 * u, t.type);
        const float x = 84 * u;
        Cup(g, x, 19 * u, 17 * u, Tier(t.type));
        Text(g, t.title, *f1, Color(255, 245, 245, 245), x + 21 * u, 16 * u, W - x - 21 * u - 16 * u);
        Text(g, t.pt ? L"Troféu conquistado!" : L"Trophy earned!", *f2, Color(255, 170, 170, 176), x, 47 * u, W - x - 16 * u);
        break;
    }
    case kPs4: { // PS4: faixa preta saindo da borda esquerda, icone com moldura, "Trofeu conquistado!" e o nome com a taca
        SolidBrush back(Color(214, 0, 0, 0));
        g.FillRectangle(&back, 0.0f, 0.0f, W, H);
        const float ix = 60 * u, iy = 12 * u, is = 52 * u;
        SolidBrush frame(Color(255, 20, 20, 20));
        g.FillRectangle(&frame, ix, iy, is, is);
        Icon(g, icon.get(), ix + 3 * u, iy + 3 * u, is - 6 * u, 0, t.type);
        Pen border(Color(255, 150, 150, 150), std::max(1.0f, 1.5f * u));
        g.DrawRectangle(&border, ix, iy, is, is);
        const float x = ix + is + 18 * u, right = W - 26 * u - 34 * u - 26 * u;
        Text(g, t.pt ? L"Troféu conquistado!" : L"Trophy earned!", *f1, Color(255, 255, 255, 255), x, 9 * u, right - x);
        Cup(g, x, 44 * u, 18 * u, Tier(t.type));
        Text(g, t.title, *f2, Color(255, 225, 225, 225), x + 22 * u, 43 * u, right - x - 22 * u);
        // circulo branco do sistema a direita
        const float cd = 34 * u, cx = W - 26 * u - cd, cy = (H - cd) * 0.5f;
        SolidBrush white(Color(255, 245, 245, 245));
        g.FillEllipse(&white, cx, cy, cd, cd);
        Cup(g, cx + cd * 0.22f, cy + cd * 0.2f, cd * 0.56f, Color(255, 16, 16, 16));
        break;
    }
    case kPs3: { // PS3: caixa translucida com degrade e borda clara, canto de cima a direita
        LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(228, 74, 74, 78), Color(238, 18, 18, 20));
        FillRound(g, back, 0.75f, 0.75f, W - 1.5f, H - 1.5f, 10 * u);
        LinearGradientBrush gloss(PointF(0, 0), PointF(0, H * 0.42f), Color(30, 255, 255, 255), Color(0, 255, 255, 255));
        FillRound(g, gloss, 2 * u, 2 * u, W - 4 * u, H * 0.40f, 8 * u);
        StrokeRound(g, Color(210, 160, 160, 166), std::max(1.0f, 1.5f * u), 0.75f, 0.75f, W - 1.5f, H - 1.5f, 10 * u);
        Icon(g, icon.get(), 9 * u, 9 * u, H - 18 * u, 5 * u, t.type);
        const float x = H + 4 * u;
        Text(g, t.pt ? L"Você ganhou um troféu." : L"You have earned a trophy.", *f1, Color(255, 255, 255, 255), x, 11 * u, W - x - 14 * u);
        Cup(g, x, 40 * u, 17 * u, Tier(t.type));
        Text(g, t.title, *f2, Color(255, 228, 228, 228), x + 21 * u, 39 * u, W - x - 21 * u - 14 * u);
        break;
    }
    case kXboxOne: { // Xbox One: pilula verde que abre a partir do circulo com a taca
        SolidBrush back(Color(255, 16, 124, 16));
        FillRound(g, back, 0, 0, W, H, H * 0.5f);
        const float x = orb_d + 14 * u, right = W - H * 0.5f;
        Text(g, t.pt ? L"Conquista desbloqueada" : L"Achievement unlocked", *f1, Color(255, 255, 255, 255), x, 11 * u, right - x);
        GamerscoreBadge(g, x, 42 * u, 17 * u, Color(255, 255, 255, 255), *f3);
        Text(g, std::to_wstring(Gamerscore(t.type)) + L" - " + t.title, *f2, Color(255, 255, 255, 255), x + 22 * u, 39 * u,
             right - x - 22 * u);
        const float ox = static_cast<float>(cw + 4);
        SolidBrush orb(Color(255, 12, 94, 12));
        g.FillEllipse(&orb, ox + 1, 1.0f, orb_d - 2, orb_d - 2);
        Pen ring(Color(70, 255, 255, 255), std::max(1.0f, 2 * u));
        g.DrawEllipse(&ring, ox + 5 * u, 5 * u, orb_d - 10 * u, orb_d - 10 * u);
        Cup(g, ox + orb_d * 0.27f, orb_d * 0.25f, orb_d * 0.46f, Color(255, 255, 255, 255));
        break;
    }
    case kXbox360: { // Xbox 360: pilula cinza com brilho e a esfera verde num anel prateado
        const float top = float((height - ch) / 2); // o mesmo deslocamento de out->card
        LinearGradientBrush back(PointF(0, top), PointF(0, top + H), Color(246, 92, 92, 96), Color(246, 34, 34, 38));
        FillRound(g, back, 0.75f, top + 0.75f, W - 1.5f, H - 1.5f, H * 0.5f);
        LinearGradientBrush gloss(PointF(0, top), PointF(0, top + H * 0.34f), Color(34, 255, 255, 255), Color(0, 255, 255, 255));
        FillRound(g, gloss, H * 0.35f, top + 2 * u, W - H * 0.7f, H * 0.32f, H * 0.16f);
        StrokeRound(g, Color(220, 140, 140, 146), std::max(1.0f, 1.5f * u), 0.75f, top + 0.75f, W - 1.5f, H - 1.5f, H * 0.5f);
        const float x = orb_d + 10 * u, right = W - H * 0.5f;
        Text(g, t.pt ? L"Conquista desbloqueada" : L"Achievement unlocked", *f1, Color(255, 255, 255, 255), x, top + 10 * u, right - x);
        Text(g, std::to_wstring(Gamerscore(t.type)) + L"G - " + t.title, *f2, Color(255, 222, 222, 222), x, top + 34 * u, right - x);
        const float ox = static_cast<float>(cw + 4);
        LinearGradientBrush silver(PointF(ox, 0), PointF(ox, orb_d), Color(255, 238, 238, 242), Color(255, 118, 118, 126));
        g.FillEllipse(&silver, ox + 1, 1.0f, orb_d - 2, orb_d - 2);
        GraphicsPath sphere;
        const float inset = 7 * u, sd = orb_d - 2 * inset;
        sphere.AddEllipse(ox + inset, inset, sd, sd);
        PathGradientBrush green(&sphere);
        green.SetCenterPoint(PointF(ox + inset + sd * 0.38f, inset + sd * 0.32f));
        green.SetCenterColor(Color(255, 170, 230, 70));
        Color edge(255, 22, 92, 12);
        INT n = 1;
        green.SetSurroundColors(&edge, &n);
        g.FillPath(&green, &sphere);
        Cup(g, ox + orb_d * 0.29f, orb_d * 0.27f, orb_d * 0.42f, Color(255, 255, 255, 255));
        break;
    }
    case kWiiU: { // Wii U: balao branco arredondado com o azul do sistema, no alto e no centro
        LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(250, 255, 255, 255), Color(250, 230, 233, 238));
        FillRound(g, back, 0.75f, 0.75f, W - 1.5f, H - 1.5f, 20 * u);
        StrokeRound(g, Color(255, 186, 192, 204), std::max(1.0f, 1.5f * u), 0.75f, 0.75f, W - 1.5f, H - 1.5f, 20 * u);
        Icon(g, icon.get(), 12 * u, 12 * u, H - 24 * u, 9 * u, t.type);
        const float x = H + 2 * u;
        Text(g, t.pt ? L"Conquista desbloqueada!" : L"Achievement unlocked!", *f1, Color(255, 0, 153, 221), x, 14 * u, W - x - 20 * u);
        Text(g, t.title, *f2, Color(255, 52, 52, 58), x, 39 * u, W - x - 20 * u);
        break;
    }
    case kSteam: { // Steam: caixa azul escura no canto de baixo a direita
        LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(250, 36, 54, 75), Color(250, 21, 29, 40));
        g.FillRectangle(&back, 0.0f, 0.0f, W, H);
        Pen border(Color(255, 58, 86, 116), 1.0f);
        g.DrawRectangle(&border, 0.5f, 0.5f, W - 1, H - 1);
        Icon(g, icon.get(), 12 * u, 12 * u, 64 * u, 0, t.type);
        const float x = 88 * u;
        Text(g, t.pt ? L"Conquista desbloqueada!" : L"Achievement Unlocked!", *f1, Color(255, 255, 255, 255), x, 16 * u, W - x - 10 * u);
        Text(g, t.title, *f2, Color(255, 184, 194, 206), x, 38 * u, W - x - 10 * u);
        Text(g, t.desc, *f3, Color(255, 128, 140, 152), x, 59 * u, W - x - 10 * u);
        break;
    }
    default: { // Switch: aviso cinza escuro arredondado no canto de cima a esquerda
        SolidBrush back(Color(242, 46, 46, 50));
        FillRound(g, back, 0.5f, 0.5f, W - 1, H - 1, 12 * u);
        Icon(g, icon.get(), 12 * u, 12 * u, H - 24 * u, 6 * u, t.type);
        const float x = H + 4 * u;
        Text(g, t.title, *f1, Color(255, 255, 255, 255), x, 14 * u, W - x - 16 * u);
        Text(g, t.pt ? L"Conquista desbloqueada" : L"Achievement unlocked", *f2, Color(255, 0, 195, 227), x, 42 * u, W - x - 16 * u);
        break;
    }
    }

    Rect rect(0, 0, width, height);
    BitmapData data{};
    if (bitmap.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
        return false;
    }
    out->w = width;
    out->h = height;
    out->rgba.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride);
        for (int x = 0; x < width; ++x) {
            const std::uint32_t argb = row[x];
            out->rgba[static_cast<size_t>(y) * width + x] =
                ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000u);
        }
    }
    bitmap.UnlockBits(&data);
    out->card = {0, (height - ch) / 2, cw, ch};
    out->orb = od ? Part{cw + 4, (height - od) / 2, od, od} : Part{};
    return true;
}

namespace {

float Clamp01(float t) {
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float EaseOut(float t) {
    t = Clamp01(t);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float EaseBack(float t) { // passa um pouco do tamanho e volta
    t = Clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return 1.0f + c3 * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) + c1 * (t - 1.0f) * (t - 1.0f);
}

void Push(std::vector<Quad>* quads, float x, float y, const Part& p, float alpha) {
    if (alpha > 0.003f) {
        quads->push_back({x, y, x + p.w, y + p.h, float(p.x), float(p.y), float(p.x + p.w), float(p.y + p.h), std::min(alpha, 1.0f)});
    }
}

} // namespace

void Layout(const std::string& style_key, const Image& image, float cw, float ch, float scale, float in, float out,
            std::vector<Quad>* quads) {
    const float u = std::clamp(scale, 0.5f, 2.5f);
    const Part& c = image.card;
    const float W = float(c.w), H = float(c.h);
    int style = StyleIndex(style_key);
    switch (style < 0 ? kPs5 : style) {
    case kPs5: { // entra deslizando da direita com fade
        const float a = std::min(EaseOut(in / 0.30f), EaseOut(out / 0.30f));
        Push(quads, cw - W - 40 * u + (1.0f - a) * 40 * u, 40 * u, c, a);
        break;
    }
    case kPs4: { // a faixa sai da borda esquerda
        const float t = std::min(EaseOut(in / 0.35f), EaseOut(out / 0.35f));
        Push(quads, -W * (1.0f - t), ch * 0.085f, c, 1.0f);
        break;
    }
    case kPs3: // aparece e some com fade
        Push(quads, cw - W - 48 * u, 44 * u, c, std::min(Clamp01(in / 0.25f), Clamp01(out / 0.45f)));
        break;
    case kXboxOne:
    case kXbox360: { // o circulo surge, a pilula abre para a direita; no fim fecha e o circulo some
        const Part& o = image.orb;
        const float x = std::round((cw - W) * 0.5f), y = std::round(ch - H - ch * 0.09f);
        const float pill = std::min(EaseOut((in - 0.18f) / 0.40f), EaseOut((out - 0.22f) / 0.32f));
        const float orb = std::min(EaseBack(in / 0.24f), Clamp01(out / 0.22f));
        if (pill > 0.0f) {
            const float cap = H * 0.5f, open = H + (W - H) * pill;
            const float a = std::min(1.0f, pill * 4.0f);
            quads->push_back({x, y, x + open - cap, y + H, float(c.x), float(c.y), float(c.x) + open - cap, float(c.y) + H, a});
            quads->push_back({x + open - cap, y, x + open, y + H, float(c.x) + W - cap, float(c.y), float(c.x) + W, float(c.y) + H, a});
        }
        if (orb > 0.0f && o.w > 0) {
            const float size = o.w * orb, cx = x + H * 0.5f, cy = y + H * 0.5f;
            quads->push_back({cx - size * 0.5f, cy - size * 0.5f, cx + size * 0.5f, cy + size * 0.5f, float(o.x), float(o.y),
                              float(o.x + o.w), float(o.y + o.h), std::min(1.0f, orb * 1.5f)});
        }
        break;
    }
    case kWiiU: { // desce do alto da tela
        const float t = std::min(EaseOut(in / 0.35f), EaseOut(out / 0.30f));
        Push(quads, std::round((cw - W) * 0.5f), 30 * u - (1.0f - t) * (H + 40 * u), c, 1.0f);
        break;
    }
    case kSteam: { // sobe da borda de baixo
        const float t = std::min(EaseOut(in / 0.40f), EaseOut(out / 0.40f));
        Push(quads, cw - W - 14 * u, ch - H - 14 * u + (1.0f - t) * (H + 20 * u), c, 1.0f);
        break;
    }
    default: { // Switch: desce um pouco com fade
        const float a = std::min(EaseOut(in / 0.25f), EaseOut(out / 0.25f));
        Push(quads, 36 * u, 36 * u - (1.0f - a) * 14 * u, c, a);
        break;
    }
    }
}

} // namespace hle::trophy_popup
