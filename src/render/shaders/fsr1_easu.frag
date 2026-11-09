#version 450
// 1.9.1: AMD FidelityFX FSR 1 - EASU (ampliacao com deteccao de bordas). Le a imagem do jogo (resolucao de render) e
// escreve uma imagem do tamanho da saida. Compilado por tools/shaders/gerar_fsr1.py (glslc) -> src/render/fsr1_spv.h.
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Constants {
    uvec4 con0, con1, con2, con3;
    vec2 origin; // canto da imagem de saida na janela (aqui sempre 0)
} pc;

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(source, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source, p, 2); }
#include "ffx_fsr1.h"

layout(location = 0) out vec4 color;

void main() {
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy - pc.origin), pc.con0, pc.con1, pc.con2, pc.con3);
    color = vec4(c, 1.0);
}
