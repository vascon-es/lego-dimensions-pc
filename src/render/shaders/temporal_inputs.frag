#version 450
// 1.9.1: entradas dos upscalers temporais (AMD FSR 2 / 3 / 4) na resolucao de render: a cor do quadro (copia em RGBA16F),
// os vetores de movimento (uv anterior - uv atual, so da camera: reprojetados pela profundidade da cena e pelas cameras
// deste quadro e do anterior), a profundidade (copia em R32F, do tamanho da cor: o alvo do PS4 tem a altura alinhada a 8;
// vira a "profundidade anterior" do proximo quadro) e a mascara reativa: onde algo se moveu sozinho (o vetor so da
// camera esta errado ali), o FSR usa o quadro atual em vez do historico - sem fantasma nos personagens.
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 motion;
layout(location = 2) out float depth_out;
layout(location = 3) out float reactive;

void main() {
    vec2 uv = gl_FragCoord.xy * pc.size.zw;
    color = vec4(texture(current, uv).rgb, 1.0);
    motion = vec2(0.0);
    depth_out = 1.0;
    reactive = 0.0;
    if (pc.params.y > 0.5) {
        float d = texture(depth, uv * pc.depth_scale.xy).r;
        depth_out = d;
        // PS4: Y do recorte para cima (o topo da imagem e +1)
        vec4 clip = pc.reproject * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
        if (clip.w > 1e-5) {
            vec2 p = clip.xy / clip.w;
            vec2 previous_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
            motion = previous_uv - uv;
            reactive = 0.9 * MovedAmount(previous_uv, clip.w, d);
        }
    }
}
