"""Fase 78: materiais dos modelos (.ghg) e cenarios (.gsc) do jogo, lidos (so leitura) dos .DAT.

Para cada material: os pares de shader (VS, PS) de cada slot, o nome e o layout de vertices da malha (bloco DXTV).
E o que liga um par de shaders ao estado do desenho: o slot e o nome do material dizem o passe e a transparencia, o
DXTV diz o layout de vertices e a variante do par (assinatura).

Formato (big-endian, achado na Fase 78):
  .ps4_shaders  registros de shader; o registro de um VS traz um id (u32 em 'Shdr' - 0x3c) e o PS do par vem logo
                depois.
  .ghg / .gsc   bloco 'LTMU' (materiais). Cada material termina num bloco 'DXTV' (declaracao de vertices); os ids
                do PS4 ficam nos 0x24c bytes antes dele (slots em DXTV-0xf4, -0xf0, ..., -0xd0, -0xc4, -0xb8, -0xac).
                O nome do material (u16 tamanho + texto) vem antes dos slots.

uso: nu2mat.py [--app0 PASTA] [--addcont PASTA] [--patch PASTA] [--saida assets/shaders_aot/materiais.tsv]
saida (TSV): arquivo  i  nome  slots (off:vs:ps;...)  dxtv (hex)
"""
import argparse
import os
import re
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcn  # noqa: E402
import nu2dat  # noqa: E402

DXTV_BYTES = 0x40
SLOT_WINDOW = 0x24c
DEFAULT_OUT = os.path.join(nu2dat.ROOT, 'assets', 'shaders_aot', 'materiais.tsv')


def read_entry(path, offset, packed, size, method):
    with open(path, 'rb') as f:  # so leitura
        f.seek(offset)
        raw = f.read(packed)
    return raw if method == 0 else nu2dat.unpack(raw, size)


def shader_entries(data):
    """{id: (hash do VS, hash do PS)} de um .ps4_shaders (hash = gcn.fnv_bytes, o mesmo do pacote AOT)"""
    recs = []
    for m in re.finditer(b'Shdr\x07\x00', data):
        p = m.start()
        if data[p - 3:p] == b'Orb' or p < 0x3c:
            continue
        code = p + 16 + struct.unpack_from('<I', data, p + 24)[0]
        if code + 8 > len(data) or data[code:code + 4] != b'\xff\x03\xeb\xbe':
            continue
        info = code + (struct.unpack_from('<I', data, code + 4)[0] + 1) * 8
        if data[info:info + 7] != b'OrbShdr':
            continue
        recs.append((data[p + 8], struct.unpack_from('>I', data, p - 0x3c)[0], gcn.fnv_bytes(data[code:info + 28])))
    out = {}
    for i in range(len(recs) - 1):
        (s0, id0, h0), (s1, _id1, h1) = recs[i], recs[i + 1]
        if s0 == gcn.STAGE_VS and s1 == gcn.STAGE_PS:
            out[id0] = (h0, h1)
    return out


def materials(data, ids):
    """[(nome, [(slot, (vs, ps))], dxtv)] do bloco LTMU"""
    lt = data.find(b'LTMU')
    if lt < 0:
        return []
    out = []
    pos = lt
    prev_end = lt + 16
    while True:
        v = data.find(b'DXTV', pos + 4)
        if v < 0:
            break
        pos = v
        slots = []
        for o in range(max(prev_end, v - SLOT_WINDOW), v - 3):
            w = struct.unpack_from('>I', data, o)[0]
            if w in ids:
                slots.append((v - o, ids[w]))
        if not slots:
            if v - prev_end > 0x4000:  # passou do bloco de materiais
                break
            continue
        name = '?'
        lo = max(prev_end, v - 0x1400)
        for m in re.finditer(rb'\x00([\x04-\x60])([A-Za-z0-9_][ -~]{2,95})\x00', data[lo:v - SLOT_WINDOW + 0x40]):
            if m.group(1)[0] == len(m.group(2)) + 1:
                name = m.group(2).decode('ascii', 'replace')
        out.append((name, slots, data[v:v + DXTV_BYTES]))
        prev_end = v + 4
    return out


def _job_shaders(job):
    try:
        return shader_entries(read_entry(*job))
    except Exception:  # noqa: BLE001 - arquivo de outra versao do jogo: fica sem
        return {}


def _job_model(job):
    entry, ids = job
    try:
        return materials(read_entry(*entry), ids)
    except Exception:  # noqa: BLE001
        return []


def scan(out_path, app0=None, addcont=None, patch=None, workers=None, progress=None):
    """Le todos os modelos e cenarios que tem .ps4_shaders e grava o TSV. Devolve (modelos, materiais)."""
    app0 = app0 or nu2dat.DEFAULT_APP0
    dats = nu2dat.find_dats(app0, addcont, patch)
    shaders, models = {}, {}
    for pack in sorted(dats):  # na ordem: o ultimo pacote com o mesmo nome vence (atualizacao, DLC)
        size_on_disk = os.path.getsize(dats[pack])
        for name, size, packed, offset, method in nu2dat.entries(pack, '', dats[pack]):
            if offset + packed > size_on_disk:
                continue
            base, ext = os.path.splitext(name.lower())
            if ext == '.ps4_shaders':
                shaders[base] = (dats[pack], offset, packed, size, method)
            elif ext in ('.ghg', '.gsc'):
                models[base] = (name, (dats[pack], offset, packed, size, method))
    bases = sorted(b for b in models if b in shaders)
    workers = workers or max(1, min(10, (os.cpu_count() or 4) - 2))
    n_mat = 0
    tmp = out_path + '.novo'
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with ProcessPoolExecutor(workers) as ex, open(tmp, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write('# ld-materiais 1\tarquivo\ti\tnome\tslots(off:vs:ps)\tdxtv\n')
        ids_list = list(ex.map(_job_shaders, [shaders[b] for b in bases], chunksize=8))
        jobs = [(models[b][1], ids) for b, ids in zip(bases, ids_list)]
        for n, (b, mats) in enumerate(zip(bases, ex.map(_job_model, jobs, chunksize=4))):
            for i, (mname, slots, dxtv) in enumerate(mats):
                fo.write('%s\t%d\t%s\t%s\t%s\n' % (models[b][0], i, mname,
                                                   ';'.join('%x:%016x:%016x' % (off, vs, ps) for off, (vs, ps) in slots),
                                                   dxtv.hex()))
                n_mat += 1
            if progress and n % 500 == 0:
                progress(n, len(bases))
    os.replace(tmp, out_path)
    return len(bases), n_mat


def load(path):
    """{(vs, ps): [(slot, nome, dxtv)]} (sem repetidos)"""
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.rstrip('\n').split('\t')
            if len(p) < 5 or not p[3]:
                continue
            name, dxtv = p[2], p[4]
            for s in p[3].split(';'):
                off, vs, ps = s.split(':')
                out.setdefault((int(vs, 16), int(ps, 16)), set()).add((int(off, 16), name, dxtv))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app0', default=None)
    ap.add_argument('--addcont', default=None)
    ap.add_argument('--patch', default=None)
    ap.add_argument('--saida', default=DEFAULT_OUT)
    ap.add_argument('--workers', type=int, default=None)
    args = ap.parse_args()
    t0 = time.time()
    models, mats = scan(args.saida, args.app0, args.addcont, args.patch, args.workers,
                        lambda n, total: print('  %d/%d modelos' % (n, total), flush=True))
    print('MATERIAIS: %d modelos e cenarios, %d materiais em %.0f s -> %s' % (models, mats, time.time() - t0, args.saida))
    return 0


if __name__ == '__main__':
    sys.exit(main())
