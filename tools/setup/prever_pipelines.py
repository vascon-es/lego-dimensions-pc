#!/usr/bin/env python3
"""Fase 78: receitas de pipeline previstas pelos MATERIAIS do jogo, para os pares de shaders que ninguem jogou ainda.

O que engasga na primeira visita a um conteudo novo e o pipeline (shader + estado do desenho), nao o shader: o
Preparar ja traduz todos os shaders e cria todos os pipelines das receitas gravadas. O estado do desenho so aparece
quando o jogo desenha, mas os arquivos do jogo dizem quase tudo dele:

  * cada material de modelo/cenario (.ghg/.gsc) aponta, em slots fixos, para os pares (VS, PS) do .ps4_shaders do
    mesmo modelo; o slot e o passe (cena principal, sombra, profundidade...) e o nome do material diz o tipo
    (TPage_LEGO_Transparent, _Hard, _Glow...). Slot + nome -> estado de saida (alvos, blend, profundidade), aprendido
    das receitas gravadas;
  * o material traz a declaracao de vertices da malha (DXTV). DXTV + VS -> layout de vertices, e DXTV -> classe do
    formato de cada atributo -> a variante exata do par no pacote AOT (assinatura), sem prever as ~3 variantes.

Tambem preve estados novos dos pares que ja tem receita (o mesmo par desenhado com outro material ou outro layout).

Medido escondendo areas inteiras e prevendo pelas outras (pares nunca vistos): top-1 acerta 59% com ~2,9 previsoes
por acerto (a Fase 77, por recursos dos shaders, acertava 44% com ~10); top-2 79% com ~5,9. Escondendo Vorton +
Gotham inteiros (1129 receitas, 326 de pares traduzidos ao vivo, que nao tem como prever): top-1 preve 225 com
~42 mil pipelines no jogo todo (cache do driver da AMD +~0,35 GB); top-2 preve 404 com ~137 mil. A Fase 77 gerava
184 mil (+1,8 GB de RAM).

O lego_dimensions.exe --prewarm-only --prewarm-predicted cria cada pipeline previsto uma vez (vai para o cache do
driver) e o destroi; no jogo, o primeiro uso sai do cache do driver. As sessoes normais nao leem o arquivo.

uso: prever_pipelines.py [--root PASTA_DO_PORT] [--n 1] [--app0 ...] [--addcont ...] [--patch ...] [--refazer-materiais]
saida: assets/shaders_spv/pipeline_states_previstas.txt ("# ld-pipeline-states 2", uma chave por linha)
"""
import argparse
import collections
import itertools
import os
import sys
import time

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


def norm_name(name):
    return ''.join(ch for ch in name if not ch.isdigit())


class Classes:
    """DXTV -> classe do formato de cada semantica de vertice, aprendida das receitas (a assinatura do par conhecida
    e resolvida por forca bruta). Vale para qualquer VS que use o mesmo DXTV."""

    def __init__(self, gcn, vs_info):
        self.gcn = gcn
        self.vs = vs_info  # hash -> (info, fetch gerado)
        self.cls = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))

    def fetches(self, vs):
        """fetch shaders possiveis do VS, o mais provavel primeiro (com mais de 8 atributos o jogo le num lote so)"""
        info, fetch = self.vs.get(vs, (None, None))
        if fetch is None:
            return info, []
        return info, self.gcn.fetch_shaders_extra(info) + [fetch]

    def learn(self, vs, sig, dxtv):
        info, fetches = self.fetches(vs)
        if not fetches or len(info['sem']) > 10:
            return
        for classes in itertools.product((0, 1, 2), repeat=len(info['sem'])):
            if any(self.gcn.class_signature(f, classes) == sig for f in fetches):
                for s, c in zip(info['sem'], classes):
                    self.cls[dxtv][s[0]][c] += 1
                return

    def predict(self, vs, dxtv):
        """assinaturas do par para esse DXTV, a mais provavel primeiro (semantica nunca vista = classe 0)"""
        info, fetches = self.fetches(vs)
        table = self.cls.get(dxtv, {})
        classes = [table[s[0]].most_common(1)[0][0] if s[0] in table else 0 for s in info['sem']] if info else []
        return [self.gcn.class_signature(f, classes) for f in fetches]


def load_vs(gcn, shaders_path):
    out = {}
    with open(shaders_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) < 4 or cols[0] != 'vs':
                continue
            info = {
                'slots': [] if cols[2] == '-' else [tuple(int(x) for x in s.split(',')) for s in cols[2].split(';')],
                'sem': [] if cols[3] == '-' else [tuple(int(x) for x in s.split(',')) + (0,) for s in cols[3].split(';')],
            }
            out[int(cols[1], 16)] = (info, gcn.fetch_shader(info))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--n', type=int, default=1, help='estados por material (1 = ~59%% das receitas novas, ~3 previsoes por acerto)')
    ap.add_argument('--app0', default=None)
    ap.add_argument('--addcont', default=None)
    ap.add_argument('--patch', default=None)
    ap.add_argument('--refazer-materiais', action='store_true', help='le os materiais dos .DAT de novo')
    ap.add_argument('--so-novos', action='store_true', help='so pares que nenhuma receita tem (sem estados novos dos ja vistos)')
    args = ap.parse_args()
    sys.path.insert(0, os.path.join(args.root, 'tools', 'nu2_shader_extractor'))
    import gcn  # noqa: E402
    import nu2mat  # noqa: E402
    spv = os.path.join(args.root, 'assets', 'shaders_spv')
    aot = os.path.join(args.root, 'assets', 'shaders_aot')
    out_path = os.path.join(spv, 'pipeline_states_previstas.txt')
    idx_path, shaders_path = os.path.join(aot, 'aot_pairs.idx'), os.path.join(aot, 'shaders.tsv')
    if not os.path.isfile(idx_path) or not os.path.isfile(shaders_path):
        print('AVISO sem pacote AOT (%s): nada a prever' % aot, flush=True)
        return 0
    t0 = time.time()
    mat_path = os.path.join(aot, 'materiais.tsv')
    if args.refazer_materiais or not os.path.isfile(mat_path) or os.path.getmtime(mat_path) < os.path.getmtime(idx_path):
        print('lendo os materiais dos modelos e cenarios do jogo...', flush=True)
        n_models, n_mats = nu2mat.scan(mat_path, args.app0, args.addcont, args.patch)
        print('  %d modelos e cenarios, %d materiais (%.0f s)' % (n_models, n_mats, time.time() - t0), flush=True)
    by_pair = nu2mat.load(mat_path)
    recipes = load_recipes(spv)
    pairs, variants_of = {}, collections.defaultdict(dict)
    with open(idx_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('pair '):
                parts = line.split()
                vs, ps, sig = int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)
                key = pair_key(vs, ps, sig)
                pairs[key] = (vs, ps, sig)
                variants_of[(vs, ps)][sig] = key
    classes = Classes(gcn, load_vs(gcn, shaders_path))
    outs = collections.defaultdict(collections.Counter)       # (slot, nome) -> estados de saida
    outs_slot = collections.defaultdict(collections.Counter)  # slot -> estados de saida
    verts = collections.defaultdict(collections.Counter)      # (vs, dxtv) -> layouts
    verts_d = collections.defaultdict(collections.Counter)    # dxtv -> layouts
    seen = set()
    seen_keys = collections.defaultdict(set)  # (vs, ps) -> variantes (chaves) que as receitas ja usaram
    for k in recipes:
        p, out, vert = split(k)
        if p not in pairs:
            continue  # par traduzido ao vivo: sem material conhecido
        vs, ps, sig = pairs[p]
        seen.add((vs, ps))
        seen_keys[(vs, ps)].add(p)
        mats = by_pair.get((vs, ps), ())
        for slot, name, dxtv in mats:
            outs[(slot, norm_name(name))][out] += 1
            outs_slot[slot][out] += 1
        dxtvs = {m[2] for m in mats}
        for d in dxtvs:
            verts[(vs, d)][vert] += 1
            verts_d[d][vert] += 1
        if len(dxtvs) == 1:
            classes.learn(vs, sig, next(iter(dxtvs)))
    predicted = set()
    n_pairs = n_new = 0
    for (vs, ps), mats in by_pair.items():
        if (vs, ps) not in variants_of or ((vs, ps) in seen and args.so_novos):
            continue
        n_pairs += 1
        n_new += (vs, ps) not in seen
        variants = variants_of[(vs, ps)]
        for slot, name, dxtv in mats:
            cnt = outs.get((slot, norm_name(name))) or outs_slot.get(slot)
            cv = verts.get((vs, dxtv)) or verts_d.get(dxtv)
            if not cnt or not cv:
                continue
            sig = next((g for g in classes.predict(vs, dxtv) if g in variants), None)
            keys = {variants[sig]} if sig is not None else set(variants.values())
            keys |= seen_keys.get((vs, ps), set())  # o mesmo par com outra malha costuma usar a variante ja vista
            for out, _n in cnt.most_common(args.n):
                for vert, _m in cv.most_common(args.n):
                    for p in keys:
                        predicted.add(join(p, out, vert))
    # o mesmo par desenhado com outra malha: os estados que o par ja tem x os layouts de vertice ja vistos com o
    # mesmo VS (em qualquer par). Medido numa instalacao nova simulada: cobriu 164 de 226 pipelines que faltaram na
    # rota de teste, com ~23 mil a mais no jogo todo.
    verts_vs = collections.defaultdict(set)
    states_of = collections.defaultdict(set)
    for k in recipes:
        p, out, vert = split(k)
        if p in pairs:
            verts_vs[pairs[p][0]].add(vert)
            states_of[p].add(out)
    n_layout = len(predicted)
    for p, outs in states_of.items():
        for vert in verts_vs[pairs[p][0]]:
            for out in outs:
                predicted.add(join(p, out, vert))
    # e os 2 layouts mais comuns entre os VS com as mesmas entradas (semantica, VGPR, componentes): telas como o manual de
    # montagem desenham pecas de outros modelos com o mesmo shader. Cobriu 30 de 61 que ainda faltavam, ~+20 mil.
    vs_sem = {vs: tuple(s[:3] for s in info['sem']) for vs, (info, _f) in classes.vs.items()}
    by_sem = collections.defaultdict(collections.Counter)
    for k in recipes:
        p, out, vert = split(k)
        if p in pairs:
            by_sem[vs_sem.get(pairs[p][0])][vert] += 1
    top_sem = {sem: [v for v, _n in c.most_common(2)] for sem, c in by_sem.items()}
    for p, outs in states_of.items():
        for vert in top_sem.get(vs_sem.get(pairs[p][0]), ()):
            for out in outs:
                predicted.add(join(p, out, vert))
    n_layout = len(predicted) - n_layout
    predicted -= recipes
    tmp = out_path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# ld-pipeline-states 2\n')
        f.write('# Fase 78: previstas pelos materiais (prever_pipelines.py, N=%d) a partir de %d receitas; so o Preparar '
                '(--prewarm-predicted) le\n' % (args.n, len(recipes)))
        for k in sorted(predicted):
            f.write(' '.join('%x' % w for w in k) + ' \n')
    os.replace(tmp, out_path)
    print('PREVISTAS %d pipelines (%d pelos materiais de %d pares, %d nunca vistos; %d por outros layouts do mesmo VS; '
          '%d receitas) em %.0f s -> %s' % (len(predicted), len(predicted) - n_layout, n_pairs, n_new, n_layout, len(recipes),
                                           time.time() - t0, out_path), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
