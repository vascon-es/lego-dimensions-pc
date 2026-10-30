#!/usr/bin/env python3
"""A fonte do LEGO Dimensions para o launcher, tirada do dump do proprio jogador.

O launcher escreve os titulos e o menu com a fonte do jogo (\\gui\\font\\font_nxg.ft2: NuFont da TT, uma folha DDS
DXT5 com as letras brancas de contorno preto + a tabela de recortes e o mapa de caracteres). Como os icones dos
botoes (botoes.py), ela nunca vai no pacote do port: sai dos .DAT de quem instalou e fica em assets\\launcher\\.
A da atualizacao (PATCH*) vence, como no jogo.

uso: launcher_art.py --app0 <pasta do jogo> [--patch <pasta da atualizacao>]
"""
import argparse
import collections
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'nu2_shader_extractor'))
import nu2dat  # noqa: E402

OUT = os.path.join(ROOT, 'assets', 'launcher')
FONT = '\\gui\\font\\font_nxg.ft2'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app0', required=True)
    parser.add_argument('--patch', default='')
    args = parser.parse_args()
    found = None
    errors = collections.Counter()
    for pack, name, data in nu2dat.iter_files(args.app0, suffix='font_nxg.ft2', addcont='', errors=errors, patch=args.patch):
        if name.lower() != FONT or data[8:12] != b'TNFN':
            continue
        if found is None or pack.upper().startswith('PATCH'):
            found = (pack, data)
    if found is None:
        print('ERRO font %s not found in the .DAT archives' % FONT, flush=True)
        return 1
    os.makedirs(OUT, exist_ok=True)
    target = os.path.join(OUT, 'font.ft2')
    with open(target + '.novo', 'wb') as f:
        f.write(found[1])
    os.replace(target + '.novo', target)
    print('PRONTO font %s (%s, %d bytes)' % (FONT, found[0], len(found[1])), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
