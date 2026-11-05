"""Fase 71: receitas de pipeline (arquivos de estados) do port.

Cada linha e a chave de um pipeline (GetGamePipeline / JobFromKey, em hex): par (2 palavras), primitiva, mascara de
cor, profundidade, cull, clamp, tem profundidade, numero de alvos / buffers / atributos, estencil (2), e depois
(formato, blend) por alvo, (stride | por instancia) por buffer e (location, formato, offset, buffer) por atributo.
Nao ha nada do jogo nelas: so hashes de shaders e estado de render.

Formato 2: primeira linha "# ld-pipeline-states 2"; linhas com # sao comentarios. Arquivo sem cabecalho = formato 1
(a mesma chave). O port le pipeline_states_base.txt (distribuido, refeito a cada versao) e pipeline_states.txt (do
jogador, so com o que ele viu e a base nao tinha).

uso:
  python tools/pipeline_states.py juntar -o SAIDA ARQUIVO [ARQUIVO ...] [--sem BASE]
      junta os arquivos de varios jogadores numa lista so (sem repetidos pelo mesmo hash do port, sem linhas
      invalidas); --sem tira o que ja esta em BASE (para mandar so as novas)
  python tools/pipeline_states.py conferir ARQUIVO [ARQUIVO ...]
      conta linhas validas, repetidas e invalidas
  python tools/pipeline_states.py areas -o SAIDA ARQUIVO [ARQUIVO ...]
      Fase 72: junta arquivos pipeline_levels ("<hash> <area>"; vale a primeira area de cada hash; as linhas
      "ultima <area>" sao de cada jogador e nao entram)
  python tools/pipeline_states.py converter --retratos pair_snapshots.txt --pacote aot_pairs.idx
                                            [--areas pipeline_levels_base.txt] ARQUIVO [ARQUIVO ...]
      Fase 78: receitas gravadas quando o par ainda era traduzido ao vivo (antes do pacote AOT) tem a chave do par
      ao vivo, que uma instalacao nova nunca pede: acrescenta a chave do pacote (mesmo VS/PS, variante pela classe dos
      formatos de vertice do retrato real) e copia a area; a chave ao vivo fica (quem ja tem o par em disco a usa).
"""
import argparse
import os
import sys

HEADER = '# ld-pipeline-states 2'
VERSION = 2
MASK64 = (1 << 64) - 1
KINDS = 7          # DrawCall::kPointList + 1 (primitivas aceitas)
FORMAT_COUNT = 64  # teto folgado para GameDraw::kFormatCount (o port confere de novo ao ler)


def key_hash(words):
    """HashPipelineKey do port (render/vulkan_backend.cpp)."""
    h = 0xcbf29ce484222325 ^ len(words)
    for w in words:
        h = ((h ^ w) * 0x100000001b3) & MASK64
        h ^= h >> 29
    return h


def parse_line(line):
    """Palavras da chave, ou None (comentario, vazia ou de outro formato)."""
    line = line.strip()
    if not line or line.startswith('#'):
        return None
    try:
        words = [int(t, 16) for t in line.split()]
    except ValueError:
        return None
    if len(words) < 13 or any(w > 0xFFFFFFFF for w in words):
        return None
    colors, bindings, attributes = words[8], words[9], words[10]
    if colors > 8 or bindings > 8 or attributes > 16 or words[2] >= KINDS:
        return None
    if len(words) != 13 + colors * 2 + bindings + attributes * 4:
        return None
    at = 13 + colors * 2 + bindings
    for i in range(attributes):
        if words[at + 1] >= FORMAT_COUNT or words[at + 3] >= 8:
            return None
        at += 4
    return words


def read_file(path):
    """(versao, [palavras]) de um arquivo; versao 0 = formato desconhecido (arquivo ignorado)."""
    version = 1
    keys = []
    with open(path, encoding='ascii', errors='replace') as f:
        for n, line in enumerate(f):
            if n == 0 and line.startswith('# ld-pipeline-states'):
                try:
                    version = int(line.split()[-1])
                except ValueError:
                    version = 0
                continue
            words = parse_line(line)
            if words:
                keys.append(words)
    return (version, keys) if version in (1, VERSION) else (0, [])


def format_key(words):
    return ' '.join('%x' % w for w in words) + ' '


def merge(paths, without=None):
    """Chaves de todos os arquivos, na ordem em que aparecem, sem repetir hash; menos as que estao em `without`."""
    seen = set()
    if without:
        for words in read_file(without)[1]:
            seen.add(key_hash(words))
    out, stats = [], []
    for path in paths:
        version, keys = read_file(path)
        new = 0
        for words in keys:
            h = key_hash(words)
            if h not in seen:
                seen.add(h)
                out.append(words)
                new += 1
        stats.append((path, version, len(keys), new))
    return out, stats


def write_file(path, keys):
    temp = path + '.novo'
    with open(temp, 'w', encoding='ascii', newline='\n') as f:
        f.write(HEADER + '\n')
        for words in keys:
            f.write(format_key(words) + '\n')
    os.replace(temp, path)


def read_levels(path):
    """[(hash, area)] de um arquivo pipeline_levels (sem as linhas "ultima")."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.strip().split(' ', 1)
            if len(parts) != 2 or parts[0].startswith('#') or parts[0] == 'ultima':
                continue
            try:
                h = int(parts[0], 16)
            except ValueError:
                continue
            if h and parts[1] and ' ' not in parts[1]:
                out.append((h, parts[1]))
    return out


def merge_levels(paths):
    seen, out = set(), []
    for path in paths:
        for h, area in read_levels(path):
            if h not in seen:
                seen.add(h)
                out.append((h, area))
    return out


def write_levels(path, entries):
    temp = path + '.novo'
    with open(temp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# ld-pipeline-levels 1\n')
        for h, area in entries:
            f.write('%x %s\n' % (h, area))
    os.replace(temp, path)


PAIR_K1, PAIR_K2, AOT_XOR = 0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F, 0xA070000000000A00


def pair_key(vs, ps, sig, aot):
    """chave do par (hle_gnm LoadGamePair); pares do pacote AOT levam o xor"""
    k = ((vs ^ ((ps * PAIR_K1) & MASK64) ^ ((sig * PAIR_K2) & MASK64)) | 1) & MASK64
    return k ^ AOT_XOR if aot else k


def live_to_aot(snapshots_path, pack_idx_path):
    """{chave do par ao vivo: chave do par no pacote} pelos retratos reais (fetch + tabelas de vertice do VS)."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nu2_shader_extractor'))
    import gcn  # noqa: E402
    packed = set()
    with open(pack_idx_path, encoding='ascii', errors='replace') as f:
        for line in f:
            if line.startswith('pair '):
                p = line.split()
                packed.add(pair_key(int(p[1], 16), int(p[2], 16), int(p[3], 16), True))
    out = {}
    cur = None

    def close(block):
        vs, ps, sig, lines = block
        fetch = next((l for l in lines if l.startswith('fetch ')), None)
        if not fetch:
            return
        real = [int(x, 16) for x in fetch.split()[2:]]
        tables = {int(l.split()[2]): [int(x, 16) for x in l.split()[3:]] for l in lines if l.startswith('vs tab ')}
        classes = []
        for base, off, _n in gcn.fetch_layout(real):
            t = tables.get(base)
            if t is None or off + 3 >= len(t):
                return
            num = (t[off + 3] >> 12) & 7
            classes.append(1 if num == 4 else 2 if num == 5 else 0)
        aot = pair_key(vs, ps, gcn.class_signature(real, classes), True)
        if aot in packed:
            out[pair_key(vs, ps, sig, False)] = aot

    with open(snapshots_path, encoding='ascii', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('pair '):
                if cur:
                    close(cur)
                p = line.split()
                try:
                    cur = (int(p[1], 16), int(p[2], 16), int(p[3], 16), [])
                except (IndexError, ValueError):
                    cur = None
            elif cur is not None:
                cur[3].append(line)
    if cur:
        close(cur)
    return out


def convert(paths, snapshots_path, pack_idx_path, levels_path=None, extra_levels=()):
    mapping = live_to_aot(snapshots_path, pack_idx_path)
    area = {}
    for lp in [levels_path] + list(extra_levels):
        if lp and os.path.isfile(lp):
            for h, a in read_levels(lp):
                area.setdefault(h, a)
    new_levels = []
    for path in paths:
        version, keys = read_file(path)
        if not version:
            print('%s: formato desconhecido, nao convertido' % path)
            continue
        seen, out, converted = set(), [], 0
        for words in keys:
            pair = words[0] | (words[1] << 32)
            versions = [list(words)]
            if pair in mapping:
                # a chave ao vivo FICA: quem ja jogou tem o par traduzido em disco (assets/shaders_spv/pairs), que vale
                # mais que o pacote no jogo e continua pedindo a chave ao vivo; instalacao nova pede a do pacote
                old_hash = key_hash(words)
                aot = mapping[pair]
                versions.append([aot & 0xFFFFFFFF, aot >> 32] + list(words[2:]))
                converted += 1
                if old_hash in area:
                    new_levels.append((key_hash(versions[-1]), area[old_hash]))
            for version in versions:
                h = key_hash(version)
                if h not in seen:
                    seen.add(h)
                    out.append(version)
        write_file(path, out)
        print('%s: %d chaves, %d com a chave do pacote AOT acrescentada, %d no total' % (path, len(keys), converted, len(out)))
    if levels_path and new_levels:
        entries = read_levels(levels_path)
        known = {h for h, _a in entries}
        added = [(h, a) for h, a in new_levels if h not in known]
        write_levels(levels_path, entries + added)
        print('%s: %d areas novas' % (levels_path, len(added)))
    print('pares ao vivo com retrato que estao no pacote: %d' % len(mapping))


def main():
    parser = argparse.ArgumentParser(description='receitas de pipeline do LEGO Dimensions PC')
    sub = parser.add_subparsers(dest='cmd', required=True)
    j = sub.add_parser('juntar')
    j.add_argument('-o', '--saida', required=True)
    j.add_argument('--sem', help='tira as chaves que ja estao neste arquivo')
    j.add_argument('arquivos', nargs='+')
    c = sub.add_parser('conferir')
    c.add_argument('arquivos', nargs='+')
    a = sub.add_parser('areas')
    a.add_argument('-o', '--saida', required=True)
    a.add_argument('arquivos', nargs='+')
    v = sub.add_parser('converter')
    v.add_argument('--retratos', required=True, help='pair_snapshots.txt (retratos reais dos pares, gravados pelo port)')
    v.add_argument('--pacote', required=True, help='aot_pairs.idx')
    v.add_argument('--areas', help='pipeline_levels a completar com a area das chaves convertidas')
    v.add_argument('--areas-ler', nargs='*', default=[], help='outros pipeline_levels so para achar a area (nao mudam)')
    v.add_argument('arquivos', nargs='+')
    args = parser.parse_args()
    if args.cmd == 'converter':
        convert([p for p in args.arquivos if os.path.isfile(p)], args.retratos, args.pacote, args.areas, args.areas_ler)
        return 0
    if args.cmd == 'areas':
        entries = merge_levels([p for p in args.arquivos if os.path.isfile(p)])
        write_levels(args.saida, entries)
        print('%s: %d pipelines com area, %d areas' % (args.saida, len(entries), len({a for _, a in entries})))
        return 0
    if args.cmd == 'juntar':
        keys, stats = merge([a for a in args.arquivos if os.path.isfile(a)], args.sem)
        for path, version, total, new in stats:
            print('%s: formato %s, %d chaves, %d novas' % (path, version or 'desconhecido', total, new))
        write_file(args.saida, keys)
        print('%s: %d chaves' % (args.saida, len(keys)))
    else:
        for path in args.arquivos:
            version, keys = read_file(path)
            hashes = {key_hash(k) for k in keys}
            with open(path, encoding='ascii', errors='replace') as f:
                lines = sum(1 for line in f if line.strip() and not line.startswith('#'))
            print('%s: formato %s, %d linhas, %d chaves validas, %d repetidas, %d invalidas' % (
                path, version or 'desconhecido', lines, len(keys), len(keys) - len(hashes), lines - len(keys)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
