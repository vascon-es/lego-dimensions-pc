#!/usr/bin/env python3
"""Fase 62: icones dos botoes de outras plataformas, a partir do dump do proprio jogador.

O jogo do PS4 so desenha os icones do PS4: \\framework-data\\gui\\buttons\\ps4\\buttonicons_dx11.tsh (uma folha DDS
608x608 em DXT5 + a tabela de recortes, com os nomes cross / circle / square / triangle / l1 ...). Os .DAT trazem
tambem as folhas do PS3, Xbox 360, Xbox One e Wii U, com outros nomes (a / b / x / y / lb ...). Aqui cada uma vira
uma folha NO FORMATO DA DO PS4 (mesmo tamanho, mesmas posicoes e nomes) com os desenhos da outra plataforma, pela
posicao fisica do botao (cruz = botao de baixo = A no Xbox, B no Wii U). O port troca a textura do PS4 por uma
destas na hora de enviar para a GPU, conforme o controle ligado (ou a opcao do launcher).

Saida em assets\\buttons\\: <plataforma>.rgba (608x608 RGBA8, sem cabecalho) para ps4 ps3 360 xboxone wiiu, e
ps4_dxt5.hash (hash FNV-1a 64 dos blocos DXT5 da folha do PS4, para o port reconhecer a textura).
2.0.2: tambem keyboard.rgba, as teclas do port (teclado.py) escritas com a fonte do proprio jogo (\\gui\\font\\font_nxg.ft2).

uso: botoes.py --app0 <pasta do jogo> [--patch <pasta da atualizacao>] [--png]   (--png: copias para conferir)
"""
import argparse
import collections
import os
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'nu2_shader_extractor'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I nao poe a pasta do script no caminho
import nu2dat  # noqa: E402
import teclado  # noqa: E402

OUT = os.path.join(ROOT, 'assets', 'buttons')
PLATFORMS = ('ps4', 'ps3', '360', 'xboxone', 'wiiu')

# icone do PS4 -> icone da outra plataforma no mesmo lugar do controle
FACE_XBOX = {'cross': 'a', 'circle': 'b', 'square': 'x', 'triangle': 'y', 'l1': 'lb', 'l2': 'lt', 'r1': 'rb', 'r2': 'rt'}
FACE_WIIU = {'cross': 'b', 'circle': 'a', 'square': 'y', 'triangle': 'x', 'l1': 'l', 'l2': 'zl', 'r1': 'r', 'r2': 'zr',
             'select': 'minus', 'start': 'plus'}
MAPS = {'ps4': {}, 'ps3': {}, '360': FACE_XBOX, 'xboxone': FACE_XBOX, 'wiiu': FACE_WIIU}


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_tsh(data, names):
    """Folha .tsh (big-endian, chunks CC4): recortes por nome + a imagem DDS."""
    count = struct.unpack_from('>I', data, 0x61)[0]
    sprites = {}
    for i in range(count):
        o = 0x65 + i * 56
        u0, v0, u1, v1 = struct.unpack_from('>4f', data, o)
        _x, _y, ow, oh = struct.unpack_from('>4I', data, o + 16)
        tl, tt, tr, tb = struct.unpack_from('>4I', data, o + 36)
        name_hash = struct.unpack_from('>I', data, o + 52)[0]
        sprites[names.get(name_hash, '%08x' % name_hash)] = (u0, v0, u1, v1, ow, oh, tl, tt, tr, tb)
    dds = data.index(b'DDS ')
    height, width = struct.unpack_from('<2I', data, dds + 12)
    fourcc = data[dds + 84:dds + 88]
    return sprites, width, height, fourcc, data[dds + 128:]


def decode_dxt(blocks, width, height, fourcc):
    """DXT1 / DXT5 -> lista de linhas RGBA (bytearray de largura*4)."""
    out = [bytearray(width * 4) for _ in range(height)]
    bw, bh = (width + 3) // 4, (height + 3) // 4
    size = 8 if fourcc == b'DXT1' else 16
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * size
            alpha = None
            if size == 16:
                a0, a1 = blocks[o], blocks[o + 1]
                bits = int.from_bytes(blocks[o + 2:o + 8], 'little')
                table = [a0, a1] + ([(a0 * (7 - k) + a1 * k) // 7 for k in range(1, 7)] if a0 > a1 else
                                    [(a0 * (5 - k) + a1 * k) // 5 for k in range(1, 5)] + [0, 255])
                alpha = [table[(bits >> (3 * k)) & 7] for k in range(16)]
                o += 8
            c0, c1, idx = struct.unpack_from('<HHI', blocks, o)

            def rgb(c):
                return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
            p0, p1 = rgb(c0), rgb(c1)
            if c0 > c1 or size == 16:
                colors = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                          tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                colors = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for k in range(16):
                x, y = bx * 4 + (k & 3), by * 4 + (k >> 2)
                if x < width and y < height:
                    r, g, b, a = colors[(idx >> (2 * k)) & 3]
                    if alpha is not None:
                        a = alpha[k]
                    row = out[y]
                    row[x * 4:x * 4 + 4] = bytes((r, g, b, a))
    return out


def sample(image, width, height, fx, fy):
    """Bilinear em coordenadas de pixel (centro do pixel = .5), com alfa pre-multiplicado."""
    fx = min(max(fx - 0.5, 0.0), width - 1.0)
    fy = min(max(fy - 0.5, 0.0), height - 1.0)
    x0, y0 = int(fx), int(fy)
    x1, y1 = min(x0 + 1, width - 1), min(y0 + 1, height - 1)
    ax, ay = fx - x0, fy - y0
    acc = [0.0, 0.0, 0.0, 0.0]
    for (x, y, w) in ((x0, y0, (1 - ax) * (1 - ay)), (x1, y0, ax * (1 - ay)), (x0, y1, (1 - ax) * ay), (x1, y1, ax * ay)):
        p = image[y][x * 4:x * 4 + 4]
        a = p[3] / 255.0
        acc[0] += p[0] * a * w
        acc[1] += p[1] * a * w
        acc[2] += p[2] * a * w
        acc[3] += p[3] * w
    a = acc[3] / 255.0
    if a <= 0.0:
        return (0, 0, 0, 0)
    return (min(255, int(acc[0] / a + 0.5)), min(255, int(acc[1] / a + 0.5)), min(255, int(acc[2] / a + 0.5)), min(255, int(acc[3] + 0.5)))


def compose(ps4, source, mapping):
    """Folha no layout do PS4 com os desenhos de `source`."""
    sprites4, w4, h4, _fourcc, _blocks = ps4['sheet']
    image = [bytearray(w4 * 4) for _ in range(h4)]
    src_sprites, sw, sh, _f, _b = source['sheet']
    src_image = source['image']
    for name, (u0, v0, u1, v1, ow, oh, tl, tt, tr, tb) in sprites4.items():
        other = src_sprites.get(mapping.get(name, name))
        if other is None:
            other_image, other_sprite, other_w, other_h = ps4['image'], sprites4[name], w4, h4  # sem equivalente: fica o do PS4
        else:
            other_image, other_sprite, other_w, other_h = src_image, other, sw, sh
        su0, sv0, su1, sv1 = other_sprite[:4]
        # v cresce de baixo para cima (o y inteiro da tabela e a altura menos v1): linha de cima = (1 - v1) * altura
        dx0, dy0 = u0 * w4, (1.0 - v1) * h4
        dx1, dy1 = u1 * w4, (1.0 - v0) * h4
        sx0, sy0 = su0 * other_w, (1.0 - sv1) * other_h
        sx1, sy1 = su1 * other_w, (1.0 - sv0) * other_h
        # o desenho da outra plataforma inteiro dentro do recorte do PS4, centralizado e sem distorcer (as margens de
        # cada plataforma sao diferentes: copiar pelo quadro original cortava setas e bordas)
        scale = min((dx1 - dx0) / (sx1 - sx0), (dy1 - dy0) / (sy1 - sy0))
        ox0 = dx0 + ((dx1 - dx0) - (sx1 - sx0) * scale) / 2
        oy0 = dy0 + ((dy1 - dy0) - (sy1 - sy0) * scale) / 2
        for y in range(int(round(dy0)), int(round(dy1))):
            py = sy0 + (y + 0.5 - oy0) / scale
            if py < sy0 or py >= sy1:
                continue
            for x in range(int(round(dx0)), int(round(dx1))):
                px = sx0 + (x + 0.5 - ox0) / scale
                if px < sx0 or px >= sx1:
                    continue
                image[y][x * 4:x * 4 + 4] = bytes(sample(other_image, other_w, other_h, px, py))
    return image


def write_png(path, image, width, height):
    raw = b''.join(b'\x00' + bytes(row) for row in image)

    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app0', required=True)
    parser.add_argument('--patch', default='')
    parser.add_argument('--png', action='store_true')
    args = parser.parse_args()
    found = {}   # plataforma -> (pacote, dados); o da atualizacao (PATCH*) vence
    names = {}
    errors = collections.Counter()
    font = None  # 2.0.2: fonte do jogo para as teclas (a da atualizacao, de resolucao maior, vence)
    for _pack, name, data in nu2dat.iter_files(args.app0, suffix='font_nxg.ft2', errors=errors, patch=args.patch):
        if name.lower() == '\\gui\\font\\font_nxg.ft2':
            try:
                candidate = teclado.parse_ft2(data, decode_dxt)
            except (ValueError, struct.error):
                continue
            if font is None or candidate['width'] > font['width']:
                font = candidate
    for suffix in ('buttonicons_dx11.tsh', 'buttonicons_dx11_contents.txt'):
        for pack, name, data in nu2dat.iter_files(args.app0, suffix=suffix, errors=errors, patch=args.patch):
            parts = name.lower().split('\\')
            if 'buttons' not in parts or 'additionalcontent' in parts:
                continue
            platform = parts[-2]
            if suffix.endswith('.txt'):
                for line in data.decode('latin-1').splitlines():
                    bits = line.split()
                    if len(bits) == 3:
                        names[int(bits[1], 16)] = bits[2]
            elif platform in PLATFORMS and (platform not in found or pack.upper().startswith('PATCH')):
                found[platform] = (pack, data)
    if 'ps4' not in found:
        print('ERROR PS4 icon sheet not found in .DAT archives', flush=True)
        return 1
    sheets = {}
    for platform, (pack, data) in found.items():
        sheet = parse_tsh(data, names)
        sprites, w, h, fourcc, blocks = sheet
        sheets[platform] = {'sheet': sheet, 'image': decode_dxt(blocks, w, h, fourcc)}
        print('icons %-8s %s: %dx%d %s, %d sprites' % (platform, pack, w, h, fourcc.decode(), len(sprites)), flush=True)
    os.makedirs(OUT, exist_ok=True)
    ps4 = sheets['ps4']
    _s, w4, h4, fourcc4, blocks4 = ps4['sheet']
    level0 = blocks4[:((w4 + 3) // 4) * ((h4 + 3) // 4) * (16 if fourcc4 == b'DXT5' else 8)]
    with open(os.path.join(OUT, 'ps4_dxt5.hash'), 'w') as f:
        f.write('%016x %d %d %d\n' % (fnv1a64(level0), w4, h4, len(level0)))
    for platform in PLATFORMS:
        if platform not in sheets:
            continue
        image = ps4['image'] if platform == 'ps4' else compose(ps4, sheets[platform], MAPS[platform])
        with open(os.path.join(OUT, platform + '.rgba'), 'wb') as f:
            f.write(b''.join(bytes(row) for row in image))
        if args.png:
            write_png(os.path.join(OUT, platform + '.png'), image, w4, h4)
        print('icons %s ready' % platform, flush=True)
    if font is None:
        print('WARNING game font not found: no keyboard icons', flush=True)
    else:
        image = teclado.compose_keyboard(ps4, font)
        with open(os.path.join(OUT, 'keyboard.rgba'), 'wb') as f:
            f.write(b''.join(bytes(row) for row in image))
        if args.png:
            write_png(os.path.join(OUT, 'keyboard.png'), image, w4, h4)
        print('icons keyboard ready', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
