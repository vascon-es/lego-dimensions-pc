"""Shaders GCN dentro dos .ps4_shaders: cabecalho 'Shdr' (Gnmx), fetch shader e texturas de cubo."""
import re
import struct

M64 = (1 << 64) - 1
STAGE_VS, STAGE_PS = 1, 2

# Tipos de "input usage slot" (Gnm::ShaderInputUsageType) que aparecem nos shaders do jogo
USAGE_IMM_RESOURCE = 0x00       # T# de 8 dwords
USAGE_IMM_SAMPLER = 0x01        # S# de 4 dwords
USAGE_IMM_CONST_BUFFER = 0x02   # V# de 4 dwords
USAGE_FETCH_SHADER = 0x12       # ponteiro do fetch shader
USAGE_RESOURCE_TABLE = 0x13     # ponteiro: T# em 8 * slot
USAGE_SAMPLER_TABLE = 0x15      # ponteiro: S# em 4 * slot
USAGE_CONST_BUFFER_TABLE = 0x16  # ponteiro: V# em 4 * slot
USAGE_VERTEX_BUFFER_TABLE = 0x17  # ponteiro: V# do atributo em 4 * semantica
USAGE_EXTENDED_USER_DATA = 0x1B  # ponteiro: registradores 16+ (dword = registrador - 16)


def fnv_bytes(data):
    """FNV-1a de 64 bits por byte: o nome que o jogo da ao shader extraido (vs_<hash>.bin)."""
    h = 0xCBF29CE484222325
    for x in data:
        h = ((h ^ x) * 0x100000001B3) & M64
    return h


def fnv_words(words):
    """FNV-1a por dword: o hash do fetch shader que o jogo usa na assinatura da variante."""
    h = 0xCBF29CE484222325
    for x in words:
        h = ((h ^ x) * 0x100000001B3) & M64
    return h


def shaders_in(data):
    """(tipo, cabecalho, binario) de cada shader do arquivo, na ordem.

    binario = do inicio do codigo ate o fim do ShaderBinaryInfo ('OrbShdr' + 28 bytes), que e o
    mesmo trecho que o jogo grava em assets/dumped_shaders (e o mesmo hash)."""
    out = []
    for m in re.finditer(b'Shdr\x07\x00', data):
        p = m.start()
        if data[p - 3:p] == b'Orb':
            continue
        code = p + 16 + struct.unpack_from('<I', data, p + 24)[0]
        if code + 8 > len(data) or data[code:code + 4] != b'\xff\x03\xeb\xbe':  # s_mov_b32 vcc_hi, imediato
            continue
        info = code + (struct.unpack_from('<I', data, code + 4)[0] + 1) * 8
        if data[info:info + 7] != b'OrbShdr':
            continue
        out.append((data[p + 8], data[p:code], data[code:info + 28]))
    return out


def parse_header(stage, hdr):
    """Registradores do estagio, input usage slots e semanticas de entrada do VS."""
    common, = struct.unpack_from('<I', hdr, 16)
    nslots = common >> 24
    o = 24
    nreg = 7 if stage == STAGE_VS else 12
    regs = list(struct.unpack_from('<%dI' % nreg, hdr, o))
    o += nreg * 4
    nin = hdr[o]
    o += 4
    slots = [tuple(hdr[o + i * 4:o + i * 4 + 4]) for i in range(nslots)]  # (tipo, slot da API, registrador, flags)
    o += nslots * 4
    sem = [tuple(hdr[o + i * 4:o + i * 4 + 4]) for i in range(nin)] if stage == STAGE_VS else []  # (semantica, vgpr, componentes, -)
    return {'regs': regs, 'slots': slots, 'sem': sem}


def fetch_shader(info, batch_size=8):
    """Fetch shader que o jogo gera para o VS (sem instancia): por atributo, s_load_dwordx4 do V#
    (tabela de vertex buffers, dword 4 * semantica) e buffer_load_format_x..xyzw no VGPR do atributo.
    Em lotes de 8 atributos (s[16:47] / s[4:11] em unidades de 4); ver fetch_shaders para VS com mais de 8."""
    table = next((s[2] for s in info['slots'] if s[0] == USAGE_VERTEX_BUFFER_TABLE), None)
    if table is None or not info['sem']:
        return None
    w = []
    sem = list(info['sem'])
    for start in range(0, len(sem), batch_size):
        batch = sem[start:start + batch_size]
        for i, (s, vgpr, size, _) in enumerate(batch):
            w.append(0xC0800100 | ((16 + 4 * i) << 15) | ((table // 2) << 9) | (4 * s))
        w.append(0xBF8C007F)  # s_waitcnt lgkmcnt(0)
        for i, (s, vgpr, size, _) in enumerate(batch):
            w.append(0xE0002000 | ((size - 1) << 18))
            w.append(0x80000000 | ((4 + i) << 16) | (vgpr << 8))
    w += [0xBF8C0000, 0xBE802000]  # s_waitcnt vmcnt(0); s_setpc_b64 s[0:1]
    return w


def fetch_shaders_extra(info):
    """Fase 78: fetch shaders que o jogo tambem gera alem de fetch_shader(info). Para VS com mais de 8 atributos o
    jogo quase sempre le todos num lote so (s[16:16+4n]): conferido em 472 de 478 retratos reais com 9-10 atributos;
    o lote de 8 de fetch_shader so aparece em 6. Sem este, todo VS assim caia na traducao ao vivo."""
    n = len(info['sem'])
    if n <= 8 or n > 16:
        return []
    one = fetch_shader(info, batch_size=n)
    return [one] if one is not None else []


def fetch_sgpr(info):
    return next((s[2] for s in info['slots'] if s[0] == USAGE_FETCH_SHADER), -1)


def signature(fetch_words, formats):
    """Assinatura da variante do par, como o jogo calcula no draw: hash do fetch shader seguido do
    formato (dword 3 & 0x7FFFF) do V# de cada atributo, na ordem do fetch."""
    s = fnv_words(fetch_words)
    for f in formats:
        s = ((s ^ (f & 0x7FFFF)) * 0x100000001B3) & M64
    return s


# ---------------- texturas de cubo ----------------
V_CUBEID_F32 = 0x144  # VOP3


def _walk(code):
    n = len(code)
    i = 0
    while i < n:
        w = code[i]
        size = 1
        if w >> 31 == 0 or w >> 25 in (0x3F, 0x3E):  # VOP2 / VOP1 / VOPC (+ literal)
            size += (w & 0x1FF) == 0xFF
        elif w >> 23 in (0x17D, 0x17E):  # SOP1 / SOPC
            size += (w & 0xFF) == 0xFF or (w >> 23 == 0x17E and ((w >> 8) & 0xFF) == 0xFF)
        elif w >> 23 == 0x17F or w >> 28 == 0xB or w >> 27 == 0x18 or w >> 26 == 0x32:  # SOPP SOPK SMRD VINTRP
            pass
        elif w >> 30 == 0x2:  # SOP2
            size += (w & 0xFF) == 0xFF or ((w >> 8) & 0xFF) == 0xFF
        elif w >> 26 in (0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E, 0x33):  # VOP3 DS MUBUF MTBUF MIMG EXP
            size = 2
        yield code[i:i + size]
        if w == 0xBF810000:  # s_endpgm
            return
        i += size


# Fase 48: posicao da face (3a coordenada) no endereco de cada amostragem: antes vem o offset (_O),
# o bias (_B) e a referencia de comparacao (_C). Derivadas (_D) vem antes e variam: posicao incerta.
def _face_slot(op):
    base = op & ~0x10 if 0x20 <= op <= 0x3F else (op - 0x20) & ~0x10 if 0x40 <= op <= 0x5F else None
    if base is None:
        return None
    if base in (0x22, 0x23, 0x2A, 0x2B):  # _D
        return -1
    extra = (op & 0x10 != 0)  # _O
    extra += base in (0x25, 0x26, 0x2D, 0x2E)  # _B
    extra += 0x28 <= base <= 0x2F  # _C
    return 2 + extra


def cube_sources(code):
    """Origens dos T# amostrados com a face vinda de v_cubeid (copiada por v_mov no maximo):
    'ud:R' (no proprio user data) ou 'tab:R:D' (dword D da tabela apontada pelo par R, R+1).
    Leitura linear, sem seguir desvios: e uma aproximacao (97% de acerto nos pares conhecidos).
    Fase 48: VOP1 (v_mov) era lido como VOP2 (os dois tem o bit 31 zerado), e a copia da face se
    perdia; a face e conferida so na posicao dela no endereco (antes: 4 registradores, falso cubo)."""
    face = set()
    sgpr_src = {r: ('ud', r) for r in range(16)}
    cubes = set()
    for ins in _walk(code):
        w = ins[0]
        vdst = None
        copy_of = None
        if w >> 25 == 0x3F:  # VOP1 (antes do VOP2: os dois tem o bit 31 zerado)
            vdst = (w >> 17) & 0xFF
            if ((w >> 9) & 0xFF) == 1 and (w & 0x1FF) >= 256:  # v_mov_b32 vN
                copy_of = (w & 0x1FF) - 256
        elif w >> 25 == 0x3E:  # VOPC: escreve VCC
            pass
        elif w >> 31 == 0:  # VOP2
            vdst = (w >> 17) & 0xFF
        elif w >> 26 == 0x34 and len(ins) == 2:  # VOP3
            op = (w >> 17) & 0x1FF
            if op >= 0x80:
                vdst = w & 0xFF
            if op == V_CUBEID_F32:
                face.add(vdst)
                continue
        elif w >> 26 == 0x32:  # VINTRP
            vdst = (w >> 18) & 0xFF
        elif w >> 27 == 0x18:  # SMRD
            op = (w >> 22) & 0x1F
            sdst = (w >> 15) & 0x7F
            base = ((w >> 9) & 0x3F) * 2
            src = sgpr_src.get(base)
            for k in range({0: 1, 1: 2, 2: 4, 3: 8, 4: 16}.get(op, 1)):
                if (w >> 8) & 1 and src is not None and src[0] == 'ud':
                    sgpr_src[sdst + k] = ('tab', src[1], (w & 0xFF) + k)
                else:
                    sgpr_src.pop(sdst + k, None)
            continue
        elif w >> 26 in (0x38, 0x3A) and len(ins) == 2:  # MUBUF / MTBUF: a leitura sobrescreve vdata
            vdata = (ins[1] >> 8) & 0xFF
            for v in range(vdata, vdata + 4):
                face.discard(v)
            continue
        elif w >> 26 == 0x36 and len(ins) == 2:  # DS: a leitura sobrescreve vdst
            vdst_ds = (ins[1] >> 24) & 0xFF
            for v in range(vdst_ds, vdst_ds + 4):
                face.discard(v)
            continue
        elif w >> 26 == 0x3C and len(ins) == 2:  # MIMG
            d1 = ins[1]
            vaddr, vdata, srsrc = d1 & 0xFF, (d1 >> 8) & 0xFF, ((d1 >> 16) & 0x1F) * 4
            slot = _face_slot((w >> 18) & 0x7F)
            if slot is None:
                hit = False
            elif slot < 0:
                hit = any(v in face for v in range(vaddr, vaddr + 8))
            else:
                hit = vaddr + slot in face
            if hit:
                src = sgpr_src.get(srsrc)
                if src is not None:
                    cubes.add('ud:%d' % src[1] if src[0] == 'ud' else 'tab:%d:%d' % (src[1], src[2]))
            for v in range(vdata, vdata + 4):
                face.discard(v)
            continue
        if vdst is not None:
            if copy_of is not None and copy_of in face:
                face.add(vdst)
            else:
                face.discard(vdst)
    return cubes


def fetch_layout(code):
    """Fase 51: os atributos que um fetch shader le, na ordem (como ParseFetchShaderLayout do port):
    (sgpr da tabela, dword do V# na tabela, componentes) de cada buffer_load_format."""
    loads = {}
    out = []
    i = 0
    while i < len(code):
        d = code[i]
        if (d & 0xFF80FF00) == 0xBE802000:  # s_setpc_b64
            break
        if d >> 27 == 0x18:  # SMRD
            loads[(d >> 15) & 0x7F] = (((d >> 9) & 0x3F) * 2, d & 0xFF)
        elif d >> 26 == 0x38 and i + 1 < len(code):  # MUBUF
            op = (d >> 18) & 0x7F
            i += 1
            if op <= 3:
                base, off = loads.get(((code[i] >> 16) & 0x1F) * 4, (0, 0))
                out.append((base, off, op + 1))
        elif d >> 26 in (0x3A, 0x34):  # MTBUF / VOP3
            i += 1
        i += 1
    return out


def words(blob):
    n = len(blob) // 4
    return list(struct.unpack('<%dI' % n, blob[:n * 4]))


# ---------------- classe do formato de vertice ----------------
# 0 = float (unorm, snorm, scaled, float), 1 = uint, 2 = sint. Formato-modelo de cada classe, com
# swizzle identidade (XYZW), usado no retrato sintetico: o SPIR-V sai o mesmo para qualquer formato
# da mesma classe com swizzle identidade.
CLASS_TEMPLATE = {0: 0x77FAC, 1: 0x54FAC, 2: 0x55FAC}


def format_class(fmt):
    n = (fmt >> 12) & 7
    return 1 if n == 4 else 2 if n == 5 else 0


def class_signature(fetch_words, classes):
    """Chave do pacote AOT, como o jogo calcula no draw: hash do fetch shader seguido de
    (0xC0000 | classe) de cada atributo."""
    s = fnv_words(fetch_words)
    for c in classes:
        s = ((s ^ (0xC0000 | c)) * 0x100000001B3) & M64
    return s
