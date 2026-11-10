#version 450
// 1.9.1: anti-aliasing temporal do port. Cada quadro o jogo e desenhado com a camera deslocada um pouco (sequencia de
// Halton de 16 posicoes, menos de um pixel); aqui a imagem do quadro e misturada com o historico (o resultado dos quadros
// anteriores), reprojetado pela profundidade da cena e pelas cameras deste quadro e do anterior. O historico e limitado
// pela vizinhanca de 3x3 do quadro atual (em YCoCg), o que tira o rastro quando algo se move sozinho ou aparece.
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

layout(location = 0) out vec4 color;
layout(location = 1) out float depth_out; // copia da profundidade (a anterior do proximo quadro)

vec3 ToYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 FromYCoCg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom com 5 leituras bilineares (historico mais nitido que o bilinear simples)
vec3 SampleHistory(vec2 uv) {
    vec2 position = uv * pc.size.xy;
    vec2 center = floor(position - 0.5) + 0.5;
    vec2 f = position - center;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc12 = (center + w2 / w12) * pc.size.zw;
    vec2 tc0 = (center - 1.0) * pc.size.zw;
    vec2 tc3 = (center + 2.0) * pc.size.zw;
    vec3 result = texture(history, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y) +
                  texture(history, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y) +
                  texture(history, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y) +
                  texture(history, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y) +
                  texture(history, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(result / weight, vec3(0.0));
}

void main() {
    vec2 uv = gl_FragCoord.xy * pc.size.zw;
    vec3 now = texture(current, uv).rgb;
    depth_out = pc.params.y > 0.5 ? texture(depth, uv * pc.depth_scale.xy).r : 1.0;
    if (pc.params.x < 0.5) {
        color = vec4(now, 1.0);
        return;
    }
    // vizinhanca 3x3 em YCoCg: media e desvio (recorte por variancia) + o ponto mais proximo da camera (bordas)
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), box_min = vec3(1e9), box_max = vec3(-1e9);
    float nearest = 1.0;
    vec2 nearest_uv = uv;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = uv + vec2(x, y) * pc.size.zw;
            vec3 c = ToYCoCg(texture(current, tap).rgb);
            m1 += c;
            m2 += c * c;
            box_min = min(box_min, c);
            box_max = max(box_max, c);
            if (pc.params.y > 0.5) {
                float d = texture(depth, tap * pc.depth_scale.xy).r;
                if (d < nearest) {
                    nearest = d;
                    nearest_uv = tap;
                }
            }
        }
    }
    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    // caixa = min / max da vizinhanca cruzada com media +- desvio (mais justa: menos fantasma em quem se move sozinho)
    // 2.0: caixa um pouco mais larga (1,25 desvio): o deslocamento por quadro muda sombras / texturas finas e a caixa justa
    // descartava o historico ali - piscava
    vec3 lo = max(box_min, mean - 1.25 * sigma), hi = min(box_max, mean + 1.25 * sigma);

    vec2 previous_uv = uv;
    float moved = 0.0;
    if (pc.params.y > 0.5) {
        // PS4: Y do recorte para cima (o topo da imagem e +1)
        vec4 ndc = vec4(nearest_uv.x * 2.0 - 1.0, 1.0 - nearest_uv.y * 2.0, nearest, 1.0);
        vec4 clip = pc.reproject * ndc;
        if (clip.w > 1e-5) {
            vec2 p = clip.xy / clip.w;
            previous_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) + (uv - nearest_uv);
            // algo se moveu sozinho ali (o vetor so da camera esta errado): o quadro atual, sem historico
            vec4 own = pc.reproject * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth_out, 1.0);
            if (own.w > 1e-5) {
                vec2 q = own.xy / own.w;
                moved = MovedAmount(vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5), own.w, depth_out);
            }
        }
    }
    if (any(lessThan(previous_uv, vec2(0.0))) || any(greaterThan(previous_uv, vec2(1.0)))) {
        color = vec4(now, 1.0);
        return;
    }
    vec3 raw = ToYCoCg(SampleHistory(previous_uv));
    // recorte em direcao ao centro da caixa (mantem a cor, so traz para dentro)
    vec3 center = 0.5 * (hi + lo), extent = max(0.5 * (hi - lo), vec3(1e-4));
    vec3 offset = raw - center;
    vec3 ratio = abs(offset / extent);
    float outside = max(ratio.x, max(ratio.y, ratio.z));
    vec3 past = outside > 1.0 ? center + offset / outside : raw;
    // mais peso ao quadro atual quando o historico foi muito recortado (algo se moveu ou apareceu) - menos rastro
    float rejected = clamp(length(raw - past) / max(mean.x, 0.02) * 0.5, 0.0, 1.0);
    float weight = max(mix(pc.params.z, 0.35, rejected), moved);
    color = vec4(max(mix(FromYCoCg(past), now, weight), vec3(0.0)), 1.0);
}
