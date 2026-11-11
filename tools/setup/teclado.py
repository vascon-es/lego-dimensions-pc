"""2.0.2: folha de icones de TECLADO no layout da do PS4: o nome da tecla entre parenteses, com a fonte do proprio jogo."""
import math
import struct

# Botao do PS4 -> tecla do port (src/hle/overrides/hle_pad.cpp, ReadKeyboard). Mudou la, muda aqui.
KEYS = {
    'cross': 'Space', 'circle': 'Esc', 'square': 'X', 'triangle': 'Y', 'start': 'Enter', 'select': 'Tab',
    'l1': 'Q', 'r1': 'E', 'l2': '1', 'r2': '3',
    'padup': 'Up', 'paddown': 'Down', 'padleft': 'Left', 'padright': 'Right',
    'lstickup': 'W', 'lstickdown': 'S', 'lstickleft': 'A', 'lstickright': 'D',
    'rstickup': 'I', 'rstickdown': 'K', 'rstickleft': 'J', 'rstickright': 'L',
    'lstick': 'WASD', 'rstick': 'IJKL',
}



def parse_ft2(data, decode_dxt):
    """Fonte .ft2 da TT (big-endian): 'ROTV' com N x (x, y, largura) em float, 'ROTV' com (caractere, glifo) u16, DDS.
    decode_dxt: o decodificador de botoes.py (blocos, largura, altura, fourcc) -> linhas RGBA."""
    r1 = data.find(b'ROTV')
    r2 = data.find(b'ROTV', r1 + 4)
    line_h = struct.unpack_from('>f', data, r1 - 0x14)[0]
    n = struct.unpack_from('>I', data, r1 + 4)[0]
    glyphs = [struct.unpack_from('>3f', data, r1 + 8 + i * 12) for i in range(n)]
    m = struct.unpack_from('>I', data, r2 + 4)[0]
    chars = {}
    for i in range(m):
        c, g = struct.unpack_from('>HH', data, r2 + 8 + i * 4)
        if g < n:
            chars[chr(c)] = glyphs[g]
    dds = data.index(b'DDS ')
    height, width = struct.unpack_from('<2I', data, dds + 12)
    return {'chars': chars, 'line_h': line_h, 'width': width, 'height': height, 'fourcc': data[dds + 84:dds + 88],
            'blocks': data[dds + 128:], 'rows': {}, 'decode': decode_dxt}


def font_pixel_rows(font, y0, y1):
    """Linhas [y0, y1) do atlas, decodificadas so as faixas de blocos pedidas (o atlas inteiro demora)."""
    w, size = font['width'], (16 if font['fourcc'] == b'DXT5' else 8)
    bw = (w + 3) // 4
    for band in range(y0 // 4, (y1 + 3) // 4):
        if band not in font['rows']:
            chunk = font['blocks'][band * bw * size:(band + 1) * bw * size]
            font['rows'][band] = font['decode'](chunk, w, 4, font['fourcc'])
    return lambda y: font['rows'][y // 4][y % 4]


class Canvas:
    """RGBA em float, alfa nao pre-multiplicado; pinta por cima com cobertura (0..1)."""
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def over(self, x, y, color, alpha):
        if alpha <= 0.0 or not (0 <= x < self.w and 0 <= y < self.h):
            return
        p = self.px[y * self.w + x]
        a = alpha + p[3] * (1.0 - alpha)
        if a <= 0.0:
            return
        for k in range(3):
            p[k] = (color[k] * alpha + p[k] * p[3] * (1.0 - alpha)) / a
        p[3] = a


def text_width(font, text, scale):
    gap = -font['line_h'] * 0.06
    total = 0.0
    for i, ch in enumerate(text):
        g = font['chars'].get(ch) or font['chars'].get(ch.upper())
        if g:
            total += g[2] + (gap if i else 0.0)
    return total * scale


def draw_text(canvas, font, text, cx, cy, height):
    """Texto centrado em (cx, cy) com altura de linha `height` (a fonte ja vem branca com contorno preto)."""
    line_h = font['line_h']
    scale = height / line_h
    gap = -line_h * 0.06
    x = cx - text_width(font, text, scale) / 2
    top = cy - height / 2
    for i, ch in enumerate(text):
        g = font['chars'].get(ch) or font['chars'].get(ch.upper())
        if not g:
            continue
        gx, gy, gw = g
        if i:
            x += gap * scale
        row = font_pixel_rows(font, int(gy), min(font['height'], int(gy + line_h) + 2))
        w, h = font['width'], font['height']
        for y in range(int(top), int(math.ceil(top + height))):
            fy = gy + (y + 0.5 - top) / scale
            if fy < gy or fy >= gy + line_h:
                continue
            for xx in range(int(x), int(math.ceil(x + gw * scale))):
                fx = gx + (xx + 0.5 - x) / scale
                if fx < gx or fx >= gx + gw:
                    continue
                # bilinear com alfa pre-multiplicado, so dentro do glifo
                fx0, fy0 = min(max(fx - 0.5, gx), gx + gw - 1), min(max(fy - 0.5, gy), gy + line_h - 1)
                ix, iy = int(fx0), int(fy0)
                ax, ay = fx0 - ix, fy0 - iy
                acc = [0.0, 0.0, 0.0, 0.0]
                for sx, sy, wgt in ((ix, iy, (1 - ax) * (1 - ay)), (ix + 1, iy, ax * (1 - ay)), (ix, iy + 1, (1 - ax) * ay),
                                    (ix + 1, iy + 1, ax * ay)):
                    sx, sy = min(sx, w - 1), min(sy, h - 1)
                    p = row(sy)[sx * 4:sx * 4 + 4]
                    a = p[3] / 255.0 * wgt
                    acc[0] += p[0] * a
                    acc[1] += p[1] * a
                    acc[2] += p[2] * a
                    acc[3] += a
                if acc[3] > 0.0:
                    canvas.over(xx, y, (acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]), min(1.0, acc[3]))
        x += gw * scale


def draw_sprite(name, label, w, h, frame, font):
    """Desenho de um recorte do PS4 (w x h pixels): o nome da tecla entre parenteses, como as dicas dos jogos LEGO de PC
    (ex. "(K)", "(Enter)"), com a fonte do jogo. A altura e a mesma em todos (metade do quadro original do icone, 128 px):
    os recortes tem tamanhos diferentes, mas o jogo desenha todos pelo quadro original; o que nao cabe encolhe."""
    c = Canvas(w, h)
    text = '(%s)' % label
    height = min(frame * 0.5, h * 0.95)
    tw = text_width(font, text, height / font['line_h'])
    if tw > w * 0.98:
        height *= w * 0.98 / tw
    draw_text(c, font, text, w / 2, h / 2, height)
    return c


def compose_keyboard(ps4, font):
    """Folha no layout do PS4: cada recorte com tecla equivalente vira a tecla; o resto (L3 / R3) fica o do PS4."""
    sprites4, w4, h4, _f, _b = ps4['sheet']
    image = [bytearray(row) for row in ps4['image']]
    for name, (u0, v0, u1, v1, _ow, oh, *_rest) in sprites4.items():
        label = KEYS.get(name)
        if not label:
            continue
        x0, y0 = int(round(u0 * w4)), int(round((1.0 - v1) * h4))
        x1, y1 = int(round(u1 * w4)), int(round((1.0 - v0) * h4))
        c = draw_sprite(name, label, x1 - x0, y1 - y0, oh, font)
        for y in range(c.h):
            row = image[y0 + y]
            for x in range(c.w):
                p = c.px[y * c.w + x]
                row[(x0 + x) * 4:(x0 + x) * 4 + 4] = bytes((int(p[0] + 0.5), int(p[1] + 0.5), int(p[2] + 0.5), int(p[3] * 255 + 0.5)))
    return image
