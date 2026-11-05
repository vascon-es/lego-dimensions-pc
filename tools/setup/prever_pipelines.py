#!/usr/bin/env python3
"""Fase 77: receitas de pipeline previstas para os pares de shaders do pacote AOT que ninguem jogou ainda.

O que engasga na primeira visita a um conteudo novo e o pipeline (shader + estado do draw), nao o shader: o
Preparar ja traduz todos os shaders. O estado do draw so aparece quando o jogo desenha, mas quase nunca e novo:
medido escondendo uma area inteira, 1144 de 1145 receitas novas usavam um estado de saida (alvos, blend,
profundidade) e 1139 um layout de vertices que outras areas ja tinham. O que muda e o par de shaders.

Previsao: os pares sao agrupados pelo layout de recursos dos dois shaders (entradas do VS e slots do PS, do
shaders.tsv do extrator); um par nunca visto recebe os N estados (saida + vertices) mais comuns do seu grupo, em
todas as variantes do pacote (a variante usada so se sabe no jogo). Medido nas areas escondidas: N=2 acerta 44%
das receitas novas dos pares do pacote com ~10 previsoes por acerto.

O lego_dimensions.exe --prewarm-only --prewarm-predicted cria cada pipeline previsto uma vez (vai para o cache do
driver) e o destroi; no jogo, o primeiro uso sai do cache do driver. As sessoes normais nao leem o arquivo.

Medido na RX 6600 (driver 26.6.1): 184 mil previstos em 4 min; no jogo, mediana de 1,2 ms por pipeline previsto em vez de
~30 ms. Custo: o driver da AMD carrega o cache inteiro na memoria em toda sessao (2,2 GB em vez de 0,4 GB = +1,8 GB de
RAM). Por isso e opcional (preparar.py --prever).

uso: prever_pipelines.py [--root PASTA_DO_PORT] [--n 2]
saida: assets/shaders_spv/pipeline_states_previstas.txt ("# ld-pipeline-states 2", uma chave por linha)
"""
import argparse
import collections
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
M = (1 << 64) - 1
K1, K2, AOT_XOR = 0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F, 0xA070000000000A00


def pair_key(vs, ps, sig):
    """a chave do par do pacote AOT (hle_gnm LoadGamePair)"""
    return (((vs ^ ((ps * K1) & M) ^ ((sig * K2) & M)) | 1) & M) ^ AOT_XOR


def load_recipes(spv):
    keys = set()
    for name in ('pipeline_states_base.txt', 'pipeline_states.txt'):
        path = os.path.join(spv, name)
        if not os.path.isfile(path):
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            first = True
            for line in f:
                if first and line.startswith('# ld-pipeline-states'):
                    first = False
                    if line.split()[-1] != '2':
                        break  # outro formato
                    continue
                first = False
                if line.startswith('#') or not line.strip():
                    continue
                try:
                    k = tuple(int(w, 16) for w in line.split())
                except ValueError:
                    continue
                if len(k) >= 13 and len(k) == 13 + 2 * k[8] + k[9] + 4 * k[10]:
                    keys.add(k)
    return keys


def split(k):
    """(par, estado de saida, layout de vertices) da chave de GetGamePipeline"""
    cc = k[8]
    out = (k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[11], k[12]) + tuple(k[13:13 + 2 * cc])
    vert = (k[9], k[10]) + tuple(k[13 + 2 * cc:])
    return k[0] | (k[1] << 32), out, vert


def join(pair, out, vert):
    prim, cmask, dstate, cull, clamp, hasd, cc, dctl, sten = out[:9]
    return (pair & 0xFFFFFFFF, pair >> 32, prim, cmask, dstate, cull, clamp, hasd, cc, vert[0], vert[1], dctl, sten) + out[9:] + vert[2:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--n', type=int, default=2, help='estados por grupo (2 = ~44%% das receitas novas, ~10 previsoes por acerto)')
    args = ap.parse_args()
    spv = os.path.join(args.root, 'assets', 'shaders_spv')
    aot = os.path.join(args.root, 'assets', 'shaders_aot')
    out_path = os.path.join(spv, 'pipeline_states_previstas.txt')
    idx_path, shaders_path = os.path.join(aot, 'aot_pairs.idx'), os.path.join(aot, 'shaders.tsv')
    if not os.path.isfile(idx_path) or not os.path.isfile(shaders_path):
        print('AVISO sem pacote AOT (%s): nada a prever' % aot, flush=True)
        return 0
    recipes = load_recipes(spv)
    pairs = {}
    by_vsps = collections.defaultdict(list)
    with open(idx_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('pair '):
                parts = line.split()
                vs, ps, sig = int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)
                key = pair_key(vs, ps, sig)
                pairs[key] = (vs, ps)
                by_vsps[(vs, ps)].append(key)
    vs_feature, ps_feature = {}, {}
    with open(shaders_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) < 4:
                continue
            if cols[0] == 'vs':
                vs_feature[int(cols[1], 16)] = cols[3]  # entradas (semantica, vgpr, componentes)
            elif cols[0] == 'ps':
                ps_feature[int(cols[1], 16)] = cols[2]  # slots de recursos
    groups = collections.defaultdict(collections.Counter)
    seen = set()
    for k in recipes:
        p, out, vert = split(k)
        if p not in pairs:
            continue  # par traduzido ao vivo: sem os recursos no shaders.tsv
        vs, ps = pairs[p]
        seen.add((vs, ps))
        groups[(vs_feature.get(vs), ps_feature.get(ps))][(out, vert)] += 1
    predicted = set()
    for (vs, ps), keys in by_vsps.items():
        if (vs, ps) in seen:
            continue
        group = groups.get((vs_feature.get(vs), ps_feature.get(ps)))
        if not group:
            continue
        for (out, vert), _count in group.most_common(args.n):
            for p in keys:
                predicted.add(join(p, out, vert))
    predicted -= recipes
    tmp = out_path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# ld-pipeline-states 2\n')
        f.write('# Fase 77: previstas por prever_pipelines.py (N=%d) a partir de %d receitas; so o Preparar (--prewarm-predicted) le\n' % (
            args.n, len(recipes)))
        for k in sorted(predicted):
            f.write(' '.join('%x' % w for w in k) + ' \n')
    os.replace(tmp, out_path)
    print('PREVISTAS %d pipelines para %d variantes de par nunca vistas (pacote: %d pares VS+PS; %d receitas em %d grupos) -> %s' % (
        len(predicted), len({k[0] | (k[1] << 32) for k in predicted}), len(by_vsps), len(recipes), len(groups), out_path), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
