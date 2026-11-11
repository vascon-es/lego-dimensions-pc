#pragma once
// 3.0: os pop-ups de conquista desenhados como imagem (GDI+: fonte suavizada, cantos arredondados, o icone da conquista em
// resolucao cheia) no visual de cada console, e a animacao de cada um. O overlay do hle_videoout manda a imagem como
// textura e desenha os retangulos que Layout devolve.
#include <cstdint>
#include <string>
#include <vector>

namespace hle::trophy_popup {

// Estilos, na ordem do menu e do launcher. "xbox" e o Xbox One (o nome antigo do ini continua valendo).
constexpr int kStyleCount = 8;
extern const char* const kStyleKeys[kStyleCount];  // ps5, ps4, ps3, xbox, xbox360, wiiu, steam, switch
extern const char* const kStyleNames[kStyleCount]; // PS5, PS4, PS3, XBOX ONE, XBOX 360, WII U, STEAM, SWITCH
int StyleIndex(const std::string& key);             // -1 se nao conhece

struct Request {
    std::string style;
    int trophy_id = -1;
    char type = 'B';
    std::string title, desc; // UTF-8 (ou ANSI)
    bool pt = false;
    float scale = 1.0f; // 1 = 1080p
};

struct Part {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint32_t> rgba; // R no byte baixo, alpha nao pre-multiplicado
    Part card;                       // o cartao inteiro
    Part orb;                        // Xbox One / 360: o circulo, desenhado por cima (w = 0 nos outros)
};

bool Render(const Request& request, Image* out);

struct Quad {
    float x0, y0, x1, y1; // na janela, em pixels
    float u0, v0, u1, v1; // na imagem, em pixels
    float alpha;
};

// Onde e como desenhar a imagem neste quadro: `elapsed` desde que o pop-up apareceu, `remaining` ate sumir.
void Layout(const std::string& style, const Image& image, float client_w, float client_h, float scale, float elapsed,
            float remaining, std::vector<Quad>* quads);

} // namespace hle::trophy_popup
