// 3.0: pop-ups de conquista no visual de cada console (ver trophy_popup.h).
// Medidas e tempos tirados quadro a quadro de gravacoes: PS5 (todos os graus), Xbox 360 e Xbox One (2013) e a conquista rara do
// Xbox One. As medidas estao em pixels de 1080p (u = 1); as do Xbox vieram de 720p (x 1,5).
#include "hle/trophy_popup.h"
#include "hle/trophy_manager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
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

// pecas do Xbox 360
enum { k3Pill, k3Text, k3Ring, k3Arc0, k3Sphere = k3Arc0 + 6, k3Cup, k3Disc };
// pecas do Xbox One
enum { k1Disc, k1PillDisc, k1Logo, k1Icon, k1TextA, k1TextB, k1TextHold, k1TextDetails };

// tempos do Xbox One (s), normal e rara
struct OneTimes {
    float open0, open1, a_out, b_in, b_out, c_in, close, end;
};
constexpr OneTimes kOneNormal{1.27f, 1.75f, 2.73f, 2.83f, 5.33f, 5.80f, 7.93f, 8.85f};
constexpr OneTimes kOneRare{1.27f, 1.75f, 4.30f, 4.40f, 7.00f, 7.30f, 9.25f, 10.15f};

} // namespace

float Duration(const std::string& style, char type) {
    switch (StyleIndex(style)) {
    case kPs5: return 3.80f;
    case kPs4:
    case kPs3: return 5.0f;
    case kXboxOne: return type == 'P' ? kOneRare.end : kOneNormal.end;
    case kXbox360: return 5.6f;
    default: return 5.5f;
    }
}

namespace {

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
    if (text.empty()) {
        return 0.0f;
    }
    RectF box;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &Typographic(), &box);
    return box.Width;
}

// texto numa linha a partir do canto de cima (y = topo da linha), cortado com "..." se passar de `width`
void Text(Graphics& g, const std::wstring& text, const Font& font, const Color& color, float x, float y, float width) {
    if (width <= 1.0f || text.empty()) {
        return;
    }
    SolidBrush brush(color);
    g.DrawString(text.c_str(), -1, &font, RectF(x, y, width, font.GetHeight(&g) * 1.4f), &Typographic(), &brush);
}

// o mesmo, com a linha centrada em `cy`
void TextMid(Graphics& g, const std::wstring& text, const Font& font, const Color& color, float x, float cy, float width) {
    Text(g, text, font, color, x, cy - font.GetHeight(&g) * 0.56f, width);
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

void Disc(Graphics& g, float cx, float cy, float d, const Color& color) {
    SolidBrush brush(color);
    g.FillEllipse(&brush, cx - d * 0.5f, cy - d * 0.5f, d, d);
}

// taca simples (Xbox: branca)
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

struct Metal {
    Color light, mid, dark;
};

Metal TierMetal(char type) {
    switch (type) {
    case 'P': return {Color(255, 248, 250, 255), Color(255, 196, 204, 216), Color(255, 112, 120, 134)};
    case 'G': return {Color(255, 255, 232, 150), Color(255, 226, 172, 52), Color(255, 140, 94, 18)};
    case 'S': return {Color(255, 248, 248, 252), Color(255, 186, 190, 200), Color(255, 104, 108, 120)};
    default: return {Color(255, 244, 176, 136), Color(255, 198, 120, 80), Color(255, 118, 62, 36)};
    }
}

// pincel de metal na horizontal: escuro nas bordas, claro no meio
std::unique_ptr<LinearGradientBrush> MetalBrush(float x, float w, const Metal& m) {
    auto brush = std::make_unique<LinearGradientBrush>(PointF(x - 1, 0), PointF(x + w + 1, 0), m.dark, m.dark);
    Color colors[] = {m.dark, m.mid, m.light, m.mid, m.dark};
    REAL positions[] = {0.0f, 0.22f, 0.45f, 0.75f, 1.0f};
    brush->SetInterpolationColors(colors, positions, 5);
    return brush;
}

// os quatro simbolos do controle gravados (triangulo, circulo, x, quadrado) num quadrado de lado s
void Symbols(Graphics& g, float x, float y, float s, const Color& color) {
    Pen pen(color, std::max(0.8f, s * 0.07f));
    const float c = s * 0.36f, gap = s * 0.14f;
    const float x1 = x, x2 = x + c + gap, y1 = y, y2 = y + c + gap;
    PointF tri[] = {PointF(x1 + c * 0.5f, y1), PointF(x1 + c, y1 + c), PointF(x1, y1 + c)};
    g.DrawPolygon(&pen, tri, 3);
    g.DrawEllipse(&pen, x2, y1, c, c);
    g.DrawLine(&pen, x1, y2, x1 + c, y2 + c);
    g.DrawLine(&pen, x1 + c, y2, x1, y2 + c);
    g.DrawRectangle(&pen, x2, y2, c, c);
}

// taca do grau no desenho do PS5 / PS4 / PS3: ouro, prata e bronze com alcas; platina = esfera azul num suporte prateado.
// (x, y, w, h): a caixa; `symbols` grava os simbolos (no tamanho grande)
void TierTrophy(Graphics& g, float x, float y, float w, float h, char type, bool symbols) {
    const Metal m = TierMetal(type);
    if (type == 'P') {
        const Metal silver = TierMetal('S');
        auto metal = MetalBrush(x, w, silver);
        // suporte: duas garras curvas abracando a esfera, haste e base
        GraphicsPath stand;
        stand.AddBezier(x + 0.06f * w, y + 0.26f * h, x + 0.02f * w, y + 0.58f * h, x + 0.32f * w, y + 0.72f * h, x + 0.42f * w, y + 0.74f * h);
        stand.AddLine(x + 0.42f * w, y + 0.74f * h, x + 0.42f * w, y + 0.86f * h);
        stand.AddLine(x + 0.24f * w, y + 0.86f * h, x + 0.76f * w, y + 0.86f * h);
        stand.AddLine(x + 0.58f * w, y + 0.86f * h, x + 0.58f * w, y + 0.74f * h);
        stand.AddBezier(x + 0.58f * w, y + 0.74f * h, x + 0.68f * w, y + 0.72f * h, x + 0.98f * w, y + 0.58f * h, x + 0.94f * w, y + 0.26f * h);
        stand.AddBezier(x + 0.94f * w, y + 0.26f * h, x + 0.82f * w, y + 0.56f * h, x + 0.18f * w, y + 0.56f * h, x + 0.06f * w, y + 0.26f * h);
        stand.CloseFigure();
        g.FillPath(metal.get(), &stand);
        FillRound(g, *metal, x + 0.20f * w, y + 0.86f * h, 0.60f * w, 0.10f * h, 0.03f * w);
        // esfera
        const float d = 0.80f * w, cx = x + 0.5f * w, cy = y + 0.30f * h;
        GraphicsPath sphere;
        sphere.AddEllipse(cx - d * 0.5f, cy - d * 0.5f, d, d);
        PathGradientBrush blue(&sphere);
        blue.SetCenterPoint(PointF(cx - d * 0.12f, cy - d * 0.16f));
        Color colors[] = {Color(255, 22, 76, 176), Color(255, 64, 150, 232), Color(255, 206, 242, 255)};
        REAL positions[] = {0.0f, 0.5f, 1.0f};
        blue.SetInterpolationColors(colors, positions, 3);
        g.FillPath(&blue, &sphere);
        Disc(g, cx - d * 0.14f, cy - d * 0.2f, d * 0.26f, Color(120, 255, 255, 255));
        if (symbols) {
            Symbols(g, cx - d * 0.22f, cy - d * 0.26f, d * 0.44f, Color(210, 245, 252, 255));
        }
        return;
    }
    auto metal = MetalBrush(x, w, m);
    GraphicsPath bowl;
    bowl.AddLine(x + 0.16f * w, y + 0.04f * h, x + 0.84f * w, y + 0.04f * h);
    bowl.AddBezier(x + 0.84f * w, y + 0.04f * h, x + 0.84f * w, y + 0.40f * h, x + 0.66f * w, y + 0.56f * h, x + 0.50f * w, y + 0.56f * h);
    bowl.AddBezier(x + 0.50f * w, y + 0.56f * h, x + 0.34f * w, y + 0.56f * h, x + 0.16f * w, y + 0.40f * h, x + 0.16f * w, y + 0.04f * h);
    bowl.CloseFigure();
    // alcas
    Pen handle(metal.get(), std::max(1.0f, 0.085f * w));
    g.DrawArc(&handle, x + 0.03f * w, y + 0.08f * h, 0.30f * w, 0.26f * h, 90, 180);
    g.DrawArc(&handle, x + 0.67f * w, y + 0.08f * h, 0.30f * w, 0.26f * h, 270, 180);
    g.FillPath(metal.get(), &bowl);
    // haste e base
    g.FillRectangle(metal.get(), x + 0.44f * w, y + 0.54f * h, 0.12f * w, 0.22f * h);
    FillRound(g, *metal, x + 0.30f * w, y + 0.74f * h, 0.40f * w, 0.08f * h, 0.02f * w);
    FillRound(g, *metal, x + 0.20f * w, y + 0.82f * h, 0.60f * w, 0.12f * h, 0.03f * w);
    // brilho em cima da taca
    SolidBrush shine(Color(70, 255, 255, 255));
    g.FillRectangle(&shine, x + 0.18f * w, y + 0.04f * h, 0.64f * w, 0.03f * h);
    if (symbols) {
        Symbols(g, x + 0.33f * w, y + 0.11f * h, 0.34f * w, Color(120, m.dark.GetR() / 2, m.dark.GetG() / 2, m.dark.GetB() / 2));
    }
}

// logo do Xbox One: esfera branca com o X recortado na cor `cut` (a cor do que esta atras)
void XboxOneLogo(Graphics& g, float cx, float cy, float d, const Color& cut) {
    Disc(g, cx, cy, d, Color(255, 255, 255, 255));
    GraphicsPath clip;
    clip.AddEllipse(cx - d * 0.5f, cy - d * 0.5f, d, d);
    g.SetClip(&clip);
    Pen pen(cut, d * 0.095f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const float r = d * 0.5f;
    g.DrawBezier(&pen, cx - r * 0.95f, cy - r * 0.55f, cx - r * 0.2f, cy - r * 0.05f, cx + r * 0.25f, cy + r * 0.35f, cx + r * 0.70f, cy + r * 1.05f);
    g.DrawBezier(&pen, cx + r * 0.95f, cy - r * 0.55f, cx + r * 0.2f, cy - r * 0.05f, cx - r * 0.25f, cy + r * 0.35f, cx - r * 0.70f, cy + r * 1.05f);
    g.ResetClip();
}

// esfera do Xbox 360: prateada com o X verde
void Xbox360Sphere(Graphics& g, float cx, float cy, float d) {
    GraphicsPath sphere;
    sphere.AddEllipse(cx - d * 0.5f, cy - d * 0.5f, d, d);
    PathGradientBrush silver(&sphere);
    silver.SetCenterPoint(PointF(cx - d * 0.14f, cy - d * 0.20f));
    Color colors[] = {Color(255, 96, 98, 104), Color(255, 170, 172, 178), Color(255, 248, 248, 250)};
    REAL positions[] = {0.0f, 0.55f, 1.0f};
    silver.SetInterpolationColors(colors, positions, 3);
    g.FillPath(&silver, &sphere);
    g.SetClip(&sphere);
    const float r = d * 0.5f;
    LinearGradientBrush green(PointF(cx - r, cy - r), PointF(cx + r, cy + r), Color(255, 160, 226, 70), Color(255, 30, 120, 12));
    Pen pen(&green, d * 0.12f);
    g.DrawBezier(&pen, cx - r * 0.85f, cy - r * 0.95f, cx - r * 0.1f, cy - r * 0.25f, cx + r * 0.1f, cy + r * 0.25f, cx + r * 0.85f, cy + r * 0.95f);
    g.DrawBezier(&pen, cx + r * 0.85f, cy - r * 0.95f, cx + r * 0.1f, cy - r * 0.25f, cx - r * 0.1f, cy + r * 0.25f, cx - r * 0.85f, cy + r * 0.95f);
    g.ResetClip();
    Disc(g, cx - d * 0.14f, cy - d * 0.24f, d * 0.30f, Color(70, 255, 255, 255));
}

// diamante da conquista rara
void Diamond(Graphics& g, float cx, float cy, float s) {
    const float w = s, h = s * 0.82f, x = cx - w * 0.5f, y = cy - h * 0.5f;
    PointF outline[] = {PointF(x + 0.22f * w, y), PointF(x + 0.78f * w, y), PointF(x + w, y + 0.32f * h), PointF(x + 0.5f * w, y + h),
                        PointF(x, y + 0.32f * h)};
    LinearGradientBrush body(PointF(x, y), PointF(x + w, y + h), Color(255, 250, 253, 255), Color(255, 150, 196, 236));
    g.FillPolygon(&body, outline, 5);
    Pen facet(Color(200, 70, 140, 210), std::max(0.8f, s * 0.025f));
    g.DrawPolygon(&facet, outline, 5);
    g.DrawLine(&facet, x, y + 0.32f * h, x + w, y + 0.32f * h);
    g.DrawLine(&facet, x + 0.22f * w, y, x + 0.36f * w, y + 0.32f * h);
    g.DrawLine(&facet, x + 0.78f * w, y, x + 0.64f * w, y + 0.32f * h);
    g.DrawLine(&facet, x + 0.5f * w, y, x + 0.36f * w, y + 0.32f * h);
    g.DrawLine(&facet, x + 0.5f * w, y, x + 0.64f * w, y + 0.32f * h);
    g.DrawLine(&facet, x + 0.36f * w, y + 0.32f * h, x + 0.5f * w, y + h);
    g.DrawLine(&facet, x + 0.64f * w, y + 0.32f * h, x + 0.5f * w, y + h);
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

// icone da conquista (arte do jogo) com cantos arredondados; sem ele, a taca do grau
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
    TierTrophy(g, x + s * 0.22f, y + s * 0.12f, s * 0.56f, s * 0.76f, type, false);
}

// junta as pecas desenhadas numa imagem so (prateleiras de ate 2048 px)
class Builder {
public:
    using Draw = std::function<void(Graphics&, float, float)>;

    int Add(float wf, float hf, const Draw& draw) {
        const int w = std::max(1, static_cast<int>(std::ceil(wf))), h = std::max(1, static_cast<int>(std::ceil(hf)));
        auto bitmap = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
        {
            Graphics g(bitmap.get());
            g.Clear(Color(0, 0, 0, 0));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            g.SetCompositingQuality(CompositingQualityHighQuality);
            draw(g, float(w), float(h));
        }
        sprites_.push_back(std::move(bitmap));
        return static_cast<int>(sprites_.size()) - 1;
    }

    bool Finish(Image* out) {
        constexpr int kMaxW = 2048, kPad = 3;
        out->parts.clear();
        int x = 0, y = 0, row_h = 0, used_w = 0;
        for (const auto& s : sprites_) {
            const int w = int(s->GetWidth()), h = int(s->GetHeight());
            if (x > 0 && x + w > kMaxW) {
                x = 0, y += row_h + kPad, row_h = 0;
            }
            out->parts.push_back({x, y, w, h});
            x += w + kPad;
            row_h = std::max(row_h, h);
            used_w = std::max(used_w, x);
        }
        out->w = std::min(kMaxW, used_w);
        out->h = y + row_h;
        out->rgba.assign(size_t(out->w) * out->h, 0);
        for (size_t i = 0; i < sprites_.size(); ++i) {
            Bitmap& bitmap = *sprites_[i];
            const Part& p = out->parts[i];
            Rect rect(0, 0, p.w, p.h);
            BitmapData data{};
            if (bitmap.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
                return false;
            }
            for (int row = 0; row < p.h; ++row) {
                const auto* src = reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(data.Scan0) + ptrdiff_t(row) * data.Stride);
                std::uint32_t* dst = &out->rgba[size_t(p.y + row) * out->w + p.x];
                for (int col = 0; col < p.w; ++col) {
                    const std::uint32_t argb = src[col];
                    dst[col] = ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000u);
                }
            }
            bitmap.UnlockBits(&data);
        }
        return true;
    }

private:
    std::vector<std::unique_ptr<Bitmap>> sprites_;
};

} // namespace

bool Render(const Request& request, Image* out) {
    StartGdiplus();
    int style = StyleIndex(request.style);
    if (style < 0) {
        style = kPs5;
    }
    const float u = std::clamp(request.scale, 0.5f, 2.5f);
    const std::wstring title = Wide(request.title), desc = Wide(request.desc);
    const bool pt = request.pt;
    const char type = request.type;
    std::unique_ptr<Bitmap> icon = LoadIcon(request.trophy_id);
    Bitmap probe(1, 1, PixelFormat32bppARGB);
    Graphics measure(&probe);
    measure.SetTextRenderingHint(TextRenderingHintAntiAlias);
    Builder b;
    out->style = style, out->type = type, out->u = u;

    switch (style) {
    case kPs5: { // cartao 655x113 a 80 do canto direito e 60 do topo; taca do grau, nome e "Trophy earned!"
        auto f1 = MakeFont(L"Segoe UI", 31 * u), f2 = MakeFont(L"Segoe UI", 25 * u);
        const float W = 655 * u, H = 113 * u;
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            LinearGradientBrush back(PointF(-1, 0), PointF(W + 1, 0), Color(250, 64, 56, 62), Color(250, 34, 31, 35));
            Color stops[] = {Color(250, 64, 56, 62), Color(250, 34, 31, 35), Color(250, 34, 31, 35)};
            REAL at[] = {0.0f, 0.6f, 1.0f};
            back.SetInterpolationColors(stops, at, 3); // (sem isto o GDI+ repete o degrade depois do ponto final)
            FillRound(g, back, 0, 0, W, H, 16 * u);
            TierTrophy(g, 37 * u, (H - 70 * u) * 0.5f, 46 * u, 70 * u, type, true);
            TextMid(g, title, *f1, Color(255, 246, 244, 246), 107 * u, 40 * u, W - 107 * u - 24 * u);
            TextMid(g, pt ? L"Troféu conquistado!" : L"Trophy earned!", *f2, Color(255, 150, 144, 152), 108 * u, 81 * u, W - 108 * u - 24 * u);
        });
        break;
    }
    case kPs4: { // faixa escura com borda fina: icone, taca + nome, "You have earned a trophy!"
        auto f1 = MakeFont(L"Segoe UI", 25 * u), f2 = MakeFont(L"Segoe UI", 21 * u);
        const std::wstring sub = pt ? L"Você ganhou um troféu!" : L"You have earned a trophy!";
        const float H = 76 * u, text_x = 86 * u;
        const float W = std::clamp(std::ceil(text_x + std::max(26 * u + TextWidth(measure, title, *f1), TextWidth(measure, sub, *f2)) + 30 * u),
                                   430 * u, 900 * u);
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            SolidBrush back(Color(214, 22, 22, 24));
            FillRound(g, back, 0.5f, 0.5f, W - 1, H - 1, 3 * u);
            StrokeRound(g, Color(230, 104, 104, 110), std::max(1.0f, 1.5f * u), 0.75f, 0.75f, W - 1.5f, H - 1.5f, 3 * u);
            Icon(g, icon.get(), 12 * u, 12 * u, H - 24 * u, 0, type);
            Pen frame(Color(255, 120, 120, 124), std::max(1.0f, u));
            g.DrawRectangle(&frame, 12 * u, 12 * u, H - 24 * u, H - 24 * u);
            TierTrophy(g, text_x, 14 * u, 18 * u, 24 * u, type, false);
            Text(g, title, *f1, Color(255, 245, 245, 245), text_x + 25 * u, 10 * u, W - text_x - 25 * u - 16 * u);
            Text(g, sub, *f2, Color(255, 172, 172, 176), text_x, 42 * u, W - text_x - 16 * u);
        });
        break;
    }
    case kPs3: { // caixa escura arredondada com borda clara: "You have earned a trophy." e a taca + nome
        auto f1 = MakeFont(L"Segoe UI", 25 * u), f2 = MakeFont(L"Segoe UI", 24 * u);
        const std::wstring head = pt ? L"Você ganhou um troféu." : L"You have earned a trophy.";
        const float H = 86 * u, text_x = H + 6 * u;
        const float W = std::clamp(std::ceil(text_x + std::max(TextWidth(measure, head, *f1), 26 * u + TextWidth(measure, title, *f2)) + 26 * u),
                                   440 * u, 900 * u);
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(232, 52, 52, 54), Color(236, 26, 26, 28));
            FillRound(g, back, 1, 1, W - 2, H - 2, 9 * u);
            StrokeRound(g, Color(235, 196, 196, 200), std::max(1.0f, 2.0f * u), 1, 1, W - 2, H - 2, 9 * u);
            Icon(g, icon.get(), 10 * u, 10 * u, H - 20 * u, 4 * u, type);
            Text(g, head, *f1, Color(255, 250, 250, 250), text_x, 12 * u, W - text_x - 16 * u);
            TierTrophy(g, text_x, 48 * u, 19 * u, 25 * u, type, false);
            Text(g, title, *f2, Color(255, 236, 236, 236), text_x + 25 * u, 44 * u, W - text_x - 25 * u - 16 * u);
        });
        break;
    }
    case kXbox360: { // pilula cinza com borda clara, anel com 4 cortes + arco verde, esfera prateada / taca
        auto f = MakeFont(L"Segoe UI", 29 * u);
        const std::wstring l1 = pt ? L"Conquista desbloqueada" : L"Achievement unlocked";
        const std::wstring l2 = std::to_wstring(Gamerscore(type)) + L"G - " + title;
        const float H = 99 * u, D = 85 * u, text_x = 105 * u;
        const float W = std::clamp(std::ceil(text_x + std::max(TextWidth(measure, l1, *f), TextWidth(measure, l2, *f)) + 40 * u), 615 * u, 980 * u);
        out->pill_w = W, out->pill_h = H, out->text_x = text_x;
        b.Add(W, H, [&](Graphics& g, float, float) { // k3Pill
            LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(238, 74, 74, 74), Color(240, 50, 50, 50));
            FillRound(g, back, 1.5f * u, 1.5f * u, W - 3 * u, H - 3 * u, H * 0.5f);
            StrokeRound(g, Color(200, 20, 20, 20), std::max(1.0f, u), 0.5f, 0.5f, W - 1, H - 1, H * 0.5f);
            LinearGradientBrush rim(PointF(0, 0), PointF(0, H), Color(255, 214, 214, 214), Color(255, 150, 150, 150));
            Pen pen(&rim, std::max(1.0f, 2.5f * u));
            GraphicsPath path;
            RoundRect(path, 2.5f * u, 2.5f * u, W - 5 * u, H - 5 * u, H * 0.5f - 2.5f * u);
            g.DrawPath(&pen, &path);
        });
        b.Add(W - text_x - H * 0.3f, H, [&](Graphics& g, float w, float) { // k3Text
            Text(g, l1, *f, Color(255, 238, 238, 238), 0, 13 * u, w);
            Text(g, l2, *f, Color(255, 238, 238, 238), 0, 51 * u, w);
        });
        b.Add(D, D, [&](Graphics& g, float, float) { // k3Ring: aro preto, faixa cinza, 4 cortes
            Disc(g, D * 0.5f, D * 0.5f, D, Color(255, 8, 8, 8));
            LinearGradientBrush band(PointF(0, 0), PointF(0, D), Color(255, 140, 140, 140), Color(255, 84, 84, 84));
            g.FillEllipse(&band, D * 0.08f, D * 0.08f, D * 0.84f, D * 0.84f);
            Disc(g, D * 0.5f, D * 0.5f, D * 0.68f, Color(255, 8, 8, 8));
            SolidBrush notch(Color(255, 8, 8, 8));
            g.FillRectangle(&notch, D * 0.46f, 0.0f, D * 0.08f, D * 0.2f);
            g.FillRectangle(&notch, D * 0.46f, D * 0.8f, D * 0.08f, D * 0.2f);
            g.FillRectangle(&notch, 0.0f, D * 0.46f, D * 0.2f, D * 0.08f);
            g.FillRectangle(&notch, D * 0.8f, D * 0.46f, D * 0.2f, D * 0.08f);
        });
        for (int k = 1; k <= 6; ++k) { // k3Arc0..5: o arco verde (das 9 as 12 horas) crescendo
            b.Add(D, D, [&, k](Graphics& g, float, float) {
                const float sweep = 84.0f * k / 6.0f;
                LinearGradientBrush lime(PointF(0, D * 0.5f), PointF(D * 0.5f, 0), Color(255, 120, 196, 30), Color(255, 176, 236, 70));
                Pen pen(&lime, D * 0.15f);
                g.DrawArc(&pen, D * 0.155f, D * 0.155f, D * 0.69f, D * 0.69f, 183.0f, sweep);
            });
        }
        b.Add(D * 1.3f, D * 1.3f, [&](Graphics& g, float w, float) { Xbox360Sphere(g, w * 0.5f, w * 0.5f, w - 2); }); // k3Sphere
        b.Add(D * 0.62f, D * 0.62f, [&](Graphics& g, float w, float) { Cup(g, w * 0.1f, w * 0.06f, w * 0.8f, Color(255, 255, 255, 255)); });
        b.Add(D, D, [&](Graphics& g, float w, float) { Disc(g, w * 0.5f, w * 0.5f, w - 1, Color(255, 255, 255, 255)); }); // k3Disc
        break;
    }
    case kXboxOne: { // 2013: pulso verde, logo, pilula que abre dos dois lados; platina = conquista rara (azul, 2 linhas)
        const bool rare = type == 'P';
        const Color pill = rare ? Color(255, 15, 78, 126) : Color(255, 0, 105, 0);
        const float H = 95 * u;
        auto f = MakeFont(L"Segoe UI Semibold", (rare ? 22 : 27) * u);
        auto fg = MakeFont(L"Segoe UI Semibold", (rare ? 15 : 17) * u);
        const std::wstring head = rare ? (pt ? L"Conquista rara desbloqueada" : L"Rare Achievement unlocked")
                                       : (pt ? L"Conquista desbloqueada" : L"Achievement unlocked");
        const std::wstring score = std::to_wstring(Gamerscore(type)) + L" - " + title;
        const std::wstring hold = pt ? L"Segure" : L"Hold", details = rare ? (pt ? L"para abrir" : L"to launch") : (pt ? L"para detalhes" : L"for details");
        const float gw = 30 * u; // o "G" do gamerscore + espaco
        float text_w = std::max(TextWidth(measure, head, *f), gw + TextWidth(measure, score, *f));
        if (rare) {
            text_w = std::max(text_w, std::min(TextWidth(measure, desc, *f) * 0.55f, 520 * u));
        }
        const float W = std::clamp(std::ceil(H + 20 * u + text_w + H * 0.55f), 410 * u, 1100 * u);
        const float tw = W - H - 20 * u - H * 0.45f;
        out->pill_w = W, out->pill_h = H;
        b.Add(H * 1.25f, H * 1.25f, [&](Graphics& g, float w, float) { Disc(g, w * 0.5f, w * 0.5f, w - 1, Color(255, 255, 255, 255)); });
        b.Add(H, H, [&](Graphics& g, float w, float) { Disc(g, w * 0.5f, w * 0.5f, w - 0.5f, Color(255, 255, 255, 255)); });
        b.Add(H * 0.62f, H * 0.62f, [&](Graphics& g, float w, float) { XboxOneLogo(g, w * 0.5f, w * 0.5f, w - 1, pill); });
        b.Add(H * 0.62f, H * 0.62f, [&](Graphics& g, float w, float) { // k1Icon: taca (ou diamante na rara)
            if (rare) {
                Diamond(g, w * 0.5f, w * 0.5f, w * 0.92f);
            } else {
                Cup(g, w * 0.12f, w * 0.08f, w * 0.76f, Color(255, 255, 255, 255));
            }
        });
        auto badge_line = [&](Graphics& g, float x, float cy, float w) { // (G) 15 - nome
            const float s = 21 * u;
            Disc(g, x + s * 0.5f, cy, s, Color(255, 255, 255, 255));
            StringFormat center;
            center.SetAlignment(StringAlignmentCenter);
            center.SetLineAlignment(StringAlignmentCenter);
            SolidBrush letter(pill);
            g.DrawString(L"G", -1, fg.get(), RectF(x, cy - s * 0.5f + u, s, s), &center, &letter);
            TextMid(g, score, *f, Color(255, 255, 255, 255), x + gw, cy, w - gw);
        };
        b.Add(tw, H, [&](Graphics& g, float w, float) { // k1TextA
            if (rare) {
                TextMid(g, head, *f, Color(255, 255, 255, 255), 0, H * 0.33f, w);
                badge_line(g, 0, H * 0.68f, w);
            } else {
                TextMid(g, head, *f, Color(255, 255, 255, 255), 0, H * 0.33f, w);
            }
        });
        b.Add(tw, H, [&](Graphics& g, float w, float) { // k1TextB
            if (rare) { // a descricao em ate duas linhas
                StringFormat wrap;
                wrap.SetTrimming(StringTrimmingEllipsisWord);
                SolidBrush white(Color(255, 255, 255, 255));
                g.DrawString(desc.c_str(), -1, f.get(), RectF(0, H * 0.14f, w, H * 0.76f), &wrap, &white);
            } else {
                badge_line(g, 0, H * 0.5f, w);
            }
        });
        b.Add(TextWidth(measure, hold, *f) + 4 * u, H, [&](Graphics& g, float w, float) { TextMid(g, hold, *f, Color(255, 255, 255, 255), 0, H * 0.5f, w); });
        b.Add(TextWidth(measure, details, *f) + 4 * u, H, [&](Graphics& g, float w, float) { TextMid(g, details, *f, Color(255, 255, 255, 255), 0, H * 0.5f, w); });
        break;
    }
    case kWiiU: { // balao branco arredondado com o azul do sistema
        auto f1 = MakeFont(L"Segoe UI Semibold", 16 * u), f2 = MakeFont(L"Segoe UI Semibold", 19 * u);
        const float W = 480 * u, H = 84 * u;
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(250, 255, 255, 255), Color(250, 230, 233, 238));
            FillRound(g, back, 0.75f, 0.75f, W - 1.5f, H - 1.5f, 20 * u);
            StrokeRound(g, Color(255, 186, 192, 204), std::max(1.0f, 1.5f * u), 0.75f, 0.75f, W - 1.5f, H - 1.5f, 20 * u);
            Icon(g, icon.get(), 12 * u, 12 * u, H - 24 * u, 9 * u, type);
            const float x = H + 2 * u;
            Text(g, pt ? L"Conquista desbloqueada!" : L"Achievement unlocked!", *f1, Color(255, 0, 153, 221), x, 14 * u, W - x - 20 * u);
            Text(g, title, *f2, Color(255, 52, 52, 58), x, 39 * u, W - x - 20 * u);
        });
        break;
    }
    case kSteam: { // caixa azul escura no canto de baixo a direita
        auto f1 = MakeFont(L"Arial", 15 * u, FontStyleBold), f2 = MakeFont(L"Arial", 14 * u), f3 = MakeFont(L"Arial", 12 * u);
        const float W = 340 * u, H = 88 * u;
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            LinearGradientBrush back(PointF(0, 0), PointF(0, H), Color(250, 36, 54, 75), Color(250, 21, 29, 40));
            g.FillRectangle(&back, 0.0f, 0.0f, W, H);
            Pen border(Color(255, 58, 86, 116), 1.0f);
            g.DrawRectangle(&border, 0.5f, 0.5f, W - 1, H - 1);
            Icon(g, icon.get(), 12 * u, 12 * u, 64 * u, 0, type);
            const float x = 88 * u;
            Text(g, pt ? L"Conquista desbloqueada!" : L"Achievement Unlocked!", *f1, Color(255, 255, 255, 255), x, 16 * u, W - x - 10 * u);
            Text(g, title, *f2, Color(255, 184, 194, 206), x, 38 * u, W - x - 10 * u);
            Text(g, desc, *f3, Color(255, 128, 140, 152), x, 59 * u, W - x - 10 * u);
        });
        break;
    }
    default: { // Switch: aviso cinza escuro arredondado no canto de cima a esquerda
        auto f1 = MakeFont(L"Segoe UI Semibold", 18 * u), f2 = MakeFont(L"Segoe UI", 14.5f * u);
        const float W = 420 * u, H = 76 * u;
        out->pill_w = W, out->pill_h = H;
        b.Add(W, H, [&](Graphics& g, float, float) {
            SolidBrush back(Color(242, 46, 46, 50));
            FillRound(g, back, 0.5f, 0.5f, W - 1, H - 1, 12 * u);
            Icon(g, icon.get(), 12 * u, 12 * u, H - 24 * u, 6 * u, type);
            const float x = H + 4 * u;
            Text(g, title, *f1, Color(255, 255, 255, 255), x, 14 * u, W - x - 16 * u);
            Text(g, pt ? L"Conquista desbloqueada" : L"Achievement unlocked", *f2, Color(255, 0, 195, 227), x, 42 * u, W - x - 16 * u);
        });
        break;
    }
    }
    return b.Finish(out);
}

namespace {

float Clamp01(float t) {
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float Ramp(float t, float t0, float t1) { // 0 antes de t0, 1 depois de t1
    return Clamp01((t - t0) / (t1 - t0));
}

float EaseOut(float t) {
    t = Clamp01(t);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float EaseInOut(float t) {
    t = Clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

std::uint32_t Rgb(int r, int g, int b) {
    return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16);
}

std::uint32_t MixRgb(std::uint32_t a, std::uint32_t b, float t) {
    auto ch = [&](int shift) { return int(Lerp(float((a >> shift) & 0xFF), float((b >> shift) & 0xFF), Clamp01(t)) + 0.5f); };
    return Rgb(ch(0), ch(8), ch(16));
}

// interpolacao linear numa tabela (tempo, valor)
float Keys(float t, std::initializer_list<std::pair<float, float>> keys) {
    const auto* k = keys.begin();
    if (t <= k[0].first) {
        return k[0].second;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= k[i].first) {
            return Lerp(k[i - 1].second, k[i].second, (t - k[i - 1].first) / (k[i].first - k[i - 1].first));
        }
    }
    return k[keys.size() - 1].second;
}

struct Out {
    const Image& image;
    std::vector<Quad>* quads;

    // a peca inteira no retangulo (x, y, w, h)
    void Put(int part, float x, float y, float w, float h, float alpha, std::uint32_t tint = 0xFFFFFF, float angle = 0.0f) {
        if (alpha <= 0.003f || w <= 0.0f || h <= 0.0f || part >= int(image.parts.size())) {
            return;
        }
        const Part& p = image.parts[part];
        quads->push_back({x, y, x + w, y + h, float(p.x), float(p.y), float(p.x + p.w), float(p.y + p.h), std::min(alpha, 1.0f), tint, angle});
    }
    // a peca no tamanho natural, centrada em (cx, cy) e escalada
    void Center(int part, float cx, float cy, float scale, float alpha, std::uint32_t tint = 0xFFFFFF, float angle = 0.0f) {
        if (part >= int(image.parts.size())) {
            return;
        }
        const Part& p = image.parts[part];
        Put(part, cx - p.w * scale * 0.5f, cy - p.h * scale * 0.5f, p.w * scale, p.h * scale, alpha, tint, angle);
    }
    // uma faixa da peca (em pixels da peca) para um retangulo da tela, cortada pelo retangulo de recorte
    void Slice(int part, float su0, float su1, float x0, float y0, float x1, float y1, float alpha, std::uint32_t tint, float cx0, float cy0,
               float cx1, float cy1) {
        if (alpha <= 0.003f || part >= int(image.parts.size()) || x1 <= x0 || y1 <= y0) {
            return;
        }
        const Part& p = image.parts[part];
        float u0 = p.x + su0, u1 = p.x + su1, v0 = float(p.y), v1 = float(p.y + p.h);
        // recorte simples e exato: calcula as fracoes visiveis em x e y
        const float fx0 = Clamp01((cx0 - x0) / (x1 - x0)), fx1 = Clamp01((cx1 - x0) / (x1 - x0));
        const float fy0 = Clamp01((cy0 - y0) / (y1 - y0)), fy1 = Clamp01((cy1 - y0) / (y1 - y0));
        if (fx1 <= fx0 || fy1 <= fy0) {
            return;
        }
        const float nx0 = Lerp(x0, x1, fx0), nx1 = Lerp(x0, x1, fx1), ny0 = Lerp(y0, y1, fy0), ny1 = Lerp(y0, y1, fy1);
        const float nu0 = Lerp(u0, u1, fx0), nu1 = Lerp(u0, u1, fx1), nv0 = Lerp(v0, v1, fy0), nv1 = Lerp(v0, v1, fy1);
        quads->push_back({nx0, ny0, nx1, ny1, nu0, nv0, nu1, nv1, std::min(alpha, 1.0f), tint, 0.0f});
    }
    // a peca no tamanho natural em (x, y), recortada
    void Clipped(int part, float x, float y, float alpha, float cx0, float cy0, float cx1, float cy1) {
        if (part >= int(image.parts.size())) {
            return;
        }
        const Part& p = image.parts[part];
        Slice(part, 0, float(p.w), x, y, x + p.w, y + p.h, alpha, 0xFFFFFF, cx0, cy0, cx1, cy1);
    }
};

// pilula do Xbox One feita do disco branco (pontas) + o meio esticado, tingida
void OnePill(Out& o, float cx, float y, float half_w, float H, std::uint32_t tint, float alpha) {
    const Part& d = o.image.parts[k1PillDisc];
    const float r = H * 0.5f, dw = float(d.w);
    half_w = std::max(half_w, r);
    const float big = 1e6f;
    o.Slice(k1PillDisc, 0, dw * 0.5f, cx - half_w, y, cx - half_w + r, y + H, alpha, tint, -big, -big, big, big);
    if (half_w > r) {
        o.Slice(k1PillDisc, dw * 0.5f - 0.5f, dw * 0.5f + 0.5f, cx - half_w + r, y, cx + half_w - r, y + H, alpha, tint, -big, -big, big, big);
    }
    o.Slice(k1PillDisc, dw * 0.5f, dw, cx + half_w - r, y, cx + half_w, y + H, alpha, tint, -big, -big, big, big);
}

void LayoutXboxOne(Out& o, float cw, float ch, float t, float remaining) {
    const Image& im = o.image;
    const bool rare = im.type == 'P';
    const OneTimes& T = rare ? kOneRare : kOneNormal;
    const float H = im.pill_h, W = im.pill_w, u = im.u;
    const float cx = std::round(cw * 0.5f), y = std::round(ch * 0.829f), cy = y + H * 0.5f;
    const std::uint32_t pill = rare ? Rgb(15, 78, 126) : Rgb(0, 105, 0);
    const std::uint32_t light = rare ? Rgb(28, 128, 204) : Rgb(68, 156, 12);
    const std::uint32_t pulse = rare ? Rgb(90, 176, 236) : Rgb(122, 204, 52);
    const std::uint32_t mid = rare ? Rgb(22, 104, 170) : Rgb(48, 142, 16);
    const float disc = float(im.parts[k1Disc].w); // o disco grande tem 1,25 H
    (void)remaining;

    // ---- saida (fecha no centro e o circulo encolhe) ----
    if (t >= T.close) {
        const float k = t - T.close;
        const float half = Lerp(W * 0.5f, H * 0.5f, EaseInOut(k / 0.28f));
        const float text_a = 1.0f - Ramp(k, 0.0f, 0.12f);
        if (k < 0.42f) {
            OnePill(o, cx, y, half, H, pill, 1.0f);
            const float hw = o.image.parts[k1TextHold].w, dw = o.image.parts[k1TextDetails].w, ls = H * 0.62f;
            o.Clipped(k1TextHold, cx - ls * 0.5f - 10 * u - hw, y, text_a, cx - half, y, cx + half, y + H);
            o.Clipped(k1TextDetails, cx + ls * 0.5f + 10 * u, y, text_a, cx - half, y, cx + half, y + H);
        }
        // circulo: aro claro aparece, encolhe e some
        const float d = Keys(k, {{0.28f, H}, {0.42f, H * 1.04f}, {0.62f, H * 0.88f}, {0.82f, H * 0.6f}, {0.92f, 0.0f}});
        const float rim = Ramp(k, 0.3f, 0.5f);
        if (k >= 0.26f && d > 0.0f) {
            o.Center(k1Disc, cx, cy, d / disc, 1.0f, MixRgb(pill, pulse, rim));
            o.Center(k1Disc, cx, cy, d * Lerp(1.0f, 0.82f, rim) / disc, 1.0f, MixRgb(pill, mid, Ramp(k, 0.55f, 0.75f)));
        }
        o.Center(k1Logo, cx, cy, Keys(k, {{0.4f, 1.0f}, {0.62f, 0.9f}, {0.72f, 0.0f}}), 1.0f - Ramp(k, 0.6f, 0.7f));
        return;
    }

    // ---- entrada: pulso de circulos ----
    const float dl = Keys(t, {{0.0f, 0.0f}, {0.067f, H * 0.1f}, {0.133f, H * 0.32f}, {0.2f, H * 0.54f}, {0.267f, H * 0.76f}, {0.333f, H * 1.05f},
                              {0.4f, H * 1.17f}, {0.6f, H * 1.17f}, {1.2f, H * 1.1f}, {T.open0, H * 1.08f}, {T.open1, H}});
    const float dm = Keys(t, {{0.133f, 0.0f}, {0.333f, 0.6f}, {0.467f, 0.9f}, {0.6f, 1.0f}});
    const float dc = Keys(t, {{0.133f, 0.2f}, {0.36f, 0.22f}, {0.55f, 0.95f}, {0.62f, 1.0f}});
    // posicao do circulo: centro da tela -> ponta esquerda da pilula
    const float open = EaseOut(Ramp(t, T.open0, T.open1));
    const float r_end = H * 0.5f;
    const float circle_x = Lerp(cx, cx - W * 0.5f + r_end, open);
    const float half = cx - (circle_x - Lerp(dl * 0.5f, r_end, open));
    if (open > 0.0f) {
        OnePill(o, cx, y, half, H, pill, 1.0f);
    }
    // o circulo muda de verde escuro (com o logo) para o claro (com a taca)
    const float to_light = Ramp(t, T.open0 + 0.12f, T.open0 + 0.33f);
    const float icon_gone = Ramp(t, T.b_out, T.b_out + 0.27f); // a taca e o circulo claro somem antes do "Hold"
    if (dl > 0.0f && icon_gone < 1.0f) {
        const float a = 1.0f - icon_gone;
        o.Center(k1Disc, circle_x, cy, dl / disc, a, t < 0.62f ? pulse : MixRgb(pill, light, to_light));
        if (t < 0.7f) {
            o.Center(k1Disc, circle_x, cy, dl * dm / disc, a, mid);
            o.Center(k1Disc, circle_x, cy, dl * dc / disc, a, pill);
        }
    }
    // logo: surge e, quando a pilula abre, rola para a esquerda e vira um disco branco e depois a taca
    const float logo_in = Keys(t, {{0.6f, 0.0f}, {0.8f, 1.0f}});
    const float roll = (circle_x - cx) / (H * 0.31f); // radianos (rolar para a esquerda = anti-horario)
    const float logo_out = Ramp(t, T.open0 + 0.10f, T.open0 + 0.2f);
    o.Center(k1Logo, circle_x, cy, logo_in * dl / H, 1.0f - logo_out, 0xFFFFFF, roll);
    const float white = std::min(Ramp(t, T.open0 + 0.12f, T.open0 + 0.2f), 1.0f - Ramp(t, T.open0 + 0.26f, T.open0 + 0.33f));
    o.Center(k1Disc, circle_x, cy, H * 0.62f / disc, white);
    const float cup = Ramp(t, T.open0 + 0.26f, T.open0 + 0.36f) * (1.0f - icon_gone);
    o.Center(k1Icon, circle_x, cy, 1.0f, cup);

    // ---- textos que trocam subindo ----
    const float lift = 15 * u;
    const float tx = cx - W * 0.5f + H + 20 * u, top = y, bottom = y + H;
    const float left_clip = circle_x + H * 0.5f, right_clip = cx + half - H * 0.25f;
    if (t >= T.open1) {
        const float a_in = Ramp(t, T.open1, T.open1 + 0.25f), a_out = Ramp(t, T.a_out, T.a_out + 0.2f);
        o.Clipped(k1TextA, tx, y - lift * EaseOut(a_out), a_in * (1.0f - a_out), left_clip, top, right_clip, bottom);
        const float b_in = Ramp(t, T.b_in, T.b_in + 0.3f), b_out = Ramp(t, T.b_out, T.b_out + 0.27f);
        o.Clipped(k1TextB, tx, y + lift * (1.0f - EaseOut(b_in)), b_in * (1.0f - b_out), left_clip, top, right_clip, bottom);
    }
    if (t >= T.c_in) { // "Hold (logo) for details": o logo gira para o lugar no meio
        const float c = Ramp(t, T.c_in, T.c_in + 0.25f);
        const float hw = o.image.parts[k1TextHold].w, ls = H * 0.62f;
        o.Put(k1TextHold, cx - ls * 0.5f - 10 * u - hw, y, hw, H, c);
        o.Put(k1TextDetails, cx + ls * 0.5f + 10 * u, y, float(o.image.parts[k1TextDetails].w), H, c);
        o.Center(k1Logo, cx, cy, 1.0f, c, 0xFFFFFF, (1.0f - EaseOut(c)) * -1.6f);
    }
}

void LayoutXbox360(Out& o, float cw, float ch, float t, float remaining) {
    const Image& im = o.image;
    const float W = im.pill_w, H = im.pill_h, u = im.u;
    const float aw = std::min(cw, ch * 16.0f / 9.0f), ax = (cw - aw) * 0.5f;
    const float x = std::round(ax + aw * 0.084f), y = std::round(ch * 0.8194f);
    const float D = float(im.parts[k3Ring].w), rcx = x + 46 * u, rcy = y + H * 0.5f;
    // entrada: a pilula estica (passa um pouco) e a esfera grande encolhe para dentro do anel
    float width = Keys(t, {{0.0f, 0.40f}, {0.067f, 0.71f}, {0.133f, 1.04f}, {0.2f, 1.03f}, {0.267f, 1.0f}});
    float pill_a = Keys(t, {{0.0f, 0.45f}, {0.067f, 0.75f}, {0.133f, 1.0f}});
    float text_a = Ramp(t, 0.08f, 0.14f);
    float ring_a = Keys(t, {{0.03f, 0.0f}, {0.067f, 0.6f}, {0.133f, 1.0f}});
    float ring_s = 1.0f;
    // saida: o texto some, a pilula fecha no anel, o anel encolhe e some
    if (remaining < 0.5f) {
        const float k = 0.5f - remaining;
        text_a *= 1.0f - Ramp(k, 0.0f, 0.12f);
        width = Lerp(1.0f, (D * 1.02f) / W, EaseInOut(Ramp(k, 0.05f, 0.3f)));
        pill_a *= 1.0f - Ramp(k, 0.28f, 0.36f);
        ring_s = 1.0f - 0.35f * Ramp(k, 0.3f, 0.5f);
        ring_a *= 1.0f - Ramp(k, 0.34f, 0.5f);
    }
    const float wc = W * width, cap = H * 0.5f;
    const float big = 1e6f;
    // pilula: parte esquerda (cortada, ou esticada quando passa do tamanho) + ponta direita
    const float left_w = std::max(0.0f, wc - cap);
    o.Slice(k3Pill, 0, std::min(left_w, W - cap), x, y, x + left_w, y + H, pill_a, 0xFFFFFF, -big, -big, big, big);
    o.Slice(k3Pill, W - cap, W, x + left_w, y, x + left_w + cap, y + H, pill_a, 0xFFFFFF, -big, -big, big, big);
    o.Clipped(k3Text, x + im.text_x, y, text_a * pill_a, x, y, x + wc - cap * 0.4f, y + H);
    // anel com o arco verde crescendo das 9 as 12 horas
    o.Center(k3Ring, rcx, rcy, ring_s, ring_a);
    const int arc = std::min(5, int(Ramp(t, 0.1f, 0.45f) * 6.0f - 0.001f));
    if (t >= 0.1f && arc >= 0) {
        o.Center(k3Arc0 + arc, rcx, rcy, ring_s, ring_a);
    }
    // miolo: esfera e taca se revezam passando pelo escuro (como o 360 faz por uns 3,5 s)
    const float inner = 0.62f * D;
    const float sphere_d = float(im.parts[k3Sphere].w);
    if (t < 0.133f) { // a esfera grande da entrada
        const float d = Keys(t, {{0.0f, 1.25f * D}, {0.067f, 0.95f * D}, {0.133f, inner}});
        o.Center(k3Sphere, rcx, rcy, d / sphere_d, Keys(t, {{0.0f, 0.9f}, {0.067f, 1.0f}}));
        return;
    }
    o.Center(k3Disc, rcx, rcy, inner * ring_s / D, ring_a, Rgb(8, 8, 8));
    // (inicio, fim) de cada aparicao: esfera, taca, esfera, taca, esfera ate o fim
    struct Show {
        int part;
        float t0, t1;
    };
    static const Show kShows[] = {{k3Sphere, 0.0f, 0.45f}, {k3Cup, 0.52f, 1.42f}, {k3Sphere, 1.49f, 2.36f}, {k3Cup, 2.43f, 3.36f}, {k3Sphere, 3.45f, 99.0f}};
    for (const Show& s : kShows) {
        const float a = std::min(Ramp(t, s.t0, s.t0 + 0.07f), 1.0f - Ramp(t, s.t1, s.t1 + 0.07f)) * ring_a;
        if (a > 0.0f) {
            const float scale = (s.part == k3Sphere ? inner / sphere_d : 1.0f) * ring_s;
            o.Center(s.part, rcx, rcy, scale, a);
        }
    }
}

} // namespace

void Layout(const Image& image, float cw, float ch, float in, float out, std::vector<Quad>* quads) {
    Out o{image, quads};
    const float u = image.u, W = image.pill_w, H = image.pill_h;
    const float aw = std::min(cw, ch * 16.0f / 9.0f), ax = (cw - aw) * 0.5f;
    switch (image.style) {
    case kPs5: { // desliza 38 px da direita com fade em 0,2 s; sai igual em 0,25 s
        const float a = std::min(EaseOut(in / 0.2f), EaseOut(out / 0.25f));
        o.Put(0, std::round(ax + aw - 80 * u - W) + (1.0f - a) * 38 * u, std::round(60 * u), W, H, a);
        break;
    }
    case kPs4: { // canto de cima a esquerda: entra deslizando da esquerda com fade
        const float a = std::min(EaseOut(in / 0.3f), EaseOut(out / 0.35f));
        o.Put(0, std::round(ax + aw * 0.05f) - (1.0f - a) * 40 * u, std::round(ch * 0.075f), W, H, a);
        break;
    }
    case kPs3: // canto de cima a direita: fade
        o.Put(0, std::round(ax + aw * 0.95f - W), std::round(ch * 0.075f), W, H, std::min(Ramp(in, 0, 0.3f), Ramp(out, 0, 0.4f)));
        break;
    case kXboxOne:
        LayoutXboxOne(o, cw, ch, in, out);
        break;
    case kXbox360:
        LayoutXbox360(o, cw, ch, in, out);
        break;
    case kWiiU: { // desce do alto da tela
        const float t = std::min(EaseOut(in / 0.35f), EaseOut(out / 0.30f));
        o.Put(0, std::round((cw - W) * 0.5f), 30 * u - (1.0f - t) * (H + 40 * u), W, H, 1.0f);
        break;
    }
    case kSteam: { // sobe da borda de baixo
        const float t = std::min(EaseOut(in / 0.40f), EaseOut(out / 0.40f));
        o.Put(0, cw - W - 14 * u, ch - H - 14 * u + (1.0f - t) * (H + 20 * u), W, H, 1.0f);
        break;
    }
    default: { // Switch: desce um pouco com fade
        const float a = std::min(EaseOut(in / 0.25f), EaseOut(out / 0.25f));
        o.Put(0, ax + 36 * u, 36 * u - (1.0f - a) * 14 * u, W, H, a);
        break;
    }
    }
}

} // namespace hle::trophy_popup
