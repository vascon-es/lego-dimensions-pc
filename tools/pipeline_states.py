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


def main():
    parser = argparse.ArgumentParser(description='receitas de pipeline do LEGO Dimensions PC')
    sub = parser.add_subparsers(dest='cmd', required=True)
    j = sub.add_parser('juntar')
    j.add_argument('-o', '--saida', required=True)
    j.add_argument('--sem', help='tira as chaves que ja estao neste arquivo')
    j.add_argument('arquivos', nargs='+')
    c = sub.add_parser('conferir')
    c.add_argument('arquivos', nargs='+')
    args = parser.parse_args()
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
