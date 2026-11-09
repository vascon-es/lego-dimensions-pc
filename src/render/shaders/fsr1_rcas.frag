#version 450
// 1.9.1: AMD FidelityFX FSR 1 - RCAS (nitidez adaptativa). Le a imagem ampliada pelo EASU e escreve na janela.
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Constants {
    uvec4 con;
    uvec4 unused0, unused1, unused2;
    vec2 origin; // canto da imagem do jogo na janela (faixas pretas fora dela)
} pc;

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(source, ASU2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

layout(location = 0) out vec4 color;

void main() {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy - pc.origin), pc.con);
    color = vec4(c, 1.0);
}
