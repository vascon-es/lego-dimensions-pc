// 1.9.1: partes comuns do TAA e das entradas dos upscalers temporais (incluido por taa.frag e temporal_inputs.frag)
layout(set = 0, binding = 0) uniform sampler2D current;
layout(set = 0, binding = 1) uniform sampler2D history;    // TAA: o resultado anterior (as entradas nao usam)
layout(set = 0, binding = 2) uniform sampler2D depth;      // profundidade da cena (alvo do jogo)
layout(set = 0, binding = 3) uniform sampler2D prev_depth; // copia R32F da profundidade do quadro anterior

layout(push_constant) uniform Constants {
    mat4 reproject;   // NDC deste quadro (x, y, profundidade, 1) -> recorte do quadro anterior
    vec4 size;        // largura, altura, 1 / largura, 1 / altura
    vec4 params;      // x: historico valido (TAA), y: reprojecao valida, z: peso do quadro atual (TAA), w: profundidade anterior valida
    vec4 depth_scale; // xy: uv da imagem -> uv da profundidade
    vec4 camera;      // P22 e P32 deste quadro e do anterior: distancia = P32 / (profundidade - P22)
} pc;

float LinearDepth(float d, float p22, float p32) {
    float den = d - p22;
    return abs(den) < 1e-7 ? 1e7 : clamp(p32 / den, 0.0, 1e7);
}

// O quanto o ponto deste pixel NAO estava ali no quadro anterior, so pela camera: 0 = parado (o historico vale),
// 1 = algo se moveu sozinho ou apareceu (personagem andando, objeto, particula). Compara a distancia que o ponto teria
// no quadro anterior (pela camera) com a profundidade guardada do quadro anterior, na vizinhanca 3x3 (o deslocamento
// de sub-pixel muda as bordas de um quadro para o outro).
// clip_w: o w da reprojecao (NDC deste quadro -> recorte do anterior), que vem dividido pela distancia atual.
float MovedAmount(vec2 previous_uv, float clip_w, float current_depth) {
    if (pc.params.w < 0.5 || clip_w <= 0.0 || current_depth >= 0.99999) {
        return pc.params.x > 1.5 && pc.params.w < 0.5 ? 0.3 : 0.0; // ceu / sem profundidade anterior (diagnostico: 0.3)
    }
    float expected = clip_w * LinearDepth(current_depth, pc.camera.x, pc.camera.y);
    float best = 1e9;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = LinearDepth(texture(prev_depth, previous_uv + vec2(x, y) * pc.size.zw).r, pc.camera.z, pc.camera.w);
            best = min(best, abs(stored - expected) / max(expected, 1e-3));
        }
    }
    if (pc.params.x > 1.5) {
        return clamp(best * 10.0, 0.0, 1.0); // DIAGNOSTICO (LD_SHOW_REACTIVE=2): a diferenca relativa x 10
    }
    return smoothstep(0.02, 0.08, best);
}
