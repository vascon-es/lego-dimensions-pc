#!/usr/bin/env python3
"""Fase 37: compilacao antecipada (AOT) dos shaders do jogo, direto dos .DAT.

Os .DAT trazem ~9.600 arquivos .ps4_shaders (um por modelo / fase / efeito). Cada um e uma lista
de pares (vertex shader, pixel shader) com o binario GCN completo e o cabecalho Gnmx de cada
shader. O que faltava para recompilar sem o jogo rodando era o "retrato" do primeiro draw
(fetch shader + descritores reais). Este script monta esse retrato sem o jogo:

  * fetch shader: gerado a partir das semanticas de entrada do cabecalho do VS, do mesmo jeito que
    o jogo gera (conferido byte a byte contra os capturados);
  * descritores: posicoes vindas dos input usage slots do cabecalho; conteudo de modelos reais;
  * formato dos atributos de vertice (o que o cabecalho nao diz): dicionario de layouts montado a
    partir dos pares ja compilados com o jogo rodando (assets/shaders_spv/pairs);
  * tipo de cada textura (2D ou cubo): lido do proprio codigo do pixel shader (v_cubeid).

Comandos (rodar da raiz do projeto, em ordem):
  extract   le os .DAT (so leitura) e grava assets/shaders_aot/bin (binarios + index.tsv) e
            assets/shaders_aot/shaders.tsv / pairs_dat.tsv
  plan      monta os retratos sinteticos (assets/shaders_aot/work/snap_*.txt) e o dicionario
  compile   roda o shader_compiler_tool em paralelo sobre os retratos e empacota o resultado em
            assets/shaders_aot/aot_pairs.pak + aot_pairs.idx
  validate  confere os retratos sinteticos contra os pares ja compilados com o jogo rodando
"""
import argparse
import collections
import glob
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcn  # noqa: E402
import pssl_reflect  # noqa: E402
import nu2dat  # noqa: E402

ROOT = nu2dat.ROOT
AOT = os.path.join(ROOT, 'assets', 'shaders_aot')
BIN = os.path.join(AOT, 'bin')
WORK = os.path.join(AOT, 'work')
SPV = os.path.join(ROOT, 'assets', 'shaders_spv')
TOOL = os.path.join(ROOT, 'tools', 'shader_compiler_tool', 'build', 'shader_compiler_tool.exe')

# Modelos de descritor tirados dos retratos reais (pair_snapshots.txt). O recompilador le deles o
# tipo da textura e o formato; enderecos e tamanhos nao entram no SPIR-V.
T_2D = [0x10E2C710, 0x02500000, 0x001FC07F, 0x96D70FAC, 0x000FE000, 0, 0, 0]
T_CUBE = [0x10EB8F30, 0x02500000, 0x007FC1FF, 0xB6D90FAC, 0x003FE000, 0x0000A000, 0, 0]
T_3D = [0x10F47C50, 0x00A00000, 0x0007C03F, 0xA4800FAC, 0x0007E00F, 0, 0, 0]  # Fase 78: de um retrato real (tipo 0xA)
# Fase 78: tipo de textura aprendido dos retratos: 0 = 2D, 1 = cubo, 2 = 3D (T# dword 3 >> 28 = 0x9 / 0xB / 0xA)
TEXTURE_TYPE = {0x9: 0, 0xB: 1, 0xA: 2}
# Fase 78: tipos por PAR ("vs:ps" -> {origem: tipo}): o mesmo PS pode ler cubo com um VS e 2D com outro, e a variante do
# pacote e por par. Preenchido por learn_from_snapshots e merge_learned.
PAIR_TYPES = {}


def types_for(vs, ps, ps_types):
    """tipos reais das texturas do par: os do par, senao os unanimes do PS"""
    out = dict(ps_types.get(ps) or {})
    out.update(PAIR_TYPES.get('%s:%s' % (vs, ps), {}))
    return out or None
S_DEFAULT = [0x00007400, 0x00FFF000, 0x0AF00000, 0]
V_CONST = [0xBAB79440, 0x00100010, 0x00000100, 0x20077FAC]
TABLE_DWORDS = 192
FAKE_POINTER = (0xB0000000, 0x10)  # so precisa ser nao nulo: a ferramenta troca pelo da tabela


# ======================= extract =======================
nu2dat_errors = collections.Counter()


EBOOT_PAIRS = os.path.join(ROOT, 'tools', 'data', 'pares_eboot.txt')


def known_eboot_pairs():
    """Fase 78: pares (VS, PS) ja vistos pelo port: a lista que vem com ele (EBOOT_PAIRS) e o que este PC viu."""
    out = set()

    def add(vs, ps):
        if re.fullmatch(r'[0-9a-f]{16}', vs) and re.fullmatch(r'[0-9a-f]{16}', ps):
            out.add((vs, ps))
    for path, first_col in ((EBOOT_PAIRS, 0), (os.path.join(SPV, 'pair_warm.tsv'), 0), (os.path.join(SPV, 'pair_snapshots.txt'), 1)):
        if not os.path.isfile(path):
            continue
        with open(path, encoding='ascii', errors='replace') as f:
            for line in f:
                p = line.split()
                if first_col == 1 and (not p or p[0] != 'pair'):
                    continue
                if len(p) >= first_col + 2:
                    add(p[first_col], p[first_col + 1])
    for path in glob.glob(os.path.join(SPV, 'pairs', 'pair_*.txt')):
        m = re.match(r'pair_([0-9a-f]{16})_([0-9a-f]{16})_', os.path.basename(path))
        if m:
            add(m.group(1), m.group(2))
    return out


def cmd_export_eboot_pairs(args):
    """Fase 78: grava em EBOOT_PAIRS os pares vistos que usam shader do eboot (so hashes; vai com o port)."""
    elf_path = os.path.join(ROOT, 'assets', 'decrypted_elfs', 'eboot.bin.elf')
    with open(elf_path, 'rb') as f:
        eboot = {'%016x' % gcn.fnv_bytes(blob) for _s, _h, blob in gcn.shaders_in(f.read())}
    pairs = sorted(p for p in known_eboot_pairs() if p[0] in eboot or p[1] in eboot)
    with open(EBOOT_PAIRS, 'w', newline='\n') as f:
        f.write('# Fase 78: pares (VS PS) com shader do eboot.bin ja vistos pelo port (so hashes; o extract compila)\n')
        f.writelines('%s %s\n' % p for p in pairs)
    print('EBOOT PAIRS: %d -> %s' % (len(pairs), EBOOT_PAIRS))


def camera_source(info, slot_api):
    """1.9.1: de onde vem o V# do buffer de camera (slot de API `slot_api`) do VS, no formato das fontes dos pares:
    'ud:R' (registrador de user data) ou 'tab:R:D' (tabela / registradores estendidos); None se o cabecalho nao diz."""
    eud = next((s[2] for s in info['slots'] if s[0] == gcn.USAGE_EXTENDED_USER_DATA), None)
    for kind, api, reg, _flags in info['slots']:
        if kind == gcn.USAGE_IMM_CONST_BUFFER and api == slot_api:
            if reg < 16:
                return 'ud:%d' % reg
            return 'tab:%d:%d' % (eud, reg - 16) if eud is not None else None
    for kind, _api, reg, _flags in info['slots']:
        if kind == gcn.USAGE_CONST_BUFFER_TABLE:
            return 'tab:%d:%d' % (reg, 4 * slot_api)
    return None


def write_cameras(seen, cameras):
    """1.9.1: assets/shaders_aot/cameras.tsv = VS com o g_CameraVS_CB (matriz de camera) e onde o port acha o buffer."""
    lines = []
    for h, (slot, offset) in sorted(cameras.items()):
        stage, info = seen.get(h, (None, None))
        # a reflexao numera os buffers a partir de 1 (g_CameraVS_CB = 1); o slot de API do cabecalho comeca em 0
        src = camera_source(info, slot - 1) if stage == gcn.STAGE_VS else None
        if src:
            lines.append('%s\t%s\t%d\n' % (h, src, offset))
    with open(os.path.join(AOT, 'cameras.tsv'), 'w', newline='\n') as f:
        f.write('# 1.9.1: vs fonte_do_buffer_de_camera deslocamento_de_vs_viewProj\n')
        f.writelines(lines)
    print('CAMERAS: %d VS with the camera buffer' % len(lines))


def cmd_cameras(args):
    """1.9.1: so a tabela de cameras (sem refazer o pacote AOT): le os .ps4_shaders e o eboot de novo."""
    seen, cameras = {}, {}

    def scan(data):
        found = pssl_reflect.vs_cameras(data, gcn.fnv_bytes, gcn.STAGE_VS)
        cameras.update(found)
        for stage, hdr, blob in gcn.shaders_in(data):
            h = '%016x' % gcn.fnv_bytes(blob)
            if stage == gcn.STAGE_VS and h in found and h not in seen:
                seen[h] = (stage, gcn.parse_header(stage, hdr))

    for _p, _n, data in nu2dat.iter_files(args.app0, addcont=args.addcont, errors=nu2dat_errors, patch=args.patch or None):
        scan(data)
    eboot_path = os.path.join(ROOT, 'assets', 'decrypted_elfs', 'eboot.bin.elf')
    if os.path.isfile(eboot_path):
        with open(eboot_path, 'rb') as f:
            scan(f.read())
    write_cameras(seen, cameras)


def cmd_extract(args):
    os.makedirs(BIN, exist_ok=True)
    seen = {}       # hash -> (estagio, info)
    pairs = collections.Counter()
    first_file = {}
    files = 0
    t0 = time.time()
    index_lines = []
    packs = collections.Counter()
    cameras = {}  # 1.9.1: hash do VS -> (slot de API do g_CameraVS_CB, deslocamento de vs_viewProj)

    def add_shader(stage, hdr, blob):
        h = '%016x' % gcn.fnv_bytes(blob)
        if h in seen:
            return h
        info = gcn.parse_header(stage, hdr)
        if stage == gcn.STAGE_PS:
            info['cubes'] = sorted(gcn.cube_sources(gcn.words(blob)))
        seen[h] = (stage, info)
        prefix = 'vs' if stage == gcn.STAGE_VS else 'ps'
        path = os.path.join(BIN, '%s_%s.bin' % (prefix, h))
        if not os.path.exists(path):
            with open(path, 'wb') as f:
                f.write(blob)
        code_bytes = struct.unpack_from('<I', blob, len(blob) - 28 + 8)[0] >> 8  # ShaderBinaryInfo.length
        regs = ' '.join('%08x' % r for r in info['regs'])
        index_lines.append('%s\t%s\t%d\t%d\t-\t-\t0x0\t%s\n' % (prefix, h, len(blob), code_bytes, regs))
        return h

    for pack, name, data in nu2dat.iter_files(args.app0, addcont=args.addcont, errors=nu2dat_errors, patch=args.patch or None):
        packs['DLC' if pack.startswith('DLC') and not pack.startswith('DLCSTREAM') else 'jogo base'] += 1
        files += 1
        found = gcn.shaders_in(data)
        cameras.update(pssl_reflect.vs_cameras(data, gcn.fnv_bytes, gcn.STAGE_VS))
        hashes = []
        for stage, hdr, blob in found:
            hashes.append((stage, add_shader(stage, hdr, blob)))
        for i in range(0, len(hashes) - 1, 2):
            (s0, h0), (s1, h1) = hashes[i], hashes[i + 1]
            if s0 == gcn.STAGE_VS and s1 == gcn.STAGE_PS:
                pairs[(h0, h1)] += 1
                first_file.setdefault((h0, h1), name)
        if files % 500 == 0:
            print('  %d files, %d shaders, %d pairs (%.0f s)' % (files, len(seen), len(pairs), time.time() - t0), flush=True)
    # Fase 78: os shaders do proprio executavel (interface, pos-processamento, efeitos de tela): ~420, com o mesmo
    # cabecalho dos .DAT, mas sem a ordem VS/PS dos arquivos. Os pares vem de EBOOT_PAIRS (os que o port ja viu, so
    # hashes) e do que este PC ja viu (pair_warm.tsv, pares compilados, retratos). Sem eles, todo desenho com shader
    # do eboot caia na traducao ao vivo e no pipeline montado na hora.
    eboot_path = os.path.join(ROOT, 'assets', 'decrypted_elfs', 'eboot.bin.elf')
    if os.path.isfile(eboot_path):
        with open(eboot_path, 'rb') as f:
            elf = f.read()
        eboot = {add_shader(stage, hdr, blob) for stage, hdr, blob in gcn.shaders_in(elf)}
        cameras.update(pssl_reflect.vs_cameras(elf, gcn.fnv_bytes, gcn.STAGE_VS))
        known = known_eboot_pairs()
        added = 0
        for vs, ps in sorted(known):
            if (vs in eboot or ps in eboot) and seen.get(vs, (0,))[0] == gcn.STAGE_VS and seen.get(ps, (0,))[0] == gcn.STAGE_PS:
                if (vs, ps) not in pairs:
                    added += 1
                pairs[(vs, ps)] += 1
                first_file.setdefault((vs, ps), 'eboot.bin')
        print('  eboot.bin: %d shaders, %d pairs' % (len(eboot), added), flush=True)
    with open(os.path.join(BIN, 'index.tsv'), 'w', newline='\n') as f:
        f.writelines(index_lines)
    with open(os.path.join(AOT, 'shaders.tsv'), 'w', newline='\n') as f:
        f.write('# estagio hash slots(tipo,slot,registrador,flags) semanticas(sem,vgpr,componentes) cubos\n')
        for h, (stage, info) in seen.items():
            slots = ';'.join('%d,%d,%d,%d' % s for s in info['slots'])
            sem = ';'.join('%d,%d,%d' % s[:3] for s in info['sem'])
            cubes = ';'.join(info.get('cubes', []))
            f.write('%s\t%s\t%s\t%s\t%s\n' % ('vs' if stage == gcn.STAGE_VS else 'ps', h, slots or '-', sem or '-', cubes or '-'))
    with open(os.path.join(AOT, 'pairs_dat.tsv'), 'w', newline='\n') as f:
        f.write('# vs ps arquivos primeiro_arquivo\n')
        for (vs, ps), n in sorted(pairs.items()):
            f.write('%s\t%s\t%d\t%s\n' % (vs, ps, n, first_file[(vs, ps)]))
    vs_count = sum(1 for s, _ in seen.values() if s == gcn.STAGE_VS)
    print('EXTRACTED: %d .ps4_shaders files, %d unique shaders (%d VS, %d PS), %d pairs (VS, PS) in %.0f s'
          % (files, len(seen), vs_count, len(seen) - vs_count, len(pairs), time.time() - t0))
    print('  files by source: ' + ', '.join('%s %d' % kv for kv in sorted(packs.items())))
    if nu2dat_errors:  # Fase 49: indice de outra versao do pacote
        print('WARNING: unreadable files by pack (index from another game version?): ' +
              ', '.join('%s %d' % kv for kv in sorted(nu2dat_errors.items())))


def load_shaders():
    shaders = {}
    with open(os.path.join(AOT, 'shaders.tsv')) as f:
        for line in f:
            if line.startswith('#'):
                continue
            stage, h, slots, sem, cubes = line.rstrip('\n').split('\t')
            shaders[h] = {
                'stage': stage,
                'slots': [] if slots == '-' else [tuple(int(x) for x in s.split(',')) for s in slots.split(';')],
                'sem': [] if sem == '-' else [tuple(int(x) for x in s.split(',')) + (0,) for s in sem.split(';')],
                'cubes': set() if cubes == '-' else set(cubes.split(';')),
            }
    return shaders


def load_dat_pairs():
    out = []
    with open(os.path.join(AOT, 'pairs_dat.tsv')) as f:
        for line in f:
            if not line.startswith('#'):
                p = line.rstrip('\n').split('\t')
                out.append((p[0], p[1], int(p[2])))
    return out


# ======================= dicionario de layouts =======================
PAIR_RE = re.compile(r'pair vs (\w+) ps (\w+) variante (\w+) fetch_sgpr (-?\d+) real (\w+)')


def known_pairs():
    """Pares compilados com o jogo rodando: (vs, ps, variante) -> texto do .txt."""
    out = {}
    if not os.path.isdir(os.path.join(SPV, 'pairs')):
        return out
    for path in glob.glob(os.path.join(SPV, 'pairs', 'pair_*.txt')):
        with open(path) as f:
            text = f.read()
        m = PAIR_RE.match(text)
        if m and m.group(5) in ('sim', 'yes'):
            out[(m.group(1), m.group(2), m.group(3))] = text
    return out


def existing_keys():
    keys = set()
    if not os.path.isdir(os.path.join(SPV, 'pairs')):
        return keys
    for path in os.listdir(os.path.join(SPV, 'pairs')):
        m = re.match(r'pair_(\w{16})_(\w{16})_(\w{16})\.txt', path)
        if m:
            keys.add(m.groups())
    return keys


class LayoutDictionary:
    """Classe numerica (float / uint / sint) de cada atributo de vertice, aprendida dos pares reais.

    O SPIR-V do VS so depende do formato do V# por dois caminhos: o swizzle (dst_sel), que o
    recompilador aplica no shader, e a classe numerica, que decide o tipo da entrada. O pacote AOT
    compila o VS com swizzle identidade e o jogo poe o swizzle real no VkFormat da entrada de vertice
    (BGRA -> B8G8R8A8; componentes que faltam -> 0 / 1 do proprio Vulkan). Sobra a classe."""

    def __init__(self, shaders, pairs, exclude_vs=None):
        self.by_vs = collections.defaultdict(collections.Counter)
        self.by_layout = collections.defaultdict(collections.Counter)
        self.by_attr = collections.defaultdict(collections.Counter)
        for (vs, ps, var), text in pairs.items():
            if vs == exclude_vs or vs not in shaders:
                continue
            sem = shaders[vs]['sem']
            fm = dict((int(a), int(b)) for a, b in re.findall(r'vformat location (\d+) format (\d+)', text))
            if len(fm) != len(sem):
                continue
            classes = tuple(gcn.format_class(fm[i]) for i in range(len(sem)))
            layout = tuple((s[0], s[2]) for s in sem)
            self.by_vs[vs][classes] += 1
            self.by_layout[layout][classes] += 1
            for a, c in zip(layout, classes):
                self.by_attr[a][c] += 1

    # Fase 49: o dicionario e so estatistica (hash do VS / semanticas -> classes, com contagem); vai
    # junto com o port (tools/data/layout_dictionary.json) para quem ainda nao tem pares compilados.
    def to_json(self):
        def dump(table):
            return {(k if isinstance(k, str) else repr(k)): {repr(c): n for c, n in v.items()} for k, v in table.items()}
        return {'by_vs': dump(self.by_vs), 'by_layout': dump(self.by_layout), 'by_attr': dump(self.by_attr)}

    def merge_json(self, data):
        import ast
        for name, table in (('by_vs', self.by_vs), ('by_layout', self.by_layout), ('by_attr', self.by_attr)):
            for k, counts in data.get(name, {}).items():
                key = k if name == 'by_vs' else ast.literal_eval(k)
                for c, n in counts.items():
                    table[key][ast.literal_eval(c)] += n

    def candidates(self, vs, sem, k):
        """Ate k tuplas de classes (uma por atributo), as mais provaveis primeiro."""
        layout = tuple((s[0], s[2]) for s in sem)
        out, source = [], 'atributos'
        if vs in self.by_vs:  # VS ja visto: as classes com que ele ja apareceu
            out = [c for c, _ in self.by_vs[vs].most_common()]
            source = 'vs'
        for c, _ in self.by_layout[layout].most_common(k):
            if c not in out:
                out.append(c)
                source = source if source == 'vs' else 'layout'
        options = []
        for a in layout:
            common = self.by_attr[a].most_common(2) or [(0, 1)]
            total = sum(n for _, n in common)
            options.append([(c, n / total) for c, n in common])

        def weight(combo):
            p = 1.0
            for _, x in combo:
                p *= x
            return p
        for combo in sorted(itertools.product(*options), key=lambda c: -weight(c)):
            if len(out) >= k:
                break
            classes = tuple(c for c, _ in combo)
            if classes not in out:
                out.append(classes)
        return out, source


# ======================= retrato sintetico =======================
def stage_snapshot(info, is_ps, vb_formats=None, real_types=None, vb_layout=None):
    """Registradores de user data + tabelas, a partir dos input usage slots do cabecalho.
    Fase 52: real_types = {origem: True se cubo} aprendido dos retratos reais (vale mais que a previsao pelo
    codigo); vb_layout = [(sgpr da tabela, dword, formato)] de uma leitura de vertice aprendida."""
    ud = [0] * 16
    tables = {}
    eud_reg = next((s[2] for s in info['slots'] if s[0] == gcn.USAGE_EXTENDED_USER_DATA), None)

    def table(reg):
        if reg not in tables:
            tables[reg] = [0] * TABLE_DWORDS
            if reg + 1 < 16:
                ud[reg], ud[reg + 1] = FAKE_POINTER
        return tables[reg]

    def put(reg, values):
        if reg < 16:
            for i, v in enumerate(values):
                if reg + i < 16:
                    ud[reg + i] = v
            return 'ud:%d' % reg
        if eud_reg is None:
            return None
        t = table(eud_reg)
        d = reg - 16
        for i, v in enumerate(values):
            if d + i < TABLE_DWORDS:
                t[d + i] = v
        return 'tab:%d:%d' % (eud_reg, d)

    def image(source):
        if real_types and source in real_types:
            kind = int(real_types[source])
            return T_3D if kind == 2 else T_CUBE if kind == 1 else T_2D
        return T_CUBE if source in info['cubes'] else T_2D

    if eud_reg is not None:
        table(eud_reg)
    for kind, slot, reg, flags in info['slots']:
        if kind == gcn.USAGE_IMM_RESOURCE:
            source = 'ud:%d' % reg if reg < 16 else ('tab:%d:%d' % (eud_reg, reg - 16) if eud_reg is not None else None)
            put(reg, image(source))
        elif kind == gcn.USAGE_IMM_SAMPLER:
            put(reg, S_DEFAULT)
        elif kind == gcn.USAGE_IMM_CONST_BUFFER:
            put(reg, V_CONST)
        elif kind == gcn.USAGE_RESOURCE_TABLE and reg + 1 < 16:
            t = table(reg)
            for a in range(16):  # tabela inteira com T#: o shader so le os slots que usa
                source = 'tab:%d:%d' % (reg, 8 * a)
                t[8 * a:8 * a + 8] = image(source)
        elif kind == gcn.USAGE_SAMPLER_TABLE and reg + 1 < 16:
            t = table(reg)
            for a in range(16):
                t[4 * a:4 * a + 4] = S_DEFAULT
        elif kind == gcn.USAGE_CONST_BUFFER_TABLE and reg + 1 < 16:
            t = table(reg)
            for a in range(16):
                t[4 * a:4 * a + 4] = V_CONST
        elif kind == gcn.USAGE_VERTEX_BUFFER_TABLE and reg + 1 < 16 and vb_layout is not None:
            t = table(reg)
            for k, (base, off, fmt) in enumerate(vb_layout):
                if base == reg and off + 4 <= TABLE_DWORDS:
                    t[off:off + 4] = [0xB9EB7000 + 16 * k, 0x00280010, 0x00000100, 0x20000000 | fmt]
        elif kind == gcn.USAGE_VERTEX_BUFFER_TABLE and reg + 1 < 16 and vb_formats is not None:
            t = table(reg)
            for (sem, vgpr, size, _), fmt in zip(info['sem'], vb_formats):
                t[4 * sem:4 * sem + 4] = [0xB9EB7000 + 16 * sem, 0x00280010, 0x00000100, 0x20000000 | fmt]
    return ud, tables


def snapshot_text(vs_info, ps_info, formats, ps_types=None, fetch=None, vb_layout=None):
    fetch = fetch or gcn.fetch_shader(vs_info)
    sgpr = next((s[2] for s in vs_info['slots'] if s[0] == gcn.USAGE_FETCH_SHADER), -1)
    lines = []
    if fetch and sgpr >= 0:
        lines.append('fetch %d %s' % (sgpr, ' '.join('%08x' % w for w in fetch)))
    for name, info, fm in (('vs', vs_info, formats), ('ps', ps_info, None)):
        ud, tables = stage_snapshot(info, name == 'ps', fm, ps_types if name == 'ps' else None, vb_layout if name == 'vs' else None)
        lines.append('%s ud %s' % (name, ' '.join('%08x' % v for v in ud)))
        for reg in sorted(tables):
            lines.append('%s tab %d %s' % (name, reg, ' '.join('%08x' % v for v in tables[reg])))
    return '\n'.join(lines) + '\n'


def variants_for_pair(dictionary, shaders, vs, ps, k):
    """(assinatura por classe, formatos-modelo com swizzle identidade) de cada variante a compilar."""
    vs_info = shaders[vs]
    fetch = gcn.fetch_shader(vs_info)
    if fetch is None:
        return [], 'sem fetch'
    cands, source = dictionary.candidates(vs, vs_info['sem'], k)
    out = []
    for classes in cands:
        sig = '%016x' % gcn.class_signature(fetch, classes)
        out.append((sig, tuple(gcn.CLASS_TEMPLATE[c] for c in classes)))
    return out, source


DICTIONARY_JSON = os.path.join(ROOT, 'tools', 'data', 'layout_dictionary.json')


def learn_from_snapshots(shaders):
    """Fase 52: o que os retratos reais do jogo (pair_snapshots.txt) ensinam, so como numeros:
    fetch_alt  {hash do fetch gerado pelo cabecalho: {fetch real: {classes}}} - leituras de vertice que vem da
               malha (o jogo gera o fetch pela declaracao de vertices, nao pelo shader); servem para todo VS
               com o mesmo fetch gerado;
    ps_types   {PS: {origem do T#: tipo}} - tipo real de cada textura (a previsao pelo codigo erra); Fase 78: so as
               origens em que todos os retratos concordam (0 2D, 1 cubo, 2 3D); o tipo por par vai para PAIR_TYPES."""
    fetch_alt = collections.defaultdict(lambda: collections.defaultdict(set))
    votes = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    pair_votes = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    path = os.path.join(SPV, 'pair_snapshots.txt')
    if not os.path.exists(path):
        return fetch_alt, {}
    blocks, cur = [], None
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('pair '):
            p = line.split()
            cur = (p[1], p[2], [])
            blocks.append(cur)
        elif cur:
            cur[2].append(line)
    for vs, ps, lines in blocks:
        f = next((l for l in lines if l.startswith('fetch ')), None)
        if f and vs in shaders:
            real = tuple(int(x, 16) for x in f.split()[2:])
            gen = gcn.fetch_shader(shaders[vs])
            if gen is not None and tuple(gen) != real:
                tables = {int(l.split()[2]): [int(x, 16) for x in l.split()[3:]] for l in lines if l.startswith('vs tab ')}
                classes = []
                for base, off, _ in gcn.fetch_layout(list(real)):
                    t = tables.get(base)
                    if t is None or off + 3 >= len(t):
                        classes = None
                        break
                    num = (t[off + 3] >> 12) & 7
                    classes.append(1 if num == 4 else 2 if num == 5 else 0)
                if classes:
                    fetch_alt['%016x' % gcn.fnv_words(gen)][real].add(tuple(classes))
        if ps in shaders:
            info = shaders[ps]
            ud = next((l for l in lines if l.startswith('ps ud ')), None)
            ud = [int(x, 16) for x in ud.split()[2:]] if ud else []
            tables = {int(l.split()[2]): [int(x, 16) for x in l.split()[3:]] for l in lines if l.startswith('ps tab ')}
            eud = next((sl[2] for sl in info['slots'] if sl[0] == gcn.USAGE_EXTENDED_USER_DATA), None)
            sources = []
            for kind, slot, reg, flags in info['slots']:
                if kind == gcn.USAGE_IMM_RESOURCE:
                    if reg < 16:
                        sources.append(('ud:%d' % reg, ud[reg:reg + 8]))
                    elif eud is not None and eud in tables:
                        sources.append(('tab:%d:%d' % (eud, reg - 16), tables[eud][reg - 16:reg - 8]))
                elif kind == gcn.USAGE_RESOURCE_TABLE and reg in tables:
                    for a in range(16):
                        sources.append(('tab:%d:%d' % (reg, 8 * a), tables[reg][8 * a:8 * a + 8]))
            for source, words in sources:
                if len(words) >= 4 and (words[3] >> 28) in TEXTURE_TYPE:
                    kind = TEXTURE_TYPE[words[3] >> 28]
                    votes[ps][source][kind] += 1
                    pair_votes['%s:%s' % (vs, ps)][source][kind] += 1
    ps_types = {ps: {src: next(iter(c)) for src, c in per.items() if len(c) == 1} for ps, per in votes.items()}
    ps_types = {ps: per for ps, per in ps_types.items() if per}
    for key, per in pair_votes.items():
        PAIR_TYPES[key] = {src: c.most_common(1)[0][0] for src, c in per.items()}
    return fetch_alt, ps_types


def merge_learned(fetch_alt, ps_types, data):
    for gen, alts in data.get('fetch_alt', {}).items():
        for words, classes in alts:
            fetch_alt[gen][tuple(int(x, 16) for x in words)].add(tuple(classes))
    for ps, per in data.get('ps_types', {}).items():
        ps_types.setdefault(ps, {}).update({k: int(v) for k, v in per.items() if k not in ps_types.get(ps, {})})
    for key, per in data.get('pair_types', {}).items():  # Fase 78
        PAIR_TYPES.setdefault(key, {}).update({k: int(v) for k, v in per.items() if k not in PAIR_TYPES.get(key, {})})


def cmd_export_dictionary(args):
    import json
    shaders = load_shaders()
    dictionary = LayoutDictionary(shaders, known_pairs())
    data = dictionary.to_json()
    fetch_alt, ps_types = learn_from_snapshots(shaders)  # Fase 52
    data['fetch_alt'] = {gen: [[['%08x' % w for w in real], list(c)] for real, cs in alts.items() for c in cs] for gen, alts in fetch_alt.items()}
    data['ps_types'] = {ps: {k: int(v) for k, v in per.items()} for ps, per in ps_types.items()}
    data['pair_types'] = {key: dict(per) for key, per in PAIR_TYPES.items()}  # Fase 78
    print('  learned vertex fetches: %d fetch shaders generated with fallback | actual texture types from %d PS' % (len(fetch_alt), len(ps_types)))
    with open(DICTIONARY_JSON, 'w', newline='\n') as f:
        json.dump(data, f, separators=(',', ':'))
    print('DICIONARIO: %d VS, %d layouts, %d atributos -> %s' % (len(dictionary.by_vs), len(dictionary.by_layout), len(dictionary.by_attr),
                                                                DICTIONARY_JSON))


REAIS_TXT = os.path.join(ROOT, 'tools', 'data', 'vs_reais.txt')


def _vs_real_layouts(path):
    """Fase 51: leituras de vertice reais (fetch + user data + tabelas do VS) de vs_reais.txt."""
    out = collections.defaultdict(list)
    cur = None
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('real '):
            cur = []
            out[line.split()[1]].append(cur)
        elif cur is not None and line:
            cur.append(line)
    return out


_COMPONENTS = (0, 1, 1, 2, 1, 2, 3, 3, 4, 4, 4, 2, 4, 3, 4, 0)


def _swizzle_fits(w3):
    """AotSwizzleFits do port (hle_gnm.cpp): identidade nos componentes que o formato tem, 0 / 1 (w) nos outros."""
    data, sel = (w3 >> 15) & 0xF, w3 & 0xFFF
    if data == 0xA and ((w3 >> 12) & 7) == 0 and sel == 0xF2E:
        return True
    for i in range(4):
        s = (sel >> (3 * i)) & 7
        if s != 4 + i and not (i >= _COMPONENTS[data] and s == (1 if i == 3 else 0)):
            return False
    return True


def _real_variant(lines):
    """(assinatura canonica, linhas do VS normalizadas) ou None se o swizzle nao cabe no pacote."""
    fetch_line = next((l for l in lines if l.startswith('fetch ')), None)
    if not fetch_line:
        return None
    fetch = [int(x, 16) for x in fetch_line.split()[2:]]
    ud = [int(x, 16) for x in next(l for l in lines if l.startswith('vs ud ')).split()[2:]]
    tables = {}
    for l in lines:
        if l.startswith('vs tab '):
            p = l.split()
            tables[int(p[2])] = [int(x, 16) for x in p[3:]]
    classes = []
    for base, off, _ in gcn.fetch_layout(fetch):
        t = tables.get(base)
        if t is None or off + 3 >= len(t):
            return None
        w3 = t[off + 3]
        sel, num, fmt = w3 & 0xFFF, (w3 >> 12) & 7, (w3 >> 15) & 0xF
        if sel == 0xF2E and fmt == 0xA and num == 0:  # BGRA: o jogo poe no VkFormat (kUnorm8x4Bgra)
            t[off + 3] = (w3 & ~0xFFF) | 0xFAC
        elif not _swizzle_fits(w3):
            return None  # o draw recusaria o par do pacote (AotSwizzleFits)
        elif sel != 0xFAC:  # Fase 78: XYZ1 e parecidos (componentes que o formato nao tem = 0/1) cabem, como no port
            t[off + 3] = (w3 & ~0xFFF) | 0xFAC
        classes.append(1 if num == 4 else 2 if num == 5 else 0)
    sig = '%016x' % gcn.class_signature(fetch, classes)
    text = [fetch_line, 'vs ud ' + ' '.join('%08x' % v for v in ud)]
    text += ['vs tab %d %s' % (r, ' '.join('%08x' % v for v in tables[r])) for r in sorted(tables)]
    return sig, '\n'.join(text) + '\n'


def cmd_export_reais(args):
    """Fase 51: das leituras reais registradas pelo jogo (pair_snapshots.txt), guarda as dos VS cujo fetch real
    e diferente do gerado pelo cabecalho (atributos que vem da malha). So numeros: fetch + user data + tabelas."""
    shaders = load_shaders()
    blocks, cur = [], None
    for line in open(os.path.join(SPV, 'pair_snapshots.txt')):
        line = line.rstrip('\n')
        if line.startswith('pair '):
            cur = (line.split()[1], [])
            blocks.append(cur)
        elif cur and (line.startswith('fetch ') or line.startswith('vs ')):
            cur[1].append(line)
    seen = set()
    kept = 0
    with open(REAIS_TXT, 'w', newline='\n') as f:
        for vs, lines in blocks:
            if vs not in shaders:
                continue
            fetch_line = next((l for l in lines if l.startswith('fetch ')), None)
            if not fetch_line:
                continue
            fetch = tuple(int(x, 16) for x in fetch_line.split()[2:])
            gen = gcn.fetch_shader(shaders[vs])
            if gen is not None and tuple(gen) == fetch:
                continue  # o pacote ja gera esta leitura
            variant = _real_variant(list(lines))
            if not variant or (vs, variant[0]) in seen:
                continue
            seen.add((vs, variant[0]))
            f.write('real %s %s\n' % (vs, variant[0]))
            f.writelines(l + '\n' for l in lines)
            kept += 1
    print('ACTUAL FETCHES: %d (VS, fetch) -> %s' % (kept, REAIS_TXT))


def cmd_plan(args):
    shaders = load_shaders()
    pairs = load_dat_pairs()
    known = known_pairs()
    dictionary = LayoutDictionary(shaders, known)
    fetch_alt, ps_types = learn_from_snapshots(shaders)  # Fase 52: dos retratos deste PC
    if os.path.exists(DICTIONARY_JSON):
        import json
        with open(DICTIONARY_JSON) as f:
            data = json.load(f)
        if not known:  # Fase 49: instalacao nova, sem pares compilados
            dictionary.merge_json(data)
            print('  vertex format dictionary: %s' % DICTIONARY_JSON)
        merge_learned(fetch_alt, ps_types, data)  # Fase 52: o que veio com o port
    print('  learned from snapshots: %d fetch shaders generated with fallback, actual texture types from %d PS' % (len(fetch_alt), len(ps_types)))
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    chunk, chunk_pairs, n_chunks = [], 0, 0
    stats = collections.Counter()
    plan_lines = []

    def flush():
        nonlocal chunk, chunk_pairs, n_chunks
        if chunk:
            with open(os.path.join(WORK, 'snap_%04d.txt' % n_chunks), 'w', newline='\n') as f:
                f.write(''.join(chunk))
            n_chunks += 1
            chunk, chunk_pairs = [], 0

    packed = set()
    if args.skip_packed:  # Fase 47: so o que ainda nao esta no pacote (DLCs), para compilar com --append
        with open(os.path.join(AOT, 'aot_pairs.idx')) as f:
            for line in f:
                if line.startswith('pair '):
                    packed.add(tuple(line.split()[1:4]))
    for vs, ps, nfiles in pairs:
        if vs not in shaders or ps not in shaders:
            continue
        variants, source = variants_for_pair(dictionary, shaders, vs, ps, args.variants)
        stats['pares'] += 1
        stats['origem dos formatos: ' + source] += 1
        # Fase 52: leituras de vertice aprendidas de outros VS com o mesmo fetch gerado (atributos da malha)
        gen = gcn.fetch_shader(shaders[vs])
        extra = []
        # Fase 78: VS com mais de 8 atributos: o jogo le todos num lote so; as mesmas classes das variantes do dicionario
        for one in gcn.fetch_shaders_extra(shaders[vs]):
            layout = gcn.fetch_layout(list(one))
            inverse = {v: k for k, v in gcn.CLASS_TEMPLATE.items()}
            for _sig, formats in variants:
                classes = [inverse.get(f) for f in formats]
                if None in classes or len(classes) != len(layout):
                    continue
                vb = [(b, o, gcn.CLASS_TEMPLATE[c]) for (b, o, _), c in zip(layout, classes)]
                extra.append(('%016x' % gcn.class_signature(list(one), classes), None, list(one), vb))
        if extra:
            stats['pares com VS de mais de 8 atributos (lote unico)'] += 1
        for real, class_set in (fetch_alt.get('%016x' % gcn.fnv_words(gen), {}).items() if gen is not None else ()):
            layout = gcn.fetch_layout(list(real))
            for classes in sorted(class_set)[:3]:
                if len(classes) == len(layout):
                    vb = [(b, o, gcn.CLASS_TEMPLATE[c]) for (b, o, _), c in zip(layout, classes)]
                    extra.append(('%016x' % gcn.class_signature(list(real), classes), None, list(real), vb))
        if extra:
            stats['pares com leitura de vertice aprendida'] += 1
        planned = set()
        for sig, formats, fetch_words, vb in [(sg, fm, None, None) for sg, fm in variants] + extra:
            if sig in planned:  # Fase 78: o lote unico pode repetir uma leitura aprendida
                continue
            planned.add(sig)
            if (vs, ps, sig) in packed:
                stats['variantes ja no pacote'] += 1
                continue
            stats['variantes a compilar'] += 1
            chunk.append('pair %s %s %s\n' % (vs, ps, sig))
            chunk.append(snapshot_text(shaders[vs], shaders[ps], formats, types_for(vs, ps, ps_types), fetch_words, vb))
            plan_lines.append('%s\t%s\t%s\t%s\t%s\n' % (vs, ps, sig, source if formats else 'leitura aprendida',
                                                         ' '.join('%x' % f for f in (formats or [v[2] for v in vb]))))
            chunk_pairs += 1
            if chunk_pairs >= args.chunk:
                flush()
    flush()
    with open(os.path.join(AOT, 'plan.tsv'), 'w', newline='\n') as f:
        f.write('# vs ps variante origem_dos_formatos formatos\n')
        f.writelines(plan_lines)
    with open(os.path.join(AOT, 'layout_dictionary.tsv'), 'w', newline='\n') as f:
        f.write('# semanticas (sem:componentes) -> formatos vistos (contagem)\n')
        for layout, counter in sorted(dictionary.by_layout.items(), key=lambda kv: -sum(kv[1].values())):
            f.write('%s\t%s\n' % (' '.join('%d:%d' % a for a in layout),
                                  ' | '.join('%s (%d)' % (' '.join('%x' % x for x in fm), n) for fm, n in counter.most_common())))
    # Fase 51: variantes com a leitura de vertices REAL do VS (atributos da malha), para todos os PS parceiros
    reais_path = args.reais or (REAIS_TXT if os.path.exists(REAIS_TXT) else '')
    if reais_path and os.path.exists(reais_path):
        partners = collections.defaultdict(set)
        for vs, ps, _ in pairs:
            partners[vs].add(ps)
        done = set(tuple(l.split('\t')[:3]) for l in plan_lines)
        for vs, layouts in _vs_real_layouts(reais_path).items():
            for lines in layouts:
                variant = _real_variant(list(lines))
                if not variant:
                    continue
                sig, vs_text = variant
                for ps in sorted(partners.get(vs, ())):
                    if ps not in shaders or (vs, ps, sig) in packed or (vs, ps, sig) in done:
                        continue
                    done.add((vs, ps, sig))
                    ud, tables = stage_snapshot(shaders[ps], True, None, types_for(vs, ps, ps_types))
                    ps_text = 'ps ud %s\n' % ' '.join('%08x' % v for v in ud)
                    ps_text += ''.join('ps tab %d %s\n' % (r, ' '.join('%08x' % v for v in tables[r])) for r in sorted(tables))
                    chunk.append('pair %s %s %s\n' % (vs, ps, sig))
                    chunk.append(vs_text + ps_text)
                    plan_lines.append('%s\t%s\t%s\treal\t-\n' % (vs, ps, sig))
                    stats['variantes com a leitura real do VS'] += 1
                    chunk_pairs += 1
                    if chunk_pairs >= args.chunk:
                        flush()
        flush()
        with open(os.path.join(AOT, 'plan.tsv'), 'w', newline='\n') as f:
            f.write('# vs ps variante origem_dos_formatos formatos\n')
            f.writelines(plan_lines)
    for key, value in sorted(stats.items()):
        print('  %-50s %d' % (key, value))
    print('PLAN: %d batches in %s' % (n_chunks, WORK))


# ======================= compile + pacote =======================
def run_tool(snap, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    r = subprocess.run([TOOL, BIN, out_dir, '--pairs-only', snap], cwd=ROOT, capture_output=True, text=True,
                       creationflags=0x00004000)  # BELOW_NORMAL_PRIORITY_CLASS
    last = [l for l in r.stdout.splitlines() if l.startswith('PARES')]
    failures = [l.strip() for l in r.stdout.splitlines() if l.strip().startswith('par ')]
    return (last[-1] if last else 'sem resumo (codigo %d)' % r.returncode), failures


class PackWriter:
    """aot_pairs.pak: blocos XPRESS deduplicados; aot_pairs.idx: texto
         blob <id> <offset> <empacotado> <tamanho>
         pair <vs> <ps> <variante> <blob do .txt> <blob do .vs.spv> <blob do .ps.spv>"""

    def __init__(self, path, append=False, replace=False):
        import hashlib
        import winpack
        self.hashlib = hashlib
        self.packer = winpack.Packer()
        self.blobs = {}
        self.first_id = 0
        self.offset = 0
        self.skip = set()
        if append:
            with open(path + '.idx') as f:
                for line in f:
                    p = line.split()
                    if p[0] == 'blob':
                        self.first_id = max(self.first_id, int(p[1]) + 1)
                    elif p[0] == 'pair' and not replace:  # Fase 78: --substituir grava de novo (a ultima linha vale)
                        self.skip.add(tuple(p[1:4]))
            self.offset = os.path.getsize(path + '.pak')
            self.pak = open(path + '.pak', 'ab')
            self.idx = open(path + '.idx', 'a', newline='\n')
        else:
            self.pak = open(path + '.pak', 'wb')
            self.idx = open(path + '.idx', 'w', newline='\n')
            self.idx.write('aot_pairs 1\n')
        self.raw_bytes = 0
        self.pairs = 0
        self.lock = threading.Lock()

    def blob(self, data):
        key = self.hashlib.sha1(data).digest()
        if key in self.blobs:
            return self.blobs[key]
        packed = self.packer.pack(data)
        blob_id = self.first_id + len(self.blobs)
        self.pak.write(packed)
        self.idx.write('blob %d %d %d %d\n' % (blob_id, self.offset, len(packed), len(data)))
        self.offset += len(packed)
        self.raw_bytes += len(data)
        self.blobs[key] = blob_id
        return blob_id

    def add_dir(self, pairs_dir):
        added = 0
        with self.lock:
            for path in glob.glob(os.path.join(pairs_dir, 'pair_*.txt')):
                base = path[:-4]
                m = re.match(r'pair_(\w{16})_(\w{16})_(\w{16})$', os.path.basename(base))
                if not m or not os.path.exists(base + '.vs.spv') or not os.path.exists(base + '.ps.spv'):
                    continue
                if m.groups() in self.skip:
                    continue
                with open(path, 'rb') as f:
                    text = f.read()
                if b' real sim' not in text.split(b'\n', 1)[0]:
                    continue
                ids = [self.blob(text)]
                for suffix in ('.vs.spv', '.ps.spv'):
                    with open(base + suffix, 'rb') as f:
                        ids.append(self.blob(f.read()))
                self.idx.write('pair %s %s %s %d %d %d\n' % (m.group(1), m.group(2), m.group(3), *ids))
                self.pairs += 1
                added += 1
            self.pak.flush()
            self.idx.flush()
        return added

    def close(self):
        self.pak.close()
        self.idx.close()


def cmd_compile(args):
    snaps = sorted(glob.glob(os.path.join(WORK, 'snap_*.txt')))
    if args.limit:
        snaps = snaps[:args.limit]
    if not os.path.exists(TOOL):
        sys.exit('ferramenta nao encontrada: ' + TOOL)
    tmp_name = os.path.join(AOT, 'aot_pairs' if args.append else 'aot_pairs.novo')
    writer = PackWriter(tmp_name, args.append, getattr(args, 'substituir', False))
    t0 = time.time()
    done = [0]
    failures = collections.Counter()

    import tempfile
    temp_root = os.path.join(tempfile.gettempdir(), 'ldpc_aot_compile')  # Fase 52: saida temporaria fora do HD

    def job(snap):
        # Fase 52: os 4500 arquivos temporarios de cada lote iam para a pasta de trabalho (no HD): com 10
        # tradutores gravando ao mesmo tempo, um lote de 10 s levava 5 min e alguns travavam.
        out_dir = os.path.join(temp_root, os.path.basename(snap)[:-4] + '_out')
        summary, fails = run_tool(snap, out_dir)
        added = writer.add_dir(os.path.join(out_dir, 'pairs'))
        shutil.rmtree(out_dir, ignore_errors=True)
        with writer.lock:
            done[0] += 1
            for line in fails:
                failures[re.sub(r'par pair_\w+: ', '', line)[:120]] += 1
            print('  batch %d/%d: %d pairs packed | %s | %.0f s' % (done[0], len(snaps), added, summary.split('->')[0].strip(),
                                                                        time.time() - t0), flush=True)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(job, snaps))
    writer.close()
    for ext in () if args.append else ('.pak', '.idx'):
        final = os.path.join(AOT, 'aot_pairs' + ext)
        if os.path.exists(final):
            os.replace(final, final + '.antigo')
        os.replace(tmp_name + ext, final)
    pak_mb = os.path.getsize(os.path.join(AOT, 'aot_pairs.pak')) / 2 ** 20
    print('COMPILED: %d pairs in %.0f s | %d unique blocks, %.0f MB of SPIR-V/text -> %.0f MB in pack'
          % (writer.pairs, time.time() - t0, len(writer.blobs), writer.raw_bytes / 2 ** 20, pak_mb))
    if failures:
        print('Falhas mais comuns:')
        for why, n in failures.most_common(10):
            print('  %5d  %s' % (n, why))


# ======================= validate =======================
def cmd_validate(args):
    """Para os pares ja compilados com o jogo: monta o retrato sintetico (com os formatos reais
    do par e o tipo de textura previsto) e compara o SPIR-V gerado com o do jogo. Mede tambem o
    acerto do dicionario de formatos para VS nunca vistos (deixando o proprio VS de fora)."""
    shaders = load_shaders()
    known = known_pairs()
    dat_pairs = {(vs, ps) for vs, ps, _ in load_dat_pairs()}
    out = os.path.join(AOT, 'validate')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    chunk = []
    usable = 0
    hits = collections.Counter()
    for (vs, ps, var), text in sorted(known.items()):
        if (vs, ps) not in dat_pairs:
            continue
        sem = shaders[vs]['sem']
        fm = dict((int(a), int(b)) for a, b in re.findall(r'vformat location (\d+) format (\d+)', text))
        if len(fm) != len(sem):
            continue
        formats = tuple(fm[i] for i in range(len(sem)))
        fetch = gcn.fetch_shader(shaders[vs])
        sig = '%016x' % gcn.signature(fetch, formats)
        hits['assinatura igual' if sig == var else 'assinatura diferente'] += 1
        if sig != var:
            continue
        usable += 1
        if args.dictionary:
            d = LayoutDictionary(shaders, known, exclude_vs=vs)
            for k in (1, 2, 4):
                cands, _ = d.candidates('-', sem, k)
                classes = tuple(gcn.format_class(f) for f in formats)
                hits['classe certa entre as %d primeiras (VS tratado como novo)' % k] += classes in cands
        chunk.append('pair %s %s %s\n' % (vs, ps, var))
        chunk.append(snapshot_text(shaders[vs], shaders[ps], formats))
    snap = os.path.join(out, 'snap.txt')
    with open(snap, 'w', newline='\n') as f:
        f.write(''.join(chunk))
    summary, fails = run_tool(snap, out)
    print('  ' + summary)
    same = diff_vs = diff_ps = diff_txt = missing = 0
    examples = []
    for (vs, ps, var) in [tuple(l.split()[1:4]) for l in chunk if l.startswith('pair ')]:
        name = 'pair_%s_%s_%s' % (vs, ps, var)
        a, b = os.path.join(SPV, 'pairs', name), os.path.join(out, 'pairs', name)
        if not os.path.exists(b + '.txt'):
            missing += 1
            continue
        eq = [open(a + s, 'rb').read() == open(b + s, 'rb').read() for s in ('.vs.spv', '.ps.spv')]
        ta = re.sub(r'srt_walker.*', '', open(a + '.txt').read())
        tb = re.sub(r'srt_walker.*', '', open(b + '.txt').read())
        if all(eq) and ta == tb:
            same += 1
        else:
            diff_vs += not eq[0]
            diff_ps += not eq[1]
            diff_txt += ta != tb
            if len(examples) < 6:
                examples.append(name)
    print('VALIDATION: %d pairs with synthetic snapshot | identical to game: %d | VS differs: %d | PS differs: %d | description differs: %d | did not compile: %d' % (usable, same, diff_vs, diff_ps, diff_txt, missing))
    for key, value in sorted(hits.items()):
        print('  %-60s %d' % (key, value))
    for e in examples:
        print('  diferente: ' + e)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('extract')
    p.add_argument('--app0', default=nu2dat.DEFAULT_APP0)
    p.add_argument('--patch', default='', help='separate update folder (default: auto-detect CUSA01176-patch)')
    p.add_argument('--addcont', default=nu2dat.DEFAULT_ADDCONT, help='DLC folder (empty = base game only)')
    p = sub.add_parser('plan')
    p.add_argument('--variants', type=int, default=4, help='candidate class tuples per VS')
    p.add_argument('--chunk', type=int, default=1500)
    p.add_argument('--skip-packed', action='store_true', help='pula as variantes que ja estao em aot_pairs.idx')
    p.add_argument('--reais', default='', help='actual vertex fetches (default: tools/data/vs_reais.txt, if present)')
    p = sub.add_parser('compile')
    p.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    p.add_argument('--limit', type=int, default=0)
    p.add_argument('--append', action='store_true', help='append to existing pack instead of rebuilding')
    p.add_argument('--substituir', action='store_true', help='with --append: also re-pack variants already in the pack (the newest wins)')
    p = sub.add_parser('validate')
    p.add_argument('--dictionary', action='store_true')
    sub.add_parser('export-dictionary')
    sub.add_parser('export-reais')
    sub.add_parser('export-eboot-pairs')
    p = sub.add_parser('cameras')
    p.add_argument('--app0', default=nu2dat.DEFAULT_APP0)
    p.add_argument('--patch', default='')
    p.add_argument('--addcont', default=nu2dat.DEFAULT_ADDCONT)
    args = ap.parse_args()
    {'extract': cmd_extract, 'plan': cmd_plan, 'compile': cmd_compile, 'validate': cmd_validate,
     'export-dictionary': cmd_export_dictionary, 'export-reais': cmd_export_reais,
     'export-eboot-pairs': cmd_export_eboot_pairs, 'cameras': cmd_cameras}[args.cmd](args)


if __name__ == '__main__':
    main()
