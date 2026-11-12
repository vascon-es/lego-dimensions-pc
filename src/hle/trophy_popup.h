#pragma once
// 3.0: os pop-ups de conquista no visual de cada console, copiados quadro a quadro de gravacoes dos consoles (PS5, Xbox 360,
// Xbox One e a conquista rara do Xbox One). Render desenha as pecas de cada estilo (cartao, pilula, anel, esfera, taca,
// textos...) com GDI+ numa imagem so; Layout diz, quadro a quadro, onde e como desenhar cada peca (posicao, escala,
// rotacao, cor e transparencia). O overlay do hle_videoout manda a imagem como textura e desenha os retangulos.
#include <cstdint>
#include <string>
#include <vector>

namespace hle::trophy_popup {

// Estilos, na ordem do menu e do launcher. "xbox" e o Xbox One (o nome antigo do ini continua valendo).
constexpr int kStyleCount = 8;
extern const char* const kStyleKeys[kStyleCount];  // ps5, ps4, ps3, xbox, xbox360, wiiu, steam, switch
extern const char* const kStyleNames[kStyleCount]; // PS5, PS4, PS3, XBOX ONE, XBOX 360, WII U, STEAM, SWITCH
int StyleIndex(const std::string& key);             // -1 se nao conhece

// Quanto tempo o pop-up fica na tela (a animacao de cada console tem o seu tempo; platina no Xbox One = conquista rara)
float Duration(const std::string& style, char type);

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
    std::vector<Part> parts;         // as pecas, na ordem de cada estilo
    int style = 0;
    char type = 'B';
    float u = 1.0f;                  // escala usada no desenho
    float pill_w = 0, pill_h = 0;    // tamanho final do cartao / pilula
    float text_x = 0;                // Xbox 360: onde o texto comeca dentro da pilula
};

bool Render(const Request& request, Image* out);

struct Quad {
    float x0, y0, x1, y1; // na janela, em pixels
    float u0, v0, u1, v1; // na imagem, em pixels
    float alpha;
    std::uint32_t tint = 0xFFFFFF; // cor multiplicada (R no byte baixo)
    float angle = 0.0f;            // rotacao em radianos em volta do centro
};

// Onde e como desenhar as pecas neste quadro: `elapsed` desde que o pop-up apareceu, `remaining` ate sumir.
void Layout(const Image& image, float client_w, float client_h, float elapsed, float remaining, std::vector<Quad>* quads);

} // namespace hle::trophy_popup
