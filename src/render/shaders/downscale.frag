#version 450
// 1.9.1: reducao para a janela quando a imagem do jogo e maior que a area dela (ex.: 2560x1080 num monitor 1920x1080, ou
// 1600x1200 em 1440x1080). O bilinear simples pula pixels da imagem e as texturas finas / bordas cintilam; aqui cada pixel
// da janela e a media (filtro tenda) de toda a area da imagem que cai nele.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Constants {
    vec4 src;    // largura, altura, 1 / largura, 1 / altura da imagem
    vec4 dst;    // largura e altura da area na janela, escala x e y (imagem / area, >= 1)
    vec4 unused0, unused1;
    vec2 origin; // canto da area na janela
} pc;

layout(location = 0) out vec4 color;

void main() {
    vec2 p = (gl_FragCoord.xy - pc.origin) / pc.dst.xy;   // 0..1 na area
    vec2 center = p * pc.src.xy;                          // em pixels da imagem
    vec2 radius = max(pc.dst.zw * 0.5 + 0.5, vec2(1.0));   // meia largura da tenda, em pixels da imagem
    ivec2 taps = ivec2(min(ceil(radius * 2.0), vec2(8.0)));
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = 0; y < taps.y; ++y) {
        for (int x = 0; x < taps.x; ++x) {
            vec2 offset = ((vec2(x, y) + 0.5) / vec2(taps) * 2.0 - 1.0) * radius;
            vec2 w2 = max(vec2(0.0), 1.0 - abs(offset) / radius);
            float w = w2.x * w2.y;
            sum += texture(source, (center + offset) * pc.src.zw).rgb * w;
            total += w;
        }
    }
    color = vec4(sum / max(total, 1e-5), 1.0);
}
