"""Fase 64: prepara os mods do LEGO Dimensions para o port (rodado pelo launcher antes de abrir o jogo).

Tipos de mod aceitos (os mesmos de PS4 / shadPS4):
  - pacote da TT Games (.HDR + .DAT com nome proprio, ex. AllWorld.DAT feito com o BrickVault);
  - pacote do jogo inteiro editado (ex. PATCH.HDR + PATCH.DAT soltos, como os mods de shadPS4): entram so os arquivos
    que diferem do pacote de mesmo nome do jogo;
  - arquivos soltos no formato do /app0 (pasta com data\\ ou sce_sys\\, como a CUSA01176-mods do shadPS4).
Onde procura: <port>\\mods\\<cada pasta> (ordem alfabetica; quem esta no desligados.txt fica de fora; a primeira
vence), <jogo>-mods (shadPS4) e <jogo>\\Mods (onde alguns mods mandam extrair).

Por que nao basta montar o pacote do mod: o jogo so monta pacotes de nomes fixos (GAME, PATCH, STREAM...), e um
pacote PATCH a mais com arquivos repetidos derruba o jogo (eboot+0xcd5dc4) - os PATCH do jogo nunca repetem
arquivo entre si. Entao o mod entra DENTRO dos pacotes do jogo: para cada arquivo que o mod troca, a entrada dele
na tabela (.HDR) de todo pacote que o tem passa a apontar para uma copia dos bytes do mod, posta depois do fim do
.DAT. Os nomes, hashes e a quantidade de pacotes ficam como no PS4. O port le o manifesto e mostra ao jogo o .HDR
mudado e o .DAT com a cauda (o .DAT de verdade nunca e copiado nem alterado).

Saida em assets\\mods_cache: <PACOTE>.HDR (tabela mudada), <PACOTE>.tail (bytes dos mods), mods.txt (manifesto),
e os indices do radar em assets\\dat_index\\<PACOTE>_MODS.tsv.
Uso: mods.py --instalar MOD.zip|.rar|.7z --mods PASTA
     mods.py --root PORT --app0 JOGO [--patch PASTA] [--addcont PASTA] [--mods PASTA] [--cache PASTA] [--lingua en]
Linhas: MOD <nome>: <n> arquivos trocados, SOBREPOSICAO <pasta>, INSTALADO <pasta>, PRONTO <n>, AVISO ..., ERRO ...
"""
import argparse
import glob
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'nu2_shader_extractor'))
import nu2toc  # noqa: E402

GAME_PACK = re.compile(r'^(PATCHSTREAM|DLCSTREAM|STREAM|PATCH|GAME)\d*$', re.IGNORECASE)
LANG = 'en'
# Dados de build de cada pacote (idiomas, SKU, versao), nao conteudo de mod: num pacote do jogo editado vem os da versao
# de quem fez o mod. O ttbmetadata.xml do PATCH de um mod sem ru-ru derruba a v1.24 no boot (sem memoria, eboot+0x4a8797).
BUILD_INFO = {'\\STUFF\\TTBMETADATA.XML'}
# Arquivos que o jogo le de um pacote so (o de maior prioridade, sem juntar os dos outros) e que tem o jogo inteiro: num
# pacote proprio (BARTVOICE) vem so as linhas do mod; o text.csv de 34 linhas no lugar do de 20 mil apaga os textos.
WHOLE_GAME_FILES = {'\\STUFF\\TEXT\\TEXT.CSV'}


def T(pt, en):
    return pt if LANG == 'pt' else en


def say(text):
    print(text, flush=True)


# ---------------------------------------------------------------- onde estao os mods
def overlay_root(folder):
    """Raiz no formato do /app0 dentro do mod: a propria pasta se tiver data\\ ou sce_sys\\, ou a unica subpasta que tiver."""
    def looks(p):
        return os.path.isdir(os.path.join(p, 'data')) or os.path.isdir(os.path.join(p, 'sce_sys'))
    if looks(folder):
        return folder
    subs = [os.path.join(folder, d) for d in os.listdir(folder) if os.path.isdir(os.path.join(folder, d))]
    return subs[0] if len(subs) == 1 and looks(subs[0]) else None


def figure_dump(folder):
    """Gravacao de figura do Toy Pad (tag NFC NTAG213: .bin de 180 bytes, as vezes com 540 ou 572), nao mod."""
    bins = others = 0
    for root, _, files in os.walk(folder):
        for f in files:
            ext = os.path.splitext(f)[1].lower()
            if ext == '.bin' and os.path.getsize(os.path.join(root, f)) in (180, 540, 572):
                bins += 1
            elif ext not in ('.png', '.jpg', '.jpeg', '.txt', '.json', '.ini'):
                others += 1
    return bins > 0 and others == 0


def not_a_mod(label, folder):
    if figure_dump(folder):
        say('AVISO ' + T('%s: e a gravacao de uma figura (tag .bin do Toy Pad), nao um mod. Use no app do portal (LegoToypad) '
                         'e tire da pasta de mods' % label,
                         '%s: this is a figure dump (Toy Pad .bin tag), not a mod. Load it in the portal app (LegoToypad) '
                         'and remove it from the mods folder' % label))
    else:
        say('AVISO ' + T('%s: nada que o port saiba carregar (nem pacote .HDR/.DAT, nem pastas data\\ ou sce_sys\\)' % label,
                         '%s: nothing the port can load (no .HDR/.DAT pack, no data\\ or sce_sys\\ folders)' % label))


def find_packs(folder):
    """Pares .HDR + .DAT do mod: pacote proprio (AllWorld) ou um pacote do jogo inteiro editado (PATCH.DAT dos mods de shadPS4)."""
    packs = []
    base = folder.rstrip('\\/').count(os.sep)
    for root, dirs, files in os.walk(folder):
        if root.count(os.sep) - base >= 4:
            dirs[:] = []
        lower = {f.lower(): f for f in files}
        for f in files:
            stem, ext = os.path.splitext(f)
            if ext.lower() == '.hdr' and (stem + '.dat').lower() in lower:
                packs.append(os.path.join(root, lower[(stem + '.dat').lower()]))
    return sorted(packs, key=lambda p: p.lower())


def mod_sources(mods_dir, app0):
    sources = []
    if os.path.isdir(mods_dir):
        off = set()
        list_file = os.path.join(mods_dir, 'desligados.txt')
        if os.path.exists(list_file):
            with open(list_file, encoding='utf-8', errors='replace') as f:
                off = {line.strip().lower() for line in f if line.strip() and not line.startswith('#')}
        for name in sorted(os.listdir(mods_dir), key=str.lower):
            path = os.path.join(mods_dir, name)
            if os.path.isdir(path) and name.lower() not in off:
                sources.append((name, path))
    if app0:
        shad = app0.rstrip('\\/') + '-mods'
        if os.path.isdir(shad):
            sources.append((os.path.basename(shad) + ' (shadPS4)', shad))
        for name in ('Mods', 'mods'):
            if os.path.isdir(os.path.join(app0, name)):
                sources.append((os.path.basename(app0.rstrip('\\/')) + '\\' + name, os.path.join(app0, name)))
                break
    return sources


# ---------------------------------------------------------------- tabela dos pacotes (formato novo, .CC4)
def toc_positions(block):
    """{nome em maiusculas: posicao da entrada no bloco}, id do pacote e versao, para mudar entradas no lugar."""
    archive_id, version, _, name_count, names_size = struct.unpack_from('>iiIII', block, 12)
    folders = [''] * name_count
    names = []
    p = 36 + names_size
    entry = 12 if version >= 2 else 10
    for i in range(name_count):
        name_off, parent = struct.unpack_from('>IH', block, p)
        is_file = struct.unpack_from('>H', block, p + entry - 2)[0]
        p += entry
        if name_off == 0xFFFFFFFF:
            continue
        text = folders[parent] + '\\' + nu2toc.cstr(block, 32 + name_off)
        if i == name_count - 1:
            is_file = len(names)
        if is_file:
            names.append(text)
        else:
            folders[i] = text
    p += 4
    file_count = struct.unpack_from('>I', block, p)[0]
    p += 4
    entries_at = p
    step = (8 if version >= 2 else 4) + 8 + (0 if archive_id <= -10 else 4)
    crc_at = entries_at + step * file_count
    # o mesmo casamento nome -> arquivo do nu2toc, guardando o indice
    marker = [(i, 0, 0, 0) for i in range(file_count)]
    rows = nu2toc.link_names(block, crc_at, file_count, names, marker)
    positions = {name.upper(): entries_at + step * offset for name, _, _, offset, _ in rows}
    return positions, archive_id, version


def toc_positions_old(hdr):
    """O mesmo para o formato antigo (GAME*, STREAM*): o .HDR e um u32 com o tamanho e a tabela do .DAT.
    Entradas de 16 bytes little-endian: offset>>8, tamanho guardado, tamanho real, (byte baixo do offset << 24) | metodo."""
    block = hdr[4:]
    archive_id, file_count = struct.unpack_from('<iI', block, 0)
    marker = [(i, 0, 0, 0) for i in range(file_count)]
    info = file_count * 16 + 8
    name_entries = struct.unpack_from('<I', block, info)[0]
    info += 4
    entry_size = 12 if archive_id <= -5 else 8
    names_at = info + name_entries * entry_size
    crc_at = struct.unpack_from('<I', block, names_at)[0] + names_at + 4
    names_at += 4
    paths, names, cursor = [], [], info
    for _ in range(file_count):
        text, piece, more = '', '', 1
        while more > 0:
            more, parent, name_off = struct.unpack_from('<hhi', block, cursor)
            cursor += entry_size
            piece = nu2toc.cstr(block, names_at + name_off) if name_off >= 0 else ''
            if parent != 0:
                text = paths[parent]
            paths.append(text)
            if more > 0 and piece:
                text = text + piece + '\\'
        names.append('\\' + text + piece)
    if archive_id == -1:
        index = {name.upper(): file_count - 1 - i for i, name in enumerate(names)}
    else:
        index = {name.upper(): offset for name, _, _, offset, _ in nu2toc.link_names(block, crc_at, file_count, names, marker)}
    return {name: 4 + 8 + 16 * i for name, i in index.items()}, archive_id, 0


def write_entry(block, pos, archive_id, version, offset, zsize, usize, packed):
    if block[4:8] != b'.CC4':  # formato antigo, little-endian
        if archive_id == -1:
            raise ValueError('pack without relocated offsets')
        struct.pack_into('<IIII', block, pos, offset >> 8, zsize, usize, ((offset & 0xFF) << 24) | (2 if packed else 0))
        return
    if version < 2 or archive_id > -10:
        raise ValueError('unknown table format')
    if archive_id <= -13:
        struct.pack_into('>QII', block, pos, offset | ((2 << 56) if packed else 0), zsize, usize)
    else:
        struct.pack_into('>QII', block, pos, offset, zsize, usize | (0x80000000 if packed else 0))


# ---------------------------------------------------------------- pacotes do jogo

def game_packs(layers, addcont):
    """{PACOTE: (.HDR, .DAT) no host}: os do jogo (chunk1 tem os .HDR; os .DAT ficam nos chunkN; a camada de cima
    vence) e os dos DLCs (uma subpasta por DLC com DLCnn.HDR2 / DLCnn.DAT2, como a addcont do shadPS4)."""
    hdrs, dats = {}, {}
    for layer in reversed(layers):
        for p in glob.glob(os.path.join(layer, 'data', 'chunk*', '*.HDR')):
            hdrs[os.path.splitext(os.path.basename(p))[0].upper()] = p
        for p in glob.glob(os.path.join(layer, 'data', 'chunk*', '*.DAT')):
            dats[os.path.splitext(os.path.basename(p))[0].upper()] = p
    if addcont and os.path.isdir(addcont):
        for p in glob.glob(os.path.join(addcont, '*', '*.HDR2')):
            dat = p[:-5] + '.DAT2'
            if os.path.exists(dat):
                hdrs[os.path.splitext(os.path.basename(p))[0].upper()] = p
                dats[os.path.splitext(os.path.basename(p))[0].upper()] = dat
    return {k: (hdrs[k], dats[k]) for k in hdrs if k in dats}


def add_files_cc4(block, items):
    """Poe arquivos novos numa tabela .CC4 (PATCH): entradas e hashes na ordem dos hashes, e o indice +6 dos nos da
    arvore remapeado. Os nomes novos nao entram na arvore: o jogo acha um arquivo pelo hash FNV-1a do caminho.
    items: (nome, offset, tamanho guardado, tamanho real, comprimido). Devolve (tabela nova, nomes postos)."""
    archive_id, version, file_count, name_count, names_size = struct.unpack_from('>iiIII', block, 12)
    if version < 2 or archive_id > -10:
        raise ValueError('unknown table format')
    tree = 36 + names_size
    q = tree + 12 * name_count
    if struct.unpack_from('>iI', block, q) != (archive_id, file_count):
        raise ValueError('unexpected table layout')
    entries_at = q + 8
    hashes_at = entries_at + 16 * file_count
    rest = bytes(block[hashes_at + 4 * file_count:])
    old = [(struct.unpack_from('>I', block, hashes_at + 4 * i)[0], bytes(block[entries_at + 16 * i:entries_at + 16 * i + 16]), i)
           for i in range(file_count)]
    # confere a variante do hash com nomes que ja estao na tabela
    positions = toc_positions(bytes(block))[0]
    checked = 0
    for name, pos in list(positions.items())[:200]:
        index = (pos - entries_at) // 16
        if old[index][0] and old[index][0] != nu2toc.fnv(name[1:], 32, 1677619):
            raise ValueError('table hash differs from expected')
        checked += 1
    if not checked:
        raise ValueError('table has no verifiable names')
    taken = {h for h, _, _ in old}
    new, added = [], []
    for name, offset, zsize, usize, packed in items:
        h = nu2toc.fnv(name[1:], 32, 1677619)
        if not h or h in taken:
            continue
        taken.add(h)
        if archive_id <= -13:
            entry = struct.pack('>QII', offset | ((2 << 56) if packed else 0), zsize, usize)
        else:
            entry = struct.pack('>QII', offset, zsize, usize | (0x80000000 if packed else 0))
        new.append((h, entry, None))
        added.append(name)
    rows = sorted(old + new, key=lambda r: r[0])  # estavel: os de hash 0 (nomes por extenso) continuam no comeco
    remap = {r[2]: i for i, r in enumerate(rows) if r[2] is not None}
    out = bytearray(block[:entries_at])
    for i in range(name_count):  # so os nos de arquivo (+10 = numero do arquivo; o ultimo no e arquivo sempre)
        at = tree + 12 * i
        if struct.unpack_from('>I', out, at)[0] != 0xFFFFFFFF and (struct.unpack_from('>H', out, at + 10)[0] or i == name_count - 1):
            struct.pack_into('>H', out, at + 6, remap[struct.unpack_from('>H', out, at + 6)[0]])
    if len(rows) > 0xFFFF:
        raise ValueError('too many files for 16-bit index')
    struct.pack_into('>I', out, q + 4, len(rows))
    struct.pack_into('>I', out, 20, len(rows))
    out += b''.join(r[1] for r in rows)
    out += b''.join(struct.pack('>I', r[0]) for r in rows)
    out += rest
    struct.pack_into('>I', out, 0, len(out) - 4)
    return out, added


def fnv1_upper(text):
    """Hash dos blocos TVES dos .sound_event: FNV-1 de 32 bits (multiplica e depois xor) do nome em maiusculas."""
    h = 0x811C9DC5
    for ch in text.upper().encode('latin-1', errors='replace'):
        h = ((h * 16777619) & 0xFFFFFFFF) ^ ch
    return h


def fix_sound_event(data, stem):
    """Um .sound_event feito por ferramenta de mod (ex. o mod de voz do Bart, de connorh315) vem com os hashes dos
    blocos TVES em ff ff ff ff; o jogo acha o evento e cada fala por esses hashes, e o evento carrega mas nunca toca.
    Bloco: 'TVES', u32 1, u32 tipo, u32 hash. Tipo 8 = o evento (hash do nome do arquivo); tipo 0x0a = uma fala (hash
    do nome dela, que esta no bloco de tipo 6 seguinte); os outros tipos so precisam de um valor unico."""
    out = bytearray(data)
    starts = [m.start() for m in re.finditer(b'TVES', data)]
    changed = False
    for k, at in enumerate(starts):
        if at + 16 > len(out) or out[at + 12:at + 16] != b'\xff\xff\xff\xff':
            continue
        kind = struct.unpack_from('>I', out, at + 8)[0]
        if kind == 8:
            value = fnv1_upper(stem)
        elif kind == 0x0a:
            value = None
            for nxt in starts[k + 1:]:
                if struct.unpack_from('>I', out, nxt + 8)[0] == 6 and nxt + 31 <= len(out):
                    size = out[nxt + 30]
                    value = fnv1_upper(out[nxt + 31:nxt + 31 + size].decode('latin-1'))
                    break
            if value is None:
                value = fnv1_upper('%s#%d' % (stem, at))
        else:
            value = fnv1_upper('%s#%d' % (stem, at))
        struct.pack_into('>I', out, at + 12, value)
        changed = True
    # falas (Audio/VO/DX/...) como as do jogo: caminho terminado em _$L (onde o jogo poe o idioma: _eng, _fre...) e a
    # marca de fala localizada (09 logo depois do nome da fala no bloco de tipo 6; o mod traz 01 e um caminho sem _$L)
    starts = [m.start() for m in re.finditer(b'TVES', out)]
    pieces = [bytes(out[:starts[0]])] + [bytes(out[a:b]) for a, b in zip(starts, starts[1:] + [len(out)])] if starts else []
    for i in range(1, len(pieces)):
        blk = bytearray(pieces[i])
        if len(blk) < 0x45 or struct.unpack_from('>I', blk, 8)[0] != 6:
            continue
        at = blk.find(b'Audio/VO/DX/')
        if at < 1:
            continue
        size = blk[at - 1]
        path = bytes(blk[at:at + size])
        if path.endswith(b'_$L') or size + 3 > 255:
            continue
        blk[at - 1:at + size] = bytes([size + 3]) + path + b'_$L'
        # depois do nome da fala (tamanho em +30, texto em +31): 01 01 00 00 01 01 <marca> 09; o jogo usa 09
        flag = 31 + blk[30] + 6
        if flag + 1 < len(blk) and blk[flag] == 0x01 and blk[flag + 1] == 0x09:
            blk[flag] = 0x09
        pieces[i] = bytes(blk)
        changed = True
    if pieces:
        out = bytearray(b''.join(pieces))
        struct.pack_into('>I', out, 0, len(out) - 4)
    return bytes(out) if changed else None


LAYERED = ('\\STUFF\\TEXT\\TEXT.CSV', '\\CHARS\\COLLECTION.TXT')  # sem hash: lidos em camadas (Anomaly) pelo nome


def build_pack(files):
    """Monta um pacote .CC4 (versao 11 do LEGO Dimensions, como os de mod): files = [(caminho '\\a\\b.ext', bytes[, tamanho real, comprimido])].
    Devolve (bytes do .HDR, bytes do .DAT). Formato (lido no jogo e no BrickVault de connorh315):
      .HDR: u32 tamanho-4, '.CC40TAD', -11, 2, arquivos, nos, tamanho dos nomes, nomes (minusculas, \\0 e alinhados a
            2), nos de 12 bytes (ultimo filho, irmao anterior, nome, pai, indice do arquivo), -11, arquivos, entradas de
            16 bytes (offset u64, guardado, real), hashes FNV-1a (ordem crescente; 0 = nome por extenso, lido em
            camadas), lista dos nomes por extenso e o rodape ROTV;
      .DAT: u32 posicao codificada da tabela, u32 tamanho dela, dados a partir de 0x108 (cada um com 24 bytes antes) e
            a copia da tabela alinhada a 256."""
    seen, unique = set(), []
    for item in files:
        path, data = item[0], item[1]
        usize, packed = (item[2], item[3]) if len(item) > 2 else (len(data), False)
        if path.upper() not in seen:
            seen.add(path.upper())
            unique.append((path, data, usize, packed))
    dat = bytearray(0x108)
    stored = {}  # id dos bytes -> offset (nomes diferentes com os mesmos bytes ficam com uma copia so)
    entries = []
    for path, data, usize, packed in unique:
        if id(data) not in stored:
            # 24 bytes antes de cada arquivo: guardado, 0, real, 0, metodo (3 = DFLT, o dos comprimidos dos pacotes
            # do jogo), 0; o jogo le o metodo daqui
            dat += struct.pack('<IIIIII', len(data), 0, usize, 0, 3 if packed else 0, 0)
            stored[id(data)] = len(dat)
            dat += data
        crc = 0 if path.upper() in LAYERED else nu2toc.fnv(path.lstrip('\\'), 32, 1677619)
        entries.append((crc, path, stored[id(data)], len(data), usize, packed))
    entries.sort(key=lambda e: (e[0], e[1].upper()))

    # arvore de nomes: pre-ordem, filhos na ordem em que aparecem (a dos hashes)
    root = {'name': None, 'children': [], 'index': 0, 'file': 0}
    for file_index, (crc, path, offset, size, usize, packed) in enumerate(entries):
        node = root
        for part in path.lstrip('\\').split('\\'):
            child = next((c for c in node['children'] if c['name'] == part.lower()), None)
            if not child:
                child = {'name': part.lower(), 'children': [], 'file': 0, 'parent': node}
                node['children'].append(child)
            node = child
        node['file'] = file_index
    flat = [root]

    def walk(node):
        for child in node['children']:
            child['index'] = len(flat)
            flat.append(child)
            walk(child)
    walk(root)
    names = bytearray()
    records = bytearray()
    for node in flat:
        final_child = node['children'][-1]['index'] if node['children'] else 0
        if node is root:
            records += struct.pack('>HHiHH', final_child, 0, -1, 0, 0)
            continue
        siblings = node['parent']['children']
        position = siblings.index(node)
        previous = siblings[position - 1]['index'] if position else 0
        records += struct.pack('>HHiHH', final_child, previous, len(names), node['parent']['index'], node['file'])
        names += node['name'].encode('latin-1') + b'\0\0'  # como no PATCH do jogo e no BrickVault
    names += b'\0\0'
    hdr = bytearray(b'\0\0\0\0.CC40TAD')
    hdr += struct.pack('>iiIII', -11, 2, len(entries), len(flat), len(names))
    hdr += names + records + bytes(4)  # 4 bytes fixos depois dos nos
    hdr += struct.pack('>iI', -11, len(entries))
    for crc, path, offset, size, usize, packed in entries:
        hdr += struct.pack('>QII', offset, size, usize | (0x80000000 if packed else 0))
    for e in entries:
        hdr += struct.pack('>I', e[0])
    clashes = [e for e in entries if e[0] == 0]
    clash_block = bytearray()
    for k, (crc, path, offset, size, usize, packed) in enumerate(clashes):
        text = path.lstrip('\\').lower().encode('latin-1')
        clash_block += text + b'\0' + (b'\0' if len(text) % 2 == 0 else b'')
        clash_block += struct.pack('>H', k)
    hdr += struct.pack('>II', len(clashes), len(clash_block)) + clash_block
    hdr += b'additionalcontent/opus_beetlejuice\0' + bytes(16)  # como nos pacotes de mod do BrickVault
    count = len(entries)
    hdr += b'ROTV' + struct.pack('>I', count) + bytes(16 * count)
    hdr += b'ROTV' + struct.pack('>I', count) + bytes(16 * count)
    hdr += b'ROTV' + struct.pack('>I', count) + bytes(count)
    hdr += b'ROTV' + struct.pack('>I', count) + b''.join(struct.pack('>I', e[3]) for e in entries)
    struct.pack_into('>I', hdr, 0, len(hdr) - 4)
    header_at = (len(dat) + 0xFF) & ~0xFF
    dat += bytes(header_at - len(dat))
    dat += hdr
    struct.pack_into('<II', dat, 0, (((header_at - 0x100) >> 8) ^ 0xFFFFFFFF) & 0xFFFFFFFF, len(hdr))
    return bytes(hdr), bytes(dat)


def read_pack_files(dat):
    """[(nome, bytes guardados, comprimido, tamanho real)] de um pacote."""
    out = []
    with open(dat, 'rb') as f:
        for name, usize, zsize, offset, packed in nu2toc.read_toc(dat):
            f.seek(offset)
            out.append((name, f.read(zsize), bool(packed), usize))
    return out


def game_file(tocs, packs, upper_name, raw=False):
    """Bytes de um arquivo do jogo, do pacote de maior prioridade que o tem; None se nao achar. Sem raw, so os nao
    comprimidos; com raw, devolve (bytes guardados, tamanho real, comprimido): os blocos comprimidos comecam com o
    proprio metodo (DFLT...) e podem ir assim para outro pacote, marcados como comprimidos."""
    order = sorted(tocs, key=lambda p: (0 if p == 'PATCH' else 1 if p.startswith('PATCH') and not p.startswith('PATCHSTREAM')
                                        else 2 if p.startswith('PATCHSTREAM') else 3, p))
    for pack in order:
        block, positions, archive_id, version = tocs[pack]
        pos = positions.get(upper_name)
        if pos is None:
            continue
        if block[4:8] == b'.CC4':
            offset, zsize, usize = struct.unpack_from('>QII', block, pos)
            packed = (offset >> 56) if archive_id <= -13 else usize >> 31
            offset &= 0xFFFFFFFFFFFFFF
            usize &= 0x7FFFFFFF
        else:
            high, zsize, usize, low = struct.unpack_from('<IIII', block, pos)
            offset, packed = (high << 8) | (low >> 24), low & 0xFFFFFF
        if packed and not raw:
            continue
        with open(packs[pack][1], 'rb') as f:
            f.seek(offset)
            data = f.read(zsize)
        return (data, usize, bool(packed)) if raw else data
    return None


def camel_name(files, lower):
    """Grafia de um nome de personagem como aparece nos arquivos (ex. BartSimpson em DX_BartSimpson_Exit)."""
    counts = {}
    pattern = re.compile(re.escape(lower.encode()), re.IGNORECASE)
    for item in files:
        for m in pattern.finditer(item[1]):
            word = m.group()
            if word != word.lower() and word != word.upper():
                counts[word] = counts.get(word, 0) + 1
    return max(counts, key=counts.get).decode() if counts else lower.capitalize()


def merge_char_txt(game_txt, mod_txt, renames):
    """.txt de personagem: o do jogo (versao 1.24) com o que o mod acrescenta (linhas novas, e sfx_* trocados);
    linhas que o jogo deixa comentadas (ex. //baddie) nao entram. renames: (antigo, novo) aplicados no do mod."""
    text = game_txt.decode('latin-1')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(newline)
    mod_text = mod_txt.decode('latin-1')
    for old, new in renames:
        mod_text = re.sub(re.escape(old), new, mod_text, flags=re.IGNORECASE)
    have = {l.strip().lower() for l in lines}
    commented = [l.strip().lstrip('/').strip().lower() for l in lines if l.strip().startswith('//')]
    added = []
    for raw in mod_text.replace('\r\n', '\n').split('\n'):
        line = raw.strip()
        if not line or line.startswith('//') or line.lower() in have:
            continue
        key = line.split()[0].lower()
        if any(c.startswith(line.lower()) or c.split()[:1] == [key] and key not in ('ability',) for c in commented):
            continue
        if key.startswith('sfx_'):
            lines = [l for l in lines if not (l.strip().split()[:1] == [line.split()[0]])]
        added.append(raw.rstrip())
    while lines and not lines[-1].strip():
        lines.pop()
    return (newline.join(lines + ['', '// Fase 67: linhas do mod'] + added) + newline).encode('latin-1')


def rebuild_mod_pack(label, name, dat, cache, tocs, packs, all_names):
    """Consertos de compatibilidade de um pacote de mod do Anomaly; se algo mudar, um pacote novo (cache\\anomaly) e
    montado com os arquivos do mod consertados e os que faltam, e o Anomaly abre ele no lugar do original (que nao
    muda). Devolve (.HDR, .DAT) do pacote novo, ou None.
      - reacoes de voz emprestadas de outro personagem de nome do mesmo tamanho (o mod de voz do Bart, de connorh315,
        usa as habilidades AudioReactionData/TriggersWonderWoman, que o jogo ja registra com as falas da Mulher-Maravilha):
        viram as do proprio personagem, com o pacote global que as registra no boot (feito do da Mulher-Maravilha)
        e o .txt do personagem com AudioReactionData<Nome>;
      - eventos de som com hashes em branco e falas sem o idioma (fix_sound_event) e os .ogg com _eng, _fre...;"""
    files = read_pack_files(dat)
    changed = []
    # issue #4: a lista dos nomes por extenso (text.csv, collection.txt) do pacote LORDVORTECH de connorh315 aponta
    # cada nome para o arquivo do outro; o jogo carregava o collection.txt como texto e nunca o text.csv do jogo
    # (todos os textos viravam rotulos). O pacote novo sai com a lista certa.
    fixed = []
    nu2toc.read_toc(dat, fixed)
    if fixed:
        changed.append(T('lista dos arquivos em camadas refeita (%s estava no lugar errado)' % ', '.join(os.path.basename(n) for n in fixed),
                         'layered file list rebuilt (%s pointed to the wrong file)' % ', '.join(os.path.basename(n) for n in fixed)))
    out = []
    reaction = re.compile(r'\\CHARCACHE\\(\w+)\\ABILITIES\\CHARACTERS\\AUDIOREACTIONS_AUTOGENERATED\\(\w+)AUDIOREACTIONS~AUDIOREACTION(DATA|TRIGGERS)(\w+)\.ABILITY$')
    borrowed = {}  # personagem -> personagem de quem as reacoes foram copiadas
    for row_name, data, packed, usize in files:
        m = reaction.match(row_name.upper())
        if m and m.group(2) == m.group(4) and m.group(1) != m.group(2) and len(m.group(1)) == len(m.group(2)):
            borrowed[m.group(1).lower()] = m.group(2).lower()
    renames = []
    for char, other in borrowed.items():
        char_camel, other_camel = camel_name(files, char), None
        bundle_name = '\\ABILITIES\\CHARACTERS\\AUDIOREACTIONS_AUTOGENERATED\\%sAUDIOREACTIONS~.ABILITIES' % other.upper()
        bundle = game_file(tocs, packs, bundle_name)
        other_camel = camel_name([(bundle_name, bundle or b'', False, 0)] + files, other)
        pairs = [(other_camel, char_camel), (other.lower(), char.lower()), (other.upper(), char.upper())]
        renames.append(('AudioReactionData' + other, 'AudioReactionData' + char_camel))

        def rename(data, pairs=pairs):
            for a, b in pairs:
                data = data.replace(a.encode(), b.encode())
            return data
        for row_name, data, packed, usize in files:
            m = reaction.match(row_name.upper())
            if m and m.group(1).lower() == char and m.group(2).lower() == other and not packed:
                new_name = '\\charcache\\%s\\abilities\\characters\\audioreactions_autogenerated\\%saudioreactions~audioreaction%s%s.ability' % (
                    char, char, m.group(3).lower(), char)
                out.append((new_name, rename(data)))
                out.append(('\\abilities\\characters\\audioreactions_autogenerated\\%saudioreactions~audioreaction%s%s.ability' % (
                    char, m.group(3).lower(), char), rename(data)))
        if bundle:
            out.append(('\\abilities\\characters\\audioreactions_autogenerated\\%saudioreactions~.abilities' % char, rename(bundle)))
        source = game_file(tocs, packs, '\\ABILITIES\\CHARACTERS\\AUDIOREACTIONS_AUTOGENERATED\\%sAUDIOREACTIONS.TXT' % other.upper())
        if source:
            out.append(('\\abilities\\characters\\audioreactions_autogenerated\\%saudioreactions.txt' % char, rename(source)))
        # abilities\abilities.txt e o indice dos pacotes de habilidade lidos no boot: sem a linha do personagem, as reacoes
        # dele nunca sao registradas
        index = next((d for n, d in out if n.upper() == '\\ABILITIES\\ABILITIES.TXT'), None) or game_file(tocs, packs, '\\ABILITIES\\ABILITIES.TXT')
        if index:
            text = index.decode('latin-1')
            newline = '\r\n' if '\r\n' in text else '\n'
            line = 'txt_file "abilities/Characters/AudioReactions_AUTOGENERATED/%sAudioReactions.txt"' % char_camel
            if line.lower() not in text.lower():
                rows = text.split(newline)
                at = next((i for i, r in enumerate(rows) if ('/%saudioreactions.txt"' % other) in r.lower()), len(rows) - 1)
                rows.insert(at + 1, line)
                out = [(n, d) for n, d in out if n.upper() != '\\ABILITIES\\ABILITIES.TXT']
                out.append(('\\abilities\\abilities.txt', newline.join(rows).encode('latin-1')))
        changed.append(T('reacoes de voz de %s viraram de %s' % (other_camel, char_camel), 'voice reactions of %s became %s\'s' % (other_camel, char_camel)))
        # .txt do personagem: o do jogo + o que o mod acrescenta
        for row_name, data, packed, usize in files:
            if re.match(r'\\CHARS\\\w+\\%s\\%s\.TXT$' % (char.upper(), char.upper()), row_name.upper()) and not packed:
                game_txt = game_file(tocs, packs, row_name.upper())
                if game_txt:
                    out.append((row_name, merge_char_txt(game_txt, data, renames)))
                    changed.append(T('%s com a versao do jogo + o mod' % os.path.basename(row_name), '%s from the game version + the mod' % os.path.basename(row_name)))
    replaced = {n.upper() for n, _ in out}
    suffixes = sorted({n.rsplit('_', 1)[1][:-4] for n in all_names
                       if n.startswith('\\AUDIO\\VO\\DX\\') and re.search(r'_[A-Z]{3}\.OGG$', n)})
    events = 0
    for row_name, data, packed, usize in files:
        upper = row_name.upper()
        m = reaction.match(upper)
        if m and m.group(1).lower() in borrowed and m.group(2).lower() == borrowed[m.group(1).lower()]:
            continue  # copia emprestada: substituida pela do proprio personagem
        if upper in replaced:
            continue
        if upper.endswith('.SOUND_EVENT') and not packed:
            new = fix_sound_event(data, os.path.splitext(os.path.basename(row_name))[0])
            if new:
                events += 1
                data = new
        out.append((row_name, data, usize, packed) if packed else (row_name, data))
        if upper.startswith('\\AUDIO\\VO\\DX\\') and upper.endswith('.OGG') and not re.search(r'_[A-Z]{3}\.OGG$', upper):
            for s in suffixes:
                alias = '%s_%s.ogg' % (row_name[:-4], s.lower())
                if alias.upper() not in all_names:
                    out.append((alias, data))
    if events:
        changed.append(T('%d evento(s) de som consertados, falas com os nomes %s' % (events, ', '.join('_' + s.lower() for s in suffixes)),
                         '%d sound event(s) fixed, lines with the names %s' % (events, ', '.join('_' + s.lower() for s in suffixes))))
    if not changed:
        return None
    hdr_bytes, dat_bytes = build_pack(out)
    folder = os.path.join(cache, 'anomaly')
    os.makedirs(folder, exist_ok=True)
    new_hdr, new_dat = os.path.join(folder, name + '.HDR'), os.path.join(folder, name + '.DAT')
    with open(new_hdr, 'wb') as f:
        f.write(hdr_bytes)
    with open(new_dat, 'wb') as f:
        f.write(dat_bytes)
    say('CONSERTO ' + '%s: %s' % (label, '; '.join(changed)))
    return new_hdr, new_dat


FIGURES_FILE = os.path.join(HERE, '..', 'data', 'figuras_ocultas.json')
TAG_CONSTANT = bytes([0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8, 0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA])


def tag_character_id(tag):
    """Id do personagem numa gravacao de tag do Toy Pad (NTAG213): [id, id] cifrado com TEA nas paginas 0x24-0x25,
    chave tirada do UID (o mesmo algoritmo do hle_toypad.cpp). 0 se nao for um personagem valido."""
    if len(tag) < 0x26 * 4:
        return 0
    mask = 0xFFFFFFFF
    if tag[0x26 * 4 + 1] == 1:
        return 0  # veiculo / gadget
    uid = bytes([tag[0], tag[1], tag[2], tag[4], tag[5], tag[6], tag[7]])

    def rotr(v, n):
        return ((v >> n) | (v << (32 - n))) & mask

    def scramble(b, words):
        v = 0
        for i in range(words):
            v = (struct.unpack_from('<I', b, i * 4)[0] + rotr(v, 25) + rotr(v, 10) - v) & mask
        return v
    key = []
    for i in range(4):
        base = bytearray(uid + TAG_CONSTANT)
        base[(3 + i) * 4 - 1] = 0xAA
        key.append(scramble(bytes(base), 3 + i))
    v0, v1 = struct.unpack_from('<II', tag, 0x24 * 4)
    delta, total = 0x9E3779B9, (0x9E3779B9 * 32) & mask
    for _ in range(32):
        v1 = (v1 - ((((v0 << 4) & mask) + key[2]) ^ ((v0 + total) & mask) ^ ((v0 >> 5) + key[3]))) & mask
        v0 = (v0 - ((((v1 << 4) & mask) + key[0]) ^ ((v1 + total) & mask) ^ ((v1 >> 5) + key[1]))) & mask
        total = (total - delta) & mask
    return v0 if v0 == v1 else 0


def figure_tags(folder):
    """Ids de personagem das gravacoes .bin de tag numa pasta de mod."""
    ids = set()
    for base, _, files in os.walk(folder):
        for f in files:
            path = os.path.join(base, f)
            if f.lower().endswith('.bin') and os.path.getsize(path) in (180, 540, 572):
                with open(path, 'rb') as fh:
                    found = tag_character_id(fh.read())
                if found:
                    ids.add(found)
    return ids


def cd_objects(data):
    """Objetos com tamanho de um .cd: (posicao, tamanho, tag). Cada um e u32 tamanho (contando ele), u32 5, TAG\\0."""
    return [(m.start() - 4, struct.unpack_from('<I', data, m.start() - 4)[0], m.group(1))
            for m in re.finditer(rb'\x05\x00\x00\x00(MOBJ|OLST)\x00', data)]


def set_anim_sets(data, current, names):
    """Troca a lista de conjuntos de animacao de um .cd (a OLST que tem o nome current; formato dos .cd de inimigo:
    u16 tipo, u32 quantos, um MOBJ por nome = u32 0, u32 tamanho, nome\\0, u8 Force Load) e corrige o tamanho dos
    objetos que a contem. None se o arquivo nao tiver esse formato."""
    at = data.find(current.encode('latin-1') + b'\0')
    lists = [(p, s) for p, s, t in cd_objects(data) if t == b'OLST' and p < at < p + s] if at >= 0 else []
    if not lists:
        return None
    p, size = min(lists, key=lambda x: x[1])
    kind, count = struct.unpack_from('<HI', data, p + 13)
    pos = p + 19
    for _ in range(count):
        child = struct.unpack_from('<I', data, pos)[0]
        if data[pos + 4:pos + 13] != b'\x05\x00\x00\x00MOBJ\x00' or child != 22 + struct.unpack_from('<I', data, pos + 17)[0]:
            return None
        pos += child
    if pos != p + size:
        return None
    body = b''
    for name in names:
        s = name.encode('latin-1') + b'\0'
        body += struct.pack('<II', 22 + len(s), 5) + b'MOBJ\0' + struct.pack('<II', 0, len(s)) + s + b'\0'
    new = struct.pack('<II', 19 + len(body), 5) + b'OLST\0' + struct.pack('<HI', kind, len(names)) + body
    out = bytearray(data[:p] + new + data[p + size:])
    for q, s, _ in cd_objects(data):
        if q < p and q + s >= p + size:
            struct.pack_into('<I', out, q, s + len(new) - size)
    return bytes(out)


def figure_txt(info, tocs, packs):
    """O .txt do personagem como figura jogavel: as habilidades escolhidas do doador, as marcas (ex. magic), o cajado
    da figura no lugar do item dele e sem as linhas de inimigo."""
    txt = game_file(tocs, packs, info['txt'].upper())
    donor_txt = game_file(tocs, packs, info['txt_doador'].upper())
    if txt is None or donor_txt is None:
        return None
    wanted = set(info['habilidades_do_doador'])
    ability = re.compile(r'\s*Ability "([^"]+)"')
    abilities = [line.strip() for line in donor_txt.decode('latin-1').splitlines()
                 if ability.match(line) and ability.match(line).group(1) in wanted]
    lines = []
    for line in txt.decode('latin-1').splitlines():
        s = line.strip()
        if s in info.get('tirar_do_txt', []):
            line = '//' + line
        elif s.startswith('//') and any(s[2:].lstrip().startswith(x) for x in info.get('descomentar_no_txt', [])):
            line = s[2:].lstrip()
        elif 'item' in info and s.startswith('default_item "%s"' % info['item']['atual']):
            line = 'default_item "%s" always_got held' % info['item']['nome']
        lines.append(line)
    # as habilidades do doador entram depois da ultima do personagem; as marcas, no fim
    last = max((i for i, line in enumerate(lines) if ability.match(line)), default=len(lines) - 1)
    lines[last + 1:last + 1] = abilities
    lines += [''] + info.get('marcas', [])
    return '\r\n'.join(lines).encode('latin-1') + b'\r\n'


def figure_item(info, tocs, packs):
    """O arquivo de itens com um item novo para a figura: copia do item do doador (habilidades de tiro, magia etc.)
    com o modelo do item do personagem, e que pode ser guardado nas costas (guardado, a figura usa as animacoes
    normais de minifigura)."""
    item = info['item']
    data = game_file(tocs, packs, item['arquivo'].upper())
    if data is None:
        return None
    text = data.decode('latin-1')
    m = re.search(r'item_type "%s".*?\n}\r?\n' % re.escape(item['base']), text, re.S)
    if not m:
        return None
    block = m.group(0).replace('item_type "%s"' % item['base'], 'item_type "%s"' % item['nome'])
    block = re.sub(r'(\n\s*obj\s+)"[^"]*"', lambda x: x.group(1) + '"%s"' % item['obj'], block, count=1)
    block = re.sub(r'(\n\s*name\s+)"[^"]*"', lambda x: x.group(1) + '"%s"' % item['nome'], block, count=1)
    block = re.sub(r'\n(\s*)cannot_manual_put_away', r'\n\1//cannot_manual_put_away', block)
    # as habilidades do item do proprio personagem tambem (ex. o portal dentro do anel do cajado do Vortech)
    own = re.search(r'item_type "%s".*?\n}\r?\n' % re.escape(item['atual']), text, re.S)
    if own:
        for line in own.group(0).splitlines():
            s = line.strip()
            if re.match(r'(ItemAbility|OwnerAbility)\s', s) and s not in block:
                block = block[:block.rstrip().rfind('}')] + '\t' + s + '\r\n}\r\n'
    block = block[:block.rstrip().rfind('}')] + ('\tputaway_locator\r\n\t{\r\n\t\tlocator "BackPack"\r\n\t\tmaya_offset 0 0 -0.9\r\n'
                                                  '\t\tmaya_rot -100 -88.078 0\r\n\t\tscale 0.6\r\n\t}\r\n}\r\n')
    return (text.rstrip() + '\r\n\r\n' + block).encode('latin-1')


def parse_reactions(txt):
    """O .txt de reacoes de voz (abilities\\characters\\audioreactions_autogenerated\\<Nome>AudioReactions.txt): as
    habilidades com classes, sub-habilidades citadas e o bloco subability_start achatado como o jogo guarda
    (reaction_start=#x,audio_reaction='y',...)."""
    abilities, cur, sub = [], None, None
    for raw in txt.splitlines():
        line = raw.split('//')[0].strip()
        if not line:
            continue
        m = re.match(r'(ability_start|canuseon_edclass|subability_start|subability)\s+"([^"]+)"', line)
        if m and m.group(1) == 'ability_start':
            cur = {'name': m.group(2), 'edclass': [], 'subability': [], 'sub': None}
            abilities.append(cur)
        elif line == 'ability_end':
            cur = None
        elif m and m.group(1) == 'subability_start':
            sub = (m.group(2), [])
            cur['sub'] = sub
        elif line == 'subability_end':
            sub = None
        elif sub is not None:
            kv = re.match(r'(\w+)\s*=\s*"([^"]*)"$', line)
            sub[1].append("%s='%s'" % (kv.group(1), kv.group(2)) if kv else re.sub(r'\s*=\s*', '=', line))
        elif m and m.group(1) == 'canuseon_edclass':
            cur['edclass'].append(m.group(2))
        elif m and m.group(1) == 'subability':
            cur['subability'].append(m.group(2))
        else:
            raise ValueError(line)
    return abilities


def reaction_record(ab):
    """Uma habilidade de reacoes serializada como no jogo (u16 BE de tamanho antes de cada texto, que acaba em \\0)."""
    s16 = lambda t: struct.pack('>H', len(t) + 1) + t.encode('latin-1') + b'\0'  # noqa: E731
    out = struct.pack('>I', len(ab['subability']) + 1)
    for s in ab['subability']:
        out += s16(s) + b'\x00\x01\x00\x00'
    return out + s16(ab['sub'][0]) + s16(','.join(ab['sub'][1])), s16


def compile_reaction_bundle(abilities, stem):
    """Pacote ~.abilities (STBA) registrado pela linha txt_file do abilities.txt; stem = nome do .txt como esta nessa
    linha. Conferido: igual ao do jogo em 124 de 125 personagens (o outro tem o .txt mais novo que o compilado)."""
    body = struct.pack('>I', len(abilities))
    for ab in abilities:
        record, s16 = reaction_record(ab)
        body += s16(ab['name']) + s16('abilities/Characters/AudioReactions_AUTOGENERATED/%s~%s.ability' % (stem, ab['name']))
        body += b'\x01' + s16(ab['name']) + record + struct.pack('>II', 0, len(ab['edclass']))
        body += b''.join(s16(e) for e in ab['edclass']) + struct.pack('>H', len(ab['subability']) + 1)
        body += b''.join(s16(s) for s in ab['subability'] + [ab['sub'][0]]) + b'\0' * 6
    data = b'.CC4STBA' + body
    return struct.pack('>I', len(data)) + data


def compile_reaction_ability(template, ab):
    """Um .ability (Data ou Triggers): o cabecalho HSER do modelo (dependencias, ja com o nome certo) + o bloco FEDA
    da habilidade. Conferido: FEDA igual ao do jogo em 359 de 360 arquivos."""
    record, s16 = reaction_record(ab)
    body = b'.CC4FEDA\0\0' + s16(ab['name']) + record + b'\0' * 6
    return template[:template.find(b'.CC4FEDA') - 4] + struct.pack('>I', len(body)) + body


def figure_reactions(char, info, tocs, packs, other_indexes=()):
    """Falas de figura (entrada, saida, parado, em dupla, veiculos...) de um personagem que o jogo deixou sem as
    reacoes de voz compiladas (a linha dele no abilities\\abilities.txt esta comentada), compiladas do .txt dele que o
    jogo tem; o cabecalho dos .ability vem do personagem "reacoes_de", de nome do mesmo tamanho, renomeado. Os eventos
    de som e as falas dx_<nome>_* ja estao no jogo. None se faltar algo."""
    other = info['reacoes_de'].lower()
    if len(other) != len(char):
        return None
    base = '\\abilities\\characters\\audioreactions_autogenerated\\'
    stem = info['collect'] + 'AudioReactions'
    source = game_file(tocs, packs, (base + '%saudioreactions.txt' % char).upper())
    index = game_file(tocs, packs, '\\ABILITIES\\ABILITIES.TXT')
    if source is None or index is None:
        return None
    try:
        abilities = parse_reactions(source.decode('latin-1'))
    except (ValueError, AttributeError, TypeError):
        return None
    pairs = [(other, char), (other.upper(), char.upper())]

    def rename(data):
        for a, b in pairs:
            data = re.sub(re.escape(a.encode('latin-1')), b.encode('latin-1'), data, flags=re.IGNORECASE)
        return data
    out = [(base + '%saudioreactions~.abilities' % char, compile_reaction_bundle(abilities, stem))]
    for kind in ('data', 'triggers'):
        ab = next((a for a in abilities if a['name'].lower().startswith('audioreaction' + kind)), None)
        if ab is None:
            return None
        name = '%saudioreactions~audioreaction%s%%s.ability' % ('%s', kind)
        for prefix in ('', '\\charcache\\%s' % other):
            got = game_file(tocs, packs, (prefix + base + name % (other, other)).upper())
            if got is None or b'.CC4FEDA' not in got:
                return None
            out.append(((prefix.replace(other, char) + base + name % (char, char)), compile_reaction_ability(rename(got), ab)))
    text = index.decode('latin-1')
    line = 'txt_file "abilities/Characters/AudioReactions_AUTOGENERATED/%sAudioReactions.txt"' % info['collect']
    m = re.search(r'^[ \t]*//[ \t]*' + re.escape(line), text, re.M | re.I)
    if m:
        text = text[:m.start()] + line + text[m.end():]
    elif line.lower() not in text.lower():
        text = text.rstrip() + '\r\n' + line + '\r\n'
    # este pacote vai na frente dos outros mods e o abilities.txt nao e lido em camadas: entram tambem as linhas que
    # os outros pacotes de mod acrescentaram ao deles (ex. as reacoes do Bart)
    have = {l.strip().lower() for l in text.splitlines()}
    extra = []
    for other_index in other_indexes:
        for l in other_index.decode('latin-1').splitlines():
            s = l.strip()
            if s.lower().startswith('txt_file') and s.lower() not in have:
                have.add(s.lower())
                extra.append(s)
    if extra:
        text = text.rstrip() + '\r\n' + '\r\n'.join(extra) + '\r\n'
    out.append(('\\abilities\\abilities.txt', text.encode('latin-1')))
    return out


def figure_text(info, tocs, packs):
    """text.csv so com os rotulos novos da figura (ex. o nome, que o jogo nao tem para um personagem nao jogavel), no
    formato que o merge_text junta ao texto do jogo."""
    game_text = game_file(tocs, packs, '\\STUFF\\TEXT\\TEXT.CSV')
    if game_text is None:
        return None
    header = csv_row(game_text.decode('cp1252', errors='replace').split('\n', 1)[0].strip('\r'))
    rows = [header[:3] + ['ENGLISH']] + [[label, 'All', kind, value] for label, kind, value in info['textos']]
    return '\r\n'.join(','.join('"%s"' % c.replace('"', '""') for c in r) for r in rows).encode('utf-8') + b'\r\n'


def hidden_figure_pack(char, info, tocs, packs, all_names, cache, other_dats=()):
    """Pacote do Anomaly que libera uma figura desligada no jogo (ver figuras_ocultas.json): a linha Collect (em
    camadas no chars\\collection.txt) e o charcache\\<nome>: o do doador inteiro (animacoes, habilidades e o resto que
    uma figura carrega), o .cd do personagem e os recursos dele. Os blocos comprimidos vao como estao. None se faltar
    algo no jogo."""
    files = [('\\chars\\collection.txt', ('Collect "%s"\tportal_model_id %d\tcharacter_tag\r\n' % (info['collect'], info['id'])).encode('latin-1'))]
    donor = '\\CHARCACHE\\%s\\' % info['doador'].upper()
    skip = {(donor + info['cd_doador'].lstrip('\\') + ext).upper() for ext in ('.CD', '.CD.RES')}
    target = '\\charcache\\%s\\' % char

    def add(dest, source):
        got = game_file(tocs, packs, source.upper(), raw=True)
        if got is None:
            return False
        data, usize, packed = got
        files.append((dest, data, usize, packed) if packed else (dest, data))
        return True
    for name in sorted(n for n in all_names if n.startswith(donor) and n not in skip):
        add(target + name[len(donor):].lower(), name)
    base = target + 'chars\\minifig\\%s\\%s' % (char, char)
    if not add(base + '.cd', info['cd'] + '.cd'):
        return None
    # Fase 67: o .cd de inimigo do personagem so cita os conjuntos de animacao das fases dele (sem eles a figura fica
    # em T); a lista vira a do doador: as animacoes de cajado e habilidades dele + o SharedAnims de toda figura.
    cd = files[-1]
    sets = info.get('conjuntos_de_animacao')
    if sets:
        cd_data = set_anim_sets(cd[1], sets['atual'], sets['novos']) if len(cd) == 2 else None
        if cd_data is None:
            return None
        files[-1] = (cd[0], cd_data)
    if not add(base + '.cd.res', info['cd'] + '.cd.res'):
        return None
    if 'txt' in info:
        txt = figure_txt(info, tocs, packs)
        if txt is None:
            return None
        files.append((info['txt'], txt))
    if 'item' in info:
        items = figure_item(info, tocs, packs)
        if items is None:
            return None
        files.append((info['item']['arquivo'], items))
    if 'reacoes_de' in info:
        indexes = [data for dat in other_dats for row, data, packed, _ in read_pack_files(dat)
                   if row.upper() == '\\ABILITIES\\ABILITIES.TXT' and not packed]
        reactions = figure_reactions(char, info, tocs, packs, indexes)
        if reactions is None:
            return None
        files += reactions
    if 'textos' in info:
        text = figure_text(info, tocs, packs)
        if text is None:
            return None
        files.append(('\\stuff\\text\\text.csv', text))
    for extra in info['extras']:
        add(target + extra.lstrip('\\'), extra)
    # arquivos do jogo no lugar de outros dentro do charcache: animacoes do proprio personagem no lugar de algumas do
    # doador (mesmo esqueleto de minifigura; os conjuntos de animacao do doador continuam dizendo quando cada uma
    # toca) e a capa com fisica de pano no lugar da movida pelos ossos (as animacoes do personagem nao mexem a capa)
    for dest, source in info.get('trocar_arquivos', []):
        got = game_file(tocs, packs, source.upper(), raw=True)
        if got is None:
            return None
        data, usize, packed = got
        name = target + dest.lstrip('\\')
        files = [f for f in files if f[0].lower() != name.lower()]
        files.append((name, data, usize, packed) if packed else (name, data))
    hdr_bytes, dat_bytes = build_pack(files)
    folder = os.path.join(cache, 'anomaly')
    os.makedirs(folder, exist_ok=True)
    name = 'FIGURA_' + char.upper()
    hdr, dat = os.path.join(folder, name + '.HDR'), os.path.join(folder, name + '.DAT')
    with open(hdr, 'wb') as f:
        f.write(hdr_bytes)
    with open(dat, 'wb') as f:
        f.write(dat_bytes)
    say('FIGURA ' + T('%s (figura %d) liberado: %d arquivos do jogo no cache da figura (%.0f MB). Ponha o .bin no app do portal.' % (
        info['nome'], info['id'], len(files), len(dat_bytes) / 1e6, ),
        '%s (figure %d) enabled: %d game files in the figure cache (%.0f MB). Load the .bin in the portal app.' % (
        info['nome'], info['id'], len(files), len(dat_bytes) / 1e6)))
    return name, hdr, dat


def csv_row(line):
    import csv
    return next(csv.reader([line]), [])


def merge_text(base, extras):
    """text.csv do jogo + as linhas (rotulos novos) do text.csv de cada mod; o jogo so le um text.csv (o do pacote de
    maior prioridade), entao as legendas e nomes de um mod de pacote proprio entram neste."""
    import csv
    import io
    # issue #4: o text.csv do jogo e UTF-8; lido e regravado como cp1252, os bytes 0x81 / 0x8d... de letras como Á viravam
    # "?" e o jogo descartava o arquivo inteiro (todos os textos viravam rotulos). Os bytes do jogo ficam como estao e
    # so as linhas novas sao codificadas, na mesma codificacao.
    try:
        text, encoding = base.decode('utf-8'), 'utf-8'
    except UnicodeDecodeError:
        text, encoding = base.decode('cp1252', errors='replace'), 'cp1252'
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(newline)
    header = csv_row(lines[0])
    seen = {csv_row(l)[0].upper() for l in lines[1:] if l.strip() and csv_row(l)}
    added = []
    for label, data in extras:
        try:
            mod_text = data.decode('utf-8')
        except UnicodeDecodeError:
            mod_text = data.decode('cp1252', errors='replace')
        rows = list(csv.reader(io.StringIO(mod_text)))
        if not rows:
            continue
        mod_header = [h.upper().lstrip('﻿').replace(' ', '_') for h in rows[0]]  # FRENCH CANADIAN = FRENCH_CANADIAN
        if not mod_header or mod_header[0] != 'LABEL':
            # issue #4: nao e um text.csv (ex. o collection.txt lido no lugar dele); linhas sem rotulo apagam os textos
            say('AVISO ' + T('%s: text.csv sem o cabecalho LABEL ignorado' % label, '%s: text.csv without the LABEL header skipped' % label))
            continue
        for row in rows[1:]:
            if not row or not row[0] or row[0].upper() in seen:
                continue
            fields = dict(zip(mod_header, row))
            english = fields.get('ENGLISH') or next((v for v in row[3:] if v), '')
            out = io.StringIO()
            csv.writer(out, quoting=csv.QUOTE_ALL, lineterminator='').writerow(
                [fields.get(h.upper().replace(' ', '_')) or (english if i >= 3 else '') for i, h in enumerate(header)])
            added.append(out.getvalue())
            seen.add(row[0].upper())
    if not added:
        return None, 0
    body = base.rstrip(b'\r\n\t ')
    merged = body + (newline + newline.join(added) + newline).encode(encoding, errors='replace')
    return merged, len(added)


def merge_lines(base, versions):
    """Junta as mudancas de cada versao (ordem = prioridade) sobre o texto base, linha a linha. Devolve (linhas,
    conflitos): trechos que duas versoes mudam de jeitos diferentes ficam com a de maior prioridade."""
    import difflib
    edits = []
    for order, lines in enumerate(versions):
        matcher = difflib.SequenceMatcher(None, base, lines, autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag != 'equal':
                edits.append((i1, i2, order, lines[j1:j2]))
    edits.sort(key=lambda e: (e[0], e[1], e[2]))
    out, pos, applied, conflicts = [], 0, [], 0
    for i1, i2, _order, new in edits:
        if (i1, i2, new) in applied:
            continue  # a mesma mudanca em dois mods
        if i1 < pos:
            conflicts += 1
            continue
        out.extend(base[pos:i1])
        out.extend(new)
        pos = i2
        applied.append((i1, i2, new))
    out.extend(base[pos:])
    return out, conflicts


def merged_mod_texts(anomaly, tocs, packs, cache):
    """Os pacotes do Anomaly sao lidos em ordem e so o primeiro que tem um arquivo vale: o abilities.txt do LORDVORTECH
    (connorh315) sumia atras do do BARTVOICE, e o .txt de um personagem do AllWorld escondia o do mod do personagem.
    Cada .txt que dois ou mais pacotes trazem vira um so, com as mudancas de todos sobre o do jogo, num pacote que
    entra na frente. Devolve (rotulo, nome, .HDR, .DAT) ou None."""
    layered = {'\\CHARS\\COLLECTION.TXT'}
    seen = {}
    for label, _name, _hdr, dat in anomaly:
        for row_name, data, packed, _usize in read_pack_files(dat):
            upper = row_name.upper()
            if upper.endswith('.TXT') and upper not in layered and not packed:
                seen.setdefault(upper, (row_name, []))[1].append((label, data))
    files, notes = [], []
    for upper, (row_name, versions) in sorted(seen.items()):
        if len(versions) < 2 or all(v[1] == versions[0][1] for v in versions):
            continue
        base = game_file(tocs, packs, upper)
        if base is None:
            continue
        text = base.decode('latin-1')
        newline = '\r\n' if '\r\n' in text else '\n'
        lines, conflicts = merge_lines(text.splitlines(), [v[1].decode('latin-1').splitlines() for v in versions])
        merged = (newline.join(lines) + (newline if text.endswith(('\n', '\r')) else '')).encode('latin-1')
        files.append((row_name, merged))
        notes.append('%s (%s%s)' % (os.path.basename(row_name), ', '.join(v[0] for v in versions),
                                    T(', %d conflito(s): vale o primeiro' % conflicts, ', %d conflict(s): the first wins' % conflicts) if conflicts else ''))
    if not files:
        return None
    hdr_bytes, dat_bytes = build_pack(files)
    folder = os.path.join(cache, 'anomaly')
    os.makedirs(folder, exist_ok=True)
    hdr, dat = os.path.join(folder, 'MODS_JUNTOS.HDR'), os.path.join(folder, 'MODS_JUNTOS.DAT')
    with open(hdr, 'wb') as f:
        f.write(hdr_bytes)
    with open(dat, 'wb') as f:
        f.write(dat_bytes)
    say('CONSERTO ' + T('arquivos que mais de um mod muda, juntos: ', 'files changed by more than one mod, merged: ') + '; '.join(notes))
    return T('mods juntos', 'merged mods'), 'MODS_JUNTOS', hdr, dat


def read_ignore(folder):
    """mods\\<mod>\\ignorar.txt: um caminho interno por linha (ex. collectables\\cheats.txt) que o port nao usa do mod."""
    path = os.path.join(folder, 'ignorar.txt')
    names = set()
    if os.path.isfile(path):
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                line = line.split('#')[0].strip().replace('/', '\\')
                if line:
                    names.add(('\\' + line.lstrip('\\')).upper())
    return names


def loose_files(folder, top_dirs):
    """Arquivos soltos com o caminho interno do jogo, como os mods de PC (chars\\collection.txt na pasta do mod): so os
    que estao numa pasta de primeiro nivel que o jogo tem (chars, audio, stuff...), para deixar de fora leia-me e fotos."""
    out = []
    for base, dirs, files in os.walk(folder):
        for f in files:
            if os.path.splitext(f)[1].lower() in ('.hdr', '.dat', '.hdr2', '.dat2'):
                continue
            path = os.path.join(base, f)
            rel = os.path.relpath(path, folder).replace('/', '\\')
            if '\\' in rel and rel.split('\\')[0].lower() in top_dirs:
                out.append(('\\' + rel, path))
    return out


# ---------------------------------------------------------------- principal
def prepare(root, app0, patch, mods_dir, cache, addcont, loader='anomaly'):
    os.makedirs(cache, exist_ok=True)
    manifest = os.path.join(cache, 'mods.txt')
    sources = mod_sources(mods_dir, app0)
    overlays, mod_files, loose_sources = [], [], []  # mod_files: (ordem da fonte, rotulo, .DAT, pasta)
    stamp = hashlib.sha1()
    with open(os.path.abspath(__file__), 'rb') as f:  # um mods.py novo (atualizacao do port) refaz o cache
        stamp.update(f.read())
    stamp.update(loader.encode())
    if os.path.isfile(FIGURES_FILE):
        with open(FIGURES_FILE, 'rb') as f:
            stamp.update(f.read())

    def stamp_file(path):
        st = os.stat(path)
        stamp.update(('%s|%d|%d' % (path, st.st_size, st.st_mtime_ns)).encode('utf-8', 'replace'))
    for source, (label, folder) in enumerate(sources):
        over = overlay_root(folder)
        if over:
            overlays.append(over)
            say('SOBREPOSICAO %s' % over)
            for base, _, files in os.walk(over):
                for f in files:
                    stamp_file(os.path.join(base, f))
        packs_here = find_packs(folder)
        for dat in packs_here:
            stem = os.path.splitext(os.path.basename(dat))[0]
            if over and GAME_PACK.match(stem) and os.path.normcase(dat).startswith(os.path.normcase(over) + os.sep):
                continue  # pacote do jogo dentro de data\chunkN: a sobreposicao ja troca o pacote inteiro
            stamp_file(dat)
            mod_files.append((source, label + ': ' + stem, dat, folder))
        if not over:
            loose_sources.append((source, label, folder))
            for base, _, files in os.walk(folder):
                for f in files:
                    if os.path.splitext(f)[1].lower() not in ('.hdr', '.dat', '.hdr2', '.dat2'):
                        stamp_file(os.path.join(base, f))
    layers = overlays + ([patch] if patch and os.path.isdir(patch) else []) + [app0]
    packs = game_packs(layers, addcont)
    for name, (hdr_path, dat_path) in sorted(packs.items()):
        stamp_file(hdr_path)
        stamp_file(dat_path)
    key = stamp.hexdigest()
    if os.path.exists(manifest):
        with open(manifest, encoding='utf-8') as f:
            if f.readline().strip() == '# chave ' + key:
                say(T('mods ja preparados (nada mudou)', 'mods already prepared (nothing changed)'))
                say('PRONTO %d' % len(sources))
                return 0
    for old in glob.glob(os.path.join(cache, '*.HDR')) + glob.glob(os.path.join(cache, '*.tail')):
        os.remove(old)
    for old in glob.glob(os.path.join(cache, 'anomaly', '*')):  # pacotes de mod consertados (Fase 67)
        os.remove(old)

    # tabelas de todos os pacotes do jogo (nome -> posicao da entrada), para saber o que ja existe
    tocs = {}
    for pack, (hdr_path, dat_path) in sorted(packs.items()):
        with open(hdr_path, 'rb') as f:
            block = bytearray(f.read())
        try:
            tocs[pack] = (block,) + (toc_positions if block[4:8] == b'.CC4' else toc_positions_old)(bytes(block))
        except Exception as e:
            say('AVISO ' + T('tabela de %s nao lida (%s)' % (pack, e), 'table of %s not read (%s)' % (pack, e)))
    all_names = set()
    for _block, positions, _id, _version in tocs.values():
        all_names.update(positions)
    top_dirs = {n.split('\\')[1].lower() for n in all_names if n.count('\\') > 1}

    # arquivos de cada mod, por (escopo, nome): escopo None = pacote proprio (AllWorld) ou arquivo solto, que troca o
    # arquivo em todo pacote do jogo que o tem (ou entra como arquivo novo no PATCH); escopo PACOTE = pacote do jogo
    # editado, que so troca dentro do pacote de mesmo nome, como no shadPS4 (cada pacote tem o seu
    # stuff\ttbmetadata.xml: o do PATCH no GAME derruba o jogo). O primeiro mod (ordem alfabetica) que trouxer um
    # nome vence. Os text.csv de pacote proprio / soltos sao juntados ao do jogo (texts), nao trocados.
    wanted, texts = {}, []
    ignored = {}

    def take(scope, order, label, name, data, usize, packed, folder):
        upper = name.upper()
        if upper in ignored.setdefault(folder, read_ignore(folder)):
            return
        if (scope, upper) in wanted:
            return
        if not scope and upper in WHOLE_GAME_FILES:
            texts.append((label, data) if not packed else None)
            return
        if scope and upper in BUILD_INFO:
            say('AVISO ' + T('%s: %s do mod ignorado (descreve a versao do jogo de quem fez o mod; trocar derruba o jogo)' % (label, name),
                             '%s: the mod\'s %s was skipped (it describes the mod author\'s game version; replacing it crashes the game)' % (label, name)))
            return
        wanted[(scope, upper)] = (order, label, name, data, usize, packed)
    anomaly = []  # (rotulo, nome no /app0/mods, .HDR, .DAT)
    for source, label, dat, folder in mod_files:
        stem = os.path.splitext(os.path.basename(dat))[0]
        if loader == 'anomaly' and not (GAME_PACK.match(stem) and stem.upper() in packs):
            # Fase 67: pacote proprio (AllWorld, BARTVOICE...) vai inteiro para o Anomaly, na frente dos do jogo
            hdr = next((os.path.join(os.path.dirname(dat), f) for f in os.listdir(os.path.dirname(dat))
                        if f.lower() == (stem + '.hdr').lower()), None)
            taken = {a[1].lower() for a in anomaly}
            name, n = stem, 2
            while name.lower() in taken:
                name, n = '%s_%d' % (stem, n), n + 1
            if hdr:
                rebuilt = rebuild_mod_pack(label, name, dat, cache, tocs, packs, all_names)
                if rebuilt:
                    hdr, dat = rebuilt
                anomaly.append((label, name, hdr, dat))
            continue
        try:
            rows = nu2toc.read_toc(dat)
        except Exception as e:
            say('AVISO ' + T('%s: tabela do pacote ilegivel (%s)' % (label, e), '%s: unreadable pack table (%s)' % (label, e)))
            continue
        # pacote do jogo inteiro editado (ex. PATCH.DAT de 1 GB com 8 arquivos mudados): so entra o que difere do pacote
        # de mesmo nome do jogo, comparando os bytes guardados
        stem = os.path.splitext(os.path.basename(dat))[0].upper()
        same_pack = packs.get(stem) if GAME_PACK.match(stem) else None
        original = {}
        if same_pack:
            try:
                original = {row[0].upper(): row for row in nu2toc.read_toc(same_pack[1])}
            except Exception:
                same_pack = None
        scope = stem if same_pack else None
        with open(dat, 'rb') as f, open(same_pack[1] if same_pack else dat, 'rb') as g:
            for name, usize, zsize, offset, packed in rows:
                f.seek(offset)
                data = f.read(zsize)
                old = original.get(name.upper())
                if old and old[1] == usize and old[2] == zsize:
                    g.seek(old[3])
                    if g.read(zsize) == data:
                        continue
                if scope and name.upper() not in original:
                    scope_new = None  # arquivo que o pacote do jogo nao tem: entra como os de pacote proprio
                else:
                    scope_new = scope
                take(scope_new, (source, 0), label, name, data, usize, bool(packed), folder)
    for source, label, folder in loose_sources:
        for name, path in loose_files(folder, top_dirs):
            with open(path, 'rb') as f:
                data = f.read()
            take(None, (source, 1), label + T(' (soltos)', ' (loose)'), name, data, len(data), False, folder)
    texts = [t for t in texts if t]
    text_name = '\\stuff\\text\\text.csv'
    if texts and 'PATCH' in tocs and text_name.upper() in tocs['PATCH'][1]:
        current = wanted.get(('PATCH', text_name.upper()))
        if current:
            base = current[3]
        else:
            base = None
            for row in nu2toc.read_toc(packs['PATCH'][1]):
                if row[0].upper() == text_name.upper():
                    with open(packs['PATCH'][1], 'rb') as f:
                        f.seek(row[3])
                        base = f.read(row[2]) if not row[4] else None
        merged, count = merge_text(base, texts) if base else (None, 0)
        if merged:
            wanted[('PATCH', text_name.upper())] = ((-1, 0), T('textos dos mods', 'mod texts'), text_name, merged, len(merged), False)
            say('TEXTOS ' + T('%d linha(s) nova(s) dos mods no text.csv (%s)' % (count, ', '.join(l for l, _ in texts)),
                              '%d new line(s) from the mods in text.csv (%s)' % (count, ', '.join(l for l, _ in texts))))
    enabled_figures = set()
    if loader == 'anomaly' and os.path.isfile(FIGURES_FILE):
        # Fase 67: figura desligada no jogo (ex. Lord Vortech) cuja gravacao .bin esta na pasta de mods
        with open(FIGURES_FILE, encoding='utf-8') as f:
            figures = {v['id']: (k, v) for k, v in json.load(f).items() if not k.startswith('_')}
        def brought_by_mod(char):
            # o mod original da figura (ex. o LORDVORTECH de connorh315: furtividade, iluminacao e cajado proprios) ja
            # traz o charcache dela: ele vale, e a figura feita do doador nao entra
            prefix = '\\CHARCACHE\\%s\\' % char.upper()
            for mod_label, _name, _hdr, dat in anomaly:
                try:
                    if any(row[0].upper().startswith(prefix) for row in nu2toc.read_toc(dat)):
                        return mod_label
                except Exception:
                    pass
            return None
        for source, label, folder in loose_sources:
            for fid in sorted(figure_tags(folder)):
                if fid in figures and fid not in enabled_figures:
                    char, info = figures[fid]
                    original = brought_by_mod(char)
                    made = None if original else hidden_figure_pack(char, info, tocs, packs, all_names, cache, [a[3] for a in anomaly])
                    if original:
                        enabled_figures.add(fid)
                        say('MOD ' + T('%s: a figura vem do mod %s (a feita do %s fica de fora)' % (info['nome'], original, info['doador']),
                                       '%s: the figure comes from the %s mod (the one made from %s is left out)' % (info['nome'], original, info['doador'])))
                    elif made:
                        enabled_figures.add(fid)
                        # na frente dos outros mods: o abilities.txt dele ja traz as linhas deles
                        anomaly.insert(0, (info['nome'], made[0], made[1], made[2]))
                if fid in enabled_figures:
                    enabled_figures.add(folder)
    if loader == 'anomaly' and len(anomaly) > 1:
        joined = merged_mod_texts(anomaly, tocs, packs, cache)
        if joined:
            anomaly.insert(0, joined)
    if loader == 'anomaly':
        # O Anomaly le em camadas os stuff\text\text.csv da lista de colisoes de hash do jogo, e essa lista muda com o
        # numero de arquivos: com mods grandes entra tambem o do GAME (textos originais, em ingles), lido depois do do
        # PATCH (traducao). Toda copia de base fica igual ao text.csv que o jogo usaria sem o Anomaly.
        upper = text_name.upper()
        current = wanted.get(('PATCH', upper))
        effective = current[3] if current else game_file(tocs, packs, upper)
        # as linhas dos mods do Anomaly (ex. legendas do Bart) entram nesse texto unico: nao dependem da lista
        extra_texts = []
        for label, _name, _hdr, dat in anomaly:
            for row_name, data, packed, _usize in read_pack_files(dat):
                if row_name.upper() == upper and not packed:
                    extra_texts.append((label, data))
        if effective and extra_texts:
            merged, count = merge_text(effective, extra_texts)
            if merged:
                effective = merged
                wanted[('PATCH', upper)] = ((-1, 0), T('textos dos mods', 'mod texts'), text_name, merged, len(merged), False)
                say('TEXTOS ' + T('%d linha(s) dos mods do Anomaly no texto do jogo' % count, '%d line(s) from the Anomaly mods in the game text' % count))
        if effective:
            for pack, (_block, positions, _aid, _ver) in tocs.items():
                if pack != 'PATCH' and upper in positions and (pack, upper) not in wanted:
                    wanted[(pack, upper)] = ((-1, 1), T('textos do jogo', 'game texts'), text_name, effective, len(effective), False)

    lines = ['# chave ' + key]
    lines += ['overlay\t%s' % o for o in overlays]
    for label, name, hdr, dat in anomaly:
        lines.append('anomaly\t%s\t%s\t%s' % (name, hdr, dat))
        say('MOD ' + T('%s: pacote inteiro pelo Anomaly (connorh315)' % label, '%s: whole pack through Anomaly (connorh315)' % label))
    found = set()
    new_names = {k for k in wanted if k[0] is None and k[1] not in all_names}
    for pack, (hdr_path, dat_path) in sorted(packs.items()):
        if pack not in tocs:
            continue
        block, positions, archive_id, version = tocs[pack]
        hits = {}  # nome -> chave de wanted (o mod de menor ordem entre os de escopo None e os deste pacote)
        for key_, entry in wanted.items():
            if key_[0] in (None, pack) and key_[1] in positions and (key_[1] not in hits or entry[0] < wanted[hits[key_[1]]][0]):
                hits[key_[1]] = key_
        extra = sorted(new_names) if pack == 'PATCH' and block[4:8] == b'.CC4' else []
        if not hits and not extra:
            continue
        base = os.path.getsize(dat_path)
        tail = bytearray()
        index_rows = []
        cc4 = block[4:8] == b'.CC4'

        def put(data, usize, packed):
            """Poe um arquivo na cauda e devolve a posicao dele. Issue #4: nos pacotes .CC4 o jogo le o metodo de
            compressao nos 24 bytes antes de cada arquivo (como no build_pack); sem eles, o primeiro arquivo da cauda
            pegava o fim do .DAT do jogo como metodo (o text.csv juntado com os textos de um mod saia vazio)."""
            while len(tail) % 16:
                tail.append(0)
            if cc4:
                tail.extend(struct.pack('<IIIIII', len(data), 0, usize, 0, 3 if packed else 0, 0))
            at = len(tail)
            tail.extend(data)
            return at
        for n, key_ in hits.items():
            _order, label, name, data, usize, packed = wanted[key_]
            at = put(data, usize, packed)
            write_entry(block, positions[n], archive_id, version, base + at, len(data), usize, packed)
            index_rows.append((name, usize, len(data), at, 2 if packed else 0))
            found.add(key_)
        if extra:
            items = []
            stored = {}  # id dos bytes -> posicao na cauda (as copias de idioma de uma fala usam os mesmos bytes)
            for key_ in extra:
                _order, label, name, data, usize, packed = wanted[key_]
                if id(data) not in stored:
                    stored[id(data)] = put(data, usize, packed)
                items.append((name, base + stored[id(data)], len(data), usize, packed))
                index_rows.append((name, usize, len(data), stored[id(data)], 2 if packed else 0))
            try:
                block, added = add_files_cc4(block, items)
                found.update((None, n.upper()) for n in added)
            except ValueError as e:
                say('AVISO ' + T('arquivos novos dos mods nao entraram no PATCH (%s)' % e, 'the mods\' new files were not added to PATCH (%s)' % e))
        cache_hdr = os.path.join(cache, pack + '.HDR')
        cache_tail = os.path.join(cache, pack + '.tail')
        with open(cache_hdr, 'wb') as f:
            f.write(block)
        with open(cache_tail, 'wb') as f:
            f.write(tail)
        try:  # indice do radar (so para o log do port): falhar aqui nao impede os mods
            os.makedirs(os.path.join(root, 'assets', 'dat_index'), exist_ok=True)
            nu2toc.write_tsv(index_rows, os.path.join(root, 'assets', 'dat_index', pack + '_MODS.tsv'))
        except OSError:
            pass
        lines.append('pacote\t%s\t%s\t%s\t%s\t%d\t%s' % (pack, hdr_path, cache_hdr, dat_path, base, cache_tail))
    per_mod = {}  # rotulo -> [trocados, novos, de fora]
    for label in [m[1] for m in mod_files if m[1] not in {a[0] for a in anomaly}]:
        per_mod[label] = [0, 0, 0]
    for key_, (_order, label, *_rest) in wanted.items():
        if _order[0] < 0:
            continue
        hit = per_mod.setdefault(label, [0, 0, 0])
        hit[(1 if key_ in new_names else 0) if key_ in found else 2] += 1
    for label, (ok, new, out) in per_mod.items():
        extra_text = T(', %d novo(s)' % new, ', %d new' % new) if new else ''
        say('MOD ' + T('%s: %d arquivo(s) do jogo trocado(s)%s' % (label, ok, extra_text), '%s: %d game file(s) replaced%s' % (label, ok, extra_text)))
        if out:
            say('AVISO ' + T('%s: %d arquivo(s) ficaram de fora (nome repetido ou o jogo nao tem o pacote)' % (label, out),
                             '%s: %d file(s) were left out (repeated name or the game lacks the pack)' % (label, out)))
    for source, label, folder in loose_sources:
        has_loose = any(k for k, v in wanted.items() if v[1].startswith(label + ' ('))
        if not has_loose and not any(m[0] == source for m in mod_files) and folder not in enabled_figures:
            not_a_mod(label, folder)
    with open(manifest, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    say('PRONTO %d' % len(sources))
    return 0


def unsafe(name, dest):
    """Caminho de dentro do arquivo que sairia da pasta do mod (absoluto, com unidade ou com ..)."""
    target = os.path.normpath(os.path.join(dest, name))
    return os.path.isabs(name) or ':' in name or not (target + os.sep).startswith(os.path.normpath(dest) + os.sep)


def run(cmd):
    import subprocess
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', stdin=subprocess.DEVNULL,
                          creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))


def last_line(result):
    text = result.stderr.strip() or result.stdout.strip()
    return text.splitlines()[-1] if text else '?'


def extractors(archive):
    """(programa, listar, nomes da listagem, extrair em dest), na ordem de tentativa: o 7-Zip que vem com o port
    (tools\\7zip), o 7-Zip / WinRAR instalados e o tar.exe do Windows (libarchive: nao abre RAR v6 / dicionario grande)."""
    import shutil
    sevens, unrars = [os.path.join(HERE, '..', '7zip', '7z.exe')], []
    try:
        import winreg
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for key, value, out, exe in ((r'SOFTWARE\7-Zip', 'Path64', sevens, '7z.exe'), (r'SOFTWARE\7-Zip', 'Path', sevens, '7z.exe'),
                                         (r'SOFTWARE\WinRAR', 'exe64', unrars, None), (r'SOFTWARE\WinRAR', 'exe32', unrars, None)):
                try:
                    with winreg.OpenKey(hive, key) as k:
                        path = winreg.QueryValueEx(k, value)[0]
                    out.append(os.path.join(path, exe) if exe else os.path.join(os.path.dirname(path), 'UnRAR.exe'))
                except OSError:
                    pass
    except ImportError:
        pass
    for pf in (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)')):
        if pf:
            sevens.append(os.path.join(pf, '7-Zip', '7z.exe'))
            unrars.append(os.path.join(pf, 'WinRAR', 'UnRAR.exe'))
    sevens.append(shutil.which('7z') or '')
    unrars.append(shutil.which('unrar') or '')
    seen, found = set(), []

    def add(exe, *rest):
        if exe and os.path.isfile(exe) and os.path.normcase(os.path.abspath(exe)) not in seen:
            seen.add(os.path.normcase(os.path.abspath(exe)))
            found.append((exe,) + rest)
    for exe in sevens:
        add(exe, [exe, 'l', '-ba', '-slt', '-sccUTF-8', archive],
            lambda out: [line[7:] for line in out.splitlines() if line.startswith('Path = ')],
            lambda dest, exe=exe: [exe, 'x', '-y', '-bso0', '-bsp0', '-o' + dest, archive])
    for exe in unrars:
        add(exe, [exe, 'lb', '-p-', archive], lambda out: [line for line in out.splitlines() if line],
            lambda dest, exe=exe: [exe, 'x', '-y', '-p-', '-idq', archive, dest + os.sep])
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    add(tar, [tar, '-tf', archive], lambda out: [line for line in out.splitlines() if line],
        lambda dest: [tar, '-xf', archive, '-C', dest])
    return found


def install(archive, mods_dir):
    """Extrai um mod (.zip, .rar ou .7z) em mods\\<nome do arquivo>; recusa caminhos que sairiam da pasta.
    .zip pelo zipfile; o resto (e .zip que o zipfile nao abre) pelos programas de extractors()."""
    import shutil
    import zipfile
    base = os.path.basename(archive)
    name = re.sub(r'[<>:"/\\|?*]', '_', os.path.splitext(base)[0]).strip(' .') or 'mod'
    dest, n = os.path.join(mods_dir, name), 2
    while os.path.exists(dest):
        dest = os.path.join(mods_dir, '%s (%d)' % (name, n))
        n += 1

    def refuse(bad):
        say('ERRO ' + T('o arquivo tem um caminho perigoso (%s): nada foi instalado' % bad,
                        'the archive has an unsafe path (%s): nothing was installed' % bad))
        return 1
    done, errors = False, []
    if zipfile.is_zipfile(archive):
        try:
            with zipfile.ZipFile(archive) as z:
                bad = next((i.filename for i in z.infolist() if unsafe(i.filename, dest)), None)
                if bad:
                    return refuse(bad)
                os.makedirs(dest)
                z.extractall(dest)
            done = True
        except (NotImplementedError, RuntimeError, zipfile.BadZipFile) as e:  # metodo que o zipfile nao tem, ou senha
            shutil.rmtree(dest, ignore_errors=True)
            errors.append('zipfile: %s' % e)
    for exe, list_cmd, names, extract in ([] if done else extractors(archive)):
        tool = os.path.basename(exe)
        listing = run(list_cmd)
        found = names(listing.stdout) if listing.returncode == 0 else []
        if not found:
            errors.append('%s: %s' % (tool, last_line(listing)))
            continue
        bad = next((f for f in found if unsafe(f, dest)), None)
        if bad:
            return refuse(bad)
        os.makedirs(dest)
        result = run(extract(dest))
        if result.returncode == 0:
            done = True
            break
        shutil.rmtree(dest, ignore_errors=True)
        errors.append('%s: %s' % (tool, last_line(result)))
    if not done:
        say('ERRO ' + T('nao consegui abrir %s (%s). Extraia com o 7-Zip / WinRAR e ponha a pasta em mods\\' % (base, '; '.join(errors)),
                        'could not open %s (%s). Extract it with 7-Zip / WinRAR and put the folder in mods\\' % (base, '; '.join(errors))))
        return 1
    if not overlay_root(dest) and figure_dump(dest) and os.path.isfile(FIGURES_FILE):
        # Fase 67: a gravacao de uma figura desligada no jogo (ex. Lord Vortech) fica na pasta de mods: e ela que faz o
        # port liberar a figura
        with open(FIGURES_FILE, encoding='utf-8') as f:
            hidden = {v['id']: v['nome'] for k, v in json.load(f).items() if not k.startswith('_')}
        names = [hidden[i] for i in sorted(figure_tags(dest)) if i in hidden]
        if names:
            say('AVISO ' + T('%s: gravacao de %s instalada. A figura e liberada ao clicar JOGAR; depois abra o .bin no app do '
                              'portal (no LegoToypad: pasta Special\\<nome>\\ ao lado do app)' % (base, ', '.join(names)),
                              '%s: %s figure dump installed. The figure is enabled when you click PLAY; then open the .bin in '
                              'the portal app (LegoToypad: Special\\<name>\\ folder next to the app)' % (base, ', '.join(names))))
            say('INSTALADO ' + os.path.basename(dest))
            return 0
    if not overlay_root(dest) and figure_dump(dest):
        shutil.rmtree(dest, ignore_errors=True)
        say('ERRO ' + T('%s e a gravacao de uma figura (tag .bin do Toy Pad), nao um mod: nada foi instalado. Abra o .bin no app '
                        'do portal (LegoToypad)' % base,
                        '%s is a figure dump (Toy Pad .bin tag), not a mod: nothing was installed. Open the .bin in the portal '
                        'app (LegoToypad)' % base))
        return 1
    if not overlay_root(dest) and not find_packs(dest):
        say('AVISO ' + T('%s: nao tem pacote .HDR/.DAT nem pastas data\\ ou sce_sys\\; confira se e um mod de PS4' % os.path.basename(dest),
                         '%s: no .HDR/.DAT pack and no data\\ or sce_sys\\ folders; check that it is a PS4 mod' % os.path.basename(dest)))
    say('INSTALADO ' + os.path.basename(dest))
    return 0


def main():
    global LANG
    parser = argparse.ArgumentParser(description='LEGO Dimensions PC: mods')
    parser.add_argument('--instalar', help='extract a mod (.zip, .rar or .7z) into mods folder')
    parser.add_argument('--root')
    parser.add_argument('--app0')
    parser.add_argument('--patch', default='')
    parser.add_argument('--lingua', default='en')
    parser.add_argument('--mods', help='mods folder (default: <root>/mods)')
    parser.add_argument('--cache', help='saida (padrao: <root>/assets/mods_cache)')
    parser.add_argument('--addcont', default='', help='DLC folder (one subfolder per DLC)')
    parser.add_argument('--carregador', default='anomaly', choices=('anomaly', 'port'),
                        help='named mod packs: Anomaly (connorh315, default) or port loader')
    args = parser.parse_args()
    LANG = 'en' if args.lingua.lower().startswith('en') else 'pt'
    try:
        if args.instalar:
            mods_dir = args.mods or os.path.join(args.root or '.', 'mods')
            os.makedirs(mods_dir, exist_ok=True)
            return install(os.path.abspath(args.instalar), os.path.abspath(mods_dir))
        if not args.root or not args.app0:
            parser.error('--root e --app0 sao necessarios')
        root = os.path.abspath(args.root)
        return prepare(root, os.path.abspath(args.app0), os.path.abspath(args.patch) if args.patch else '',
                       os.path.abspath(args.mods or os.path.join(root, 'mods')), os.path.abspath(args.cache or os.path.join(root, 'assets', 'mods_cache')),
                       os.path.abspath(args.addcont) if args.addcont else '', args.carregador)
    except OSError as e:
        say('ERRO %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main())
