#!/usr/bin/env python3
"""
Fase 1 - Diagnostico dos binarios do LEGO Dimensions (PS4) para o port nativo de PC.

  1. Classifica cada arquivo (ELF puro / SELF-FSELF Sony / desconhecido) pelos magic bytes.
  2. Converte FSELF -> ELF puro em assets/decrypted_elfs/ (cabecalho SELF do Orbis).
  3. Analisa Program Headers + secao dinamica SCE (PT_SCE_DYNLIBDATA / PT_SCE_PROCPARAM).
  4. Gera FASE1_DIAGNOSTICO.md e FASE1_imports.json na raiz do projeto.

Uso:
    python tools/check_binaries.py            # converte eboot + dependencias (fecho transitivo)
    python tools/check_binaries.py --all      # converte TODOS os FSELF encontrados
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import glob as _glob_compat
for _gsym in ('_StringGlobber', '_PathGlobber', '_no_recurse_symlinks'):
    if not hasattr(_glob_compat, _gsym):
        if _gsym == '_no_recurse_symlinks':
            setattr(_glob_compat, _gsym, lambda *a, **kw: False)
        else:
            class _FallbackGlobber:
                def __init__(self, *a, **kw): pass
                def compile(self, pat, *a, **kw): return lambda p: True
                def selector(self, parts): return lambda p, *a: iter(())
            setattr(_glob_compat, _gsym, _FallbackGlobber)
del _glob_compat
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
OUT_DIR = ASSETS / "decrypted_elfs"
REPORT = ROOT / "FASE1_DIAGNOSTICO.md"
IMPORTS_JSON = ROOT / "FASE1_imports.json"

ELF_MAGIC = b"\x7fELF"
SELF_MAGIC = b"\x4f\x15\x3d\x1d"

# ---------------------------------------------------------------------------
# Estruturas SELF (Orbis)
# ---------------------------------------------------------------------------
SELF_HDR = struct.Struct("<4sBBBBIHHQHHI")  # 0x20 bytes
SELF_SEG = struct.Struct("<QQQQ")  # flags, offset, file_size, mem_size

SEG_ORDERED = 0x1
SEG_ENCRYPTED = 0x2
SEG_SIGNED = 0x4
SEG_COMPRESSED = 0x8
SEG_BLOCKED = 0x800

# ---------------------------------------------------------------------------
# Estruturas ELF64
# ---------------------------------------------------------------------------
ELF_HDR = struct.Struct("<16sHHIQQQIHHHHHH")  # 0x40 bytes
ELF_PHDR = struct.Struct("<IIQQQQQQ")  # 0x38 bytes
ELF_SYM = struct.Struct("<IBBHQQ")  # 0x18 bytes

ET_NAMES = {
    2: "ET_EXEC",
    3: "ET_DYN",
    0xFE00: "ET_SCE_EXEC",
    0xFE0C: "ET_SCE_STUBLIB",
    0xFE10: "ET_SCE_DYNEXEC (PIE)",
    0xFE18: "ET_SCE_DYNAMIC (PRX)",
}

PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000
PT_SCE_PROCPARAM = 0x61000001
PT_SCE_RELRO = 0x61000010
PT_NAMES = {
    0: "PT_NULL",
    1: "PT_LOAD",
    2: "PT_DYNAMIC",
    3: "PT_INTERP",
    4: "PT_NOTE",
    6: "PT_PHDR",
    7: "PT_TLS",
    0x6474E550: "PT_GNU_EH_FRAME",
    0x61000000: "PT_SCE_DYNLIBDATA",
    0x61000001: "PT_SCE_PROCPARAM",
    0x61000002: "PT_SCE_MODULEPARAM",
    0x61000010: "PT_SCE_RELRO",
    0x6FFFFF00: "PT_SCE_COMMENT",
    0x6FFFFF01: "PT_SCE_VERSION",
}

DT_NEEDED = 1
DT_SONAME = 0xE
DT_SCE_FINGERPRINT = 0x61000007
DT_SCE_ORIGINAL_FILENAME = 0x61000009
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F

NID_ID_CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def decode_nid_id(s: str) -> int:
    v = 0
    for c in s:
        v = v * 64 + NID_ID_CHARSET.index(c)
    return v


def flags_str(f: int) -> str:
    return "".join(c if f & b else "-" for c, b in (("R", 4), ("W", 2), ("X", 1)))


# ---------------------------------------------------------------------------
# SELF -> ELF
# ---------------------------------------------------------------------------
@dataclass
class SelfInfo:
    version: int = 0
    mode: int = 0
    attr: int = 0
    key_type: int = 0
    header_size: int = 0
    meta_size: int = 0
    file_size: int = 0
    num_segments: int = 0
    flags: int = 0
    segments: list = field(default_factory=list)
    encrypted: bool = False
    compressed: bool = False
    authid: int | None = None
    program_type: int | None = None
    app_version: int | None = None
    fw_version: int | None = None
    notes: list = field(default_factory=list)


def classify(path: Path) -> str:
    with path.open("rb") as f:
        magic = f.read(4)
    if magic == ELF_MAGIC:
        return "ELF"
    if magic == SELF_MAGIC:
        return "SELF"
    return "DESCONHECIDO"


def parse_self_header(data: bytes) -> tuple[SelfInfo, int]:
    """Retorna (info, offset do cabecalho ELF embutido)."""
    (magic, version, mode, _endian, attr, key_type, header_size, meta_size, file_size,
     num_segments, flags, _pad) = SELF_HDR.unpack_from(data, 0)
    assert magic == SELF_MAGIC
    info = SelfInfo(version, mode, attr, key_type, header_size, meta_size, file_size,
                    num_segments, flags)
    for i in range(num_segments):
        info.segments.append(SELF_SEG.unpack_from(data, SELF_HDR.size + i * SELF_SEG.size))
    info.encrypted = any(s[0] & SEG_ENCRYPTED for s in info.segments)
    info.compressed = any(s[0] & SEG_COMPRESSED for s in info.segments)
    elf_off = SELF_HDR.size + num_segments * SELF_SEG.size
    return info, elf_off


def self_to_elf(data: bytes) -> tuple[bytes | None, SelfInfo]:
    """Extrai o ELF puro de um FSELF. Retorna (None, info) se estiver realmente criptografado."""
    info, elf_off = parse_self_header(data)
    if data[elf_off:elf_off + 4] != ELF_MAGIC:
        info.notes.append("cabecalho ELF embutido nao encontrado apos a tabela de segmentos")
        return None, info

    eh = list(ELF_HDR.unpack_from(data, elf_off))
    e_phoff, e_phentsize, e_phnum = eh[5], eh[9], eh[10]
    phdrs = [ELF_PHDR.unpack_from(data, elf_off + e_phoff + i * e_phentsize) for i in range(e_phnum)]

    # SELF extended info logo apos os program headers (alinhado em 16)
    ex_off = (elf_off + e_phoff + e_phnum * e_phentsize + 15) & ~15
    if ex_off + 0x20 <= len(data):
        info.authid, info.program_type, info.app_version, info.fw_version = struct.unpack_from(
            "<QQQQ", data, ex_off)

    if info.encrypted:
        info.notes.append("segmentos com flag ENCRYPTED: SELF real (retail), nao e possivel "
                          "extrair sem as chaves do console")
        return None, info

    elf_size = max((p[2] + p[5] for p in phdrs if p[5]), default=0)
    elf_size = max(elf_size, e_phoff + e_phnum * e_phentsize)
    out = bytearray(elf_size)

    covered = []  # (inicio, fim) no ELF de saida
    for seg_flags, seg_off, seg_fsz, seg_msz in info.segments:
        if not seg_flags & SEG_BLOCKED:
            continue  # segmento de digests/metadados, nao contem dados do ELF
        idx = (seg_flags >> 20) & 0xFFF
        if idx >= e_phnum:
            info.notes.append(f"segmento aponta para phdr inexistente ({idx})")
            continue
        blob = data[seg_off:seg_off + seg_fsz]
        if seg_flags & SEG_COMPRESSED:
            blob = zlib.decompress(blob)
        p_offset, p_filesz = phdrs[idx][2], phdrs[idx][5]
        blob = blob[:p_filesz]
        out[p_offset:p_offset + len(blob)] = blob
        covered.append((p_offset, p_offset + len(blob)))

    # e_shoff/e_shnum/e_shstrndx do SELF apontam para secoes que nao existem no dump
    eh[6] = 0
    eh[12] = 0
    eh[13] = 0
    ELF_HDR.pack_into(out, 0, *eh)
    out[e_phoff:e_phoff + e_phnum * e_phentsize] = data[elf_off + e_phoff:
                                                        elf_off + e_phoff + e_phnum * e_phentsize]

    for i, p in enumerate(phdrs):
        if not p[5] or p[0] == 6:
            continue
        # PT_SCE_VERSION nao tem segmento SELF: fica cru no fim do arquivo, apos file_size
        if p[0] == 0x6FFFFF01 and info.file_size + p[5] <= len(data):
            out[p[2]:p[2] + p[5]] = data[info.file_size:info.file_size + p[5]]
            covered.append((p[2], p[2] + p[5]))
            continue
        if not any(a <= p[2] and p[2] + p[5] <= b for a, b in covered):
            info.notes.append(f"phdr {i} ({PT_NAMES.get(p[0], hex(p[0]))}) sem dados no SELF "
                              f"(offset {p[2]:#x}, {p[5]:#x} bytes ficaram zerados)")
    return bytes(out), info


# ---------------------------------------------------------------------------
# Analise do ELF
# ---------------------------------------------------------------------------
@dataclass
class ElfInfo:
    e_type: int = 0
    e_entry: int = 0
    phdrs: list = field(default_factory=list)
    base: int = 0
    image_end: int = 0
    needed: list = field(default_factory=list)  # DT_NEEDED (nomes de arquivo .prx)
    modules: dict = field(default_factory=dict)  # id -> (nome, versao)
    import_libs: dict = field(default_factory=dict)  # id -> (nome, versao)
    export_libs: dict = field(default_factory=dict)
    module_info: tuple | None = None
    soname: str | None = None
    original_filename: str | None = None
    fingerprint: str | None = None
    imports: list = field(default_factory=list)  # (nid, lib, modulo, tipo)
    num_exports: int = 0
    exports: list = field(default_factory=list)  # (nid, lib, vaddr, tamanho, tipo)
    procparam: dict | None = None
    notes: list = field(default_factory=list)


def cstr(data: bytes, off: int) -> str:
    end = data.find(b"\0", off)
    return data[off:end].decode("utf-8", "replace")


def analyze_elf(data: bytes) -> ElfInfo:
    info = ElfInfo()
    eh = ELF_HDR.unpack_from(data, 0)
    info.e_type, info.e_entry = eh[1], eh[4]
    e_phoff, e_phentsize, e_phnum = eh[5], eh[9], eh[10]
    info.phdrs = [ELF_PHDR.unpack_from(data, e_phoff + i * e_phentsize) for i in range(e_phnum)]

    loads = [p for p in info.phdrs if p[0] in (PT_LOAD, PT_SCE_RELRO)]
    if loads:
        info.base = min(p[3] for p in loads)
        info.image_end = max(p[3] + p[6] for p in loads)

    dyn = next((p for p in info.phdrs if p[0] == PT_DYNAMIC), None)
    dld = next((p for p in info.phdrs if p[0] == PT_SCE_DYNLIBDATA), None)
    pparam = next((p for p in info.phdrs if p[0] == PT_SCE_PROCPARAM), None)

    if pparam and pparam[5] >= 0x18:
        size, magic, entries, sdk = struct.unpack_from("<Q4sIQ", data, pparam[2])
        info.procparam = {
            "vaddr": pparam[3],
            "size": size,
            "magic": magic.decode("ascii", "replace"),
            "entry_count": entries,
            "sdk_version": sdk & 0xFFFFFFFF,
        }

    if not dyn or not dld:
        info.notes.append("sem PT_DYNAMIC/PT_SCE_DYNLIBDATA (binario estatico ou nao-SCE)")
        return info

    tags = []
    for off in range(dyn[2], dyn[2] + dyn[5], 16):
        tag, val = struct.unpack_from("<qQ", data, off)
        if tag == 0:
            break
        tags.append((tag, val))
    tagmap = dict(tags)  # tags unicas
    strtab = dld[2] + tagmap.get(DT_SCE_STRTAB, 0)

    def name_ver(val: int) -> tuple[int, str, str]:
        name = cstr(data, strtab + (val & 0xFFFFFFFF))
        minor, major = (val >> 32) & 0xFF, (val >> 40) & 0xFF
        return val >> 48, name, f"{major}.{minor}"

    for tag, val in tags:
        if tag == DT_NEEDED:
            info.needed.append(cstr(data, strtab + val))
        elif tag == DT_SONAME:
            info.soname = cstr(data, strtab + val)
        elif tag == DT_SCE_ORIGINAL_FILENAME:
            info.original_filename = cstr(data, strtab + (val & 0xFFFFFFFF))
        elif tag == DT_SCE_FINGERPRINT:
            info.fingerprint = data[dld[2] + val:dld[2] + val + 20].hex()
        elif tag == DT_SCE_MODULE_INFO:
            info.module_info = name_ver(val)
        elif tag == DT_SCE_NEEDED_MODULE:
            i, n, v = name_ver(val)
            info.modules[i] = (n, v)
        elif tag == DT_SCE_IMPORT_LIB:
            i, n, v = name_ver(val)
            info.import_libs[i] = (n, v)
        elif tag == DT_SCE_EXPORT_LIB:
            i, n, v = name_ver(val)
            info.export_libs[i] = (n, v)

    symtab = dld[2] + tagmap.get(DT_SCE_SYMTAB, 0)
    for off in range(symtab, symtab + tagmap.get(DT_SCE_SYMTABSZ, 0), ELF_SYM.size):
        st_name, st_info, _other, st_shndx, _value, _size = ELF_SYM.unpack_from(data, off)
        name = cstr(data, strtab + st_name)
        parts = name.split("#")
        if len(parts) != 3:
            continue
        kind = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 6: "TLS"}.get(st_info & 0xF, str(st_info & 0xF))
        if st_shndx != 0:
            info.num_exports += 1
            try:
                elib = info.export_libs.get(decode_nid_id(parts[1]), ("?", ""))[0]
            except ValueError:
                elib = "?"
            info.exports.append((parts[0], elib, _value, _size, kind))
            continue
        try:
            lib = info.import_libs.get(decode_nid_id(parts[1]), ("?", ""))[0]
            mod = info.modules.get(decode_nid_id(parts[2]), ("?", ""))[0]
        except ValueError:
            lib = mod = "?"
        info.imports.append((parts[0], lib, mod, kind))
    return info


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def find_sys_modules() -> Path | None:
    for cand in (ASSETS / "sys_modules", ROOT / "sys_modules"):
        if cand.is_dir():
            return cand
    return None


def load_elf(path: Path) -> tuple[bytes | None, str, SelfInfo | None]:
    data = path.read_bytes()
    if data[:4] == ELF_MAGIC:
        return data, "ELF", None
    if data[:4] == SELF_MAGIC:
        elf, sinfo = self_to_elf(data)
        return elf, "SELF", sinfo
    return None, "DESCONHECIDO", None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="converte todos os FSELF, nao so as dependencias")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    sys_modules = find_sys_modules()
    eboot = ASSETS / "eboot.bin"
    if not eboot.is_file():
        print(f"ERRO: {eboot} nao encontrado")
        return 1

    # ---- 1. classificacao por magic bytes --------------------------------
    files = [p for p in sorted(ASSETS.rglob("*")) if p.is_file() and OUT_DIR not in p.parents]
    if sys_modules and ASSETS not in sys_modules.parents:
        files += [p for p in sorted(sys_modules.rglob("*")) if p.is_file()]
    kinds = {p: classify(p) for p in files}
    kind_count = Counter(kinds.values())

    # indice de sys_modules: stem minusculo -> caminho  (libkernel.prx <-> libkernel.sprx)
    sysidx = {}
    if sys_modules:
        for p in sys_modules.iterdir():
            if p.is_file():
                sysidx[p.name.rsplit(".", 1)[0].lower()] = p

    def mod_name(needed_name: str) -> str:
        """libSceGnmDriver_padebug.prx -> libSceGnmDriver (variantes de debug do SDK)."""
        stem = needed_name.rsplit(".", 1)[0]
        for suf in ("_padebug", "_debug"):
            if stem.endswith(suf):
                return stem[:-len(suf)]
        return stem

    def resolve(needed_name: str) -> Path | None:
        return sysidx.get(mod_name(needed_name).lower())

    # ---- 2/3. eboot + fecho transitivo de dependencias -------------------
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    analyzed: dict[Path, tuple[ElfInfo | None, str, SelfInfo | None]] = {}
    converted: list[tuple[Path, Path, int]] = []
    failed: list[tuple[Path, str]] = []

    def process(path: Path, analyze: bool = True) -> ElfInfo | None:
        elf, kind, sinfo = load_elf(path)
        einfo = None
        if elf is None:
            if kind == "SELF":
                failed.append((path, "; ".join(sinfo.notes) or "falha na extracao"))
        else:
            if kind == "SELF":
                out = OUT_DIR / (path.name + ".elf")
                out.write_bytes(elf)
                converted.append((path, out, len(elf)))
            if analyze:
                einfo = analyze_elf(elf)
        analyzed[path] = (einfo, kind, sinfo)
        return einfo

    eboot_info = process(eboot)
    eboot_sinfo = analyzed[eboot][2]
    if eboot_info is None:
        print("ERRO: nao foi possivel obter o ELF do eboot.bin:", failed)
        return 1

    direct = {n: resolve(n) for n in eboot_info.needed}
    # fecho transitivo: quem pede cada modulo
    requested_by: dict[str, set[str]] = defaultdict(set)
    queue = [(n, "eboot.bin") for n in eboot_info.needed]
    seen_paths: set[Path] = set()
    missing_transitive: dict[str, set[str]] = defaultdict(set)
    while queue:
        name, parent = queue.pop(0)
        requested_by[name].add(parent)
        path = resolve(name)
        if path is None:
            missing_transitive[name].add(parent)
            continue
        if path in seen_paths:
            continue
        seen_paths.add(path)
        info = process(path)
        if info:
            queue += [(n, name) for n in info.needed]

    if args.all:
        for p, k in kinds.items():
            if k == "SELF" and p not in analyzed:
                process(p, analyze=False)

    # ---- agregados --------------------------------------------------------
    per_lib = Counter((lib, mod) for _nid, lib, mod, _k in eboot_info.imports)
    per_mod = Counter(mod for _nid, _lib, mod, _k in eboot_info.imports)
    mod_to_file = {}  # nome do modulo SCE -> nome DT_NEEDED
    for n in eboot_info.needed:
        mod_to_file[n.rsplit(".", 1)[0]] = n

    # o nome do modulo SCE nem sempre bate com o arquivo (libSceAppContent.prx -> libSceAppContentUtil)
    def nid_count(needed_name: str) -> int:
        path = resolve(needed_name)
        minfo = analyzed.get(path, (None,))[0] if path else None
        if minfo and minfo.module_info:
            return per_mod.get(minfo.module_info[1], 0)
        return per_mod.get(mod_name(needed_name), 0)

    have = [n for n, p in direct.items() if p]
    miss = [n for n, p in direct.items() if not p]
    trans_only = sorted(n for n in requested_by if n not in direct)
    trans_have = [n for n in trans_only if resolve(n)]
    trans_miss = [n for n in trans_only if not resolve(n)]

    # ---- 4. relatorio -----------------------------------------------------
    L: list[str] = []
    w = L.append
    w("# FASE 1 - Diagnostico dos binarios (LEGO Dimensions PS4 -> PC x86-64)")
    w("")
    w(f"Gerado por `tools/check_binaries.py`. Raiz do projeto: `{ROOT}`")
    w("")
    w("## 1. Classificacao por magic bytes")
    w("")
    w(f"- Arquivos verificados: **{len(files)}**")
    for k, c in kind_count.most_common():
        w(f"- {k}: **{c}**")
    if sys_modules:
        loc = "dentro de `assets/`" if ASSETS in sys_modules.parents else "na RAIZ do projeto (nao em `assets/`)"
        w(f"- `sys_modules/` encontrado em `{sys_modules}` ({loc})")
    else:
        w("- **`sys_modules/` nao encontrado**")
    w("")
    others = [(p, k) for p, k in kinds.items() if k != "SELF"]
    if others:
        w("Arquivos que NAO sao SELF:")
        w("")
        for p, k in others:
            w(f"- `{p.relative_to(ROOT)}` -> {k}")
        w("")

    w("## 2. Conversao FSELF -> ELF")
    w("")
    s = eboot_sinfo
    if s:
        w("Cabecalho SELF do `eboot.bin`:")
        w("")
        w(f"- versao/modo/attr: `{s.version:#x}` / `{s.mode:#x}` / `{s.attr:#x}`, key_type `{s.key_type:#x}`")
        w(f"- header_size `{s.header_size:#x}`, meta_size `{s.meta_size:#x}`, segmentos: {s.num_segments}")
        w(f"- file_size no cabecalho: {s.file_size} bytes (arquivo em disco: {eboot.stat().st_size} bytes)")
        w(f"- criptografado: **{'SIM' if s.encrypted else 'nao (fake signed)'}**, comprimido: {'sim' if s.compressed else 'nao'}")
        if s.authid is not None:
            w(f"- authid `{s.authid:#018x}`, program_type `{s.program_type:#x}`, "
              f"app_version `{s.app_version:#x}`, fw_version `{s.fw_version:#x}`")
        w("")
        w("| # | flags | id phdr | blocked | enc | comp | offset | file_size | mem_size |")
        w("|---|-------|---------|---------|-----|------|--------|-----------|----------|")
        for i, (f, o, fs, ms) in enumerate(s.segments):
            w(f"| {i} | `{f:#x}` | {(f >> 20) & 0xFFF} | {'s' if f & SEG_BLOCKED else '-'} | "
              f"{'s' if f & SEG_ENCRYPTED else '-'} | {'s' if f & SEG_COMPRESSED else '-'} | "
              f"`{o:#x}` | `{fs:#x}` | `{ms:#x}` |")
        w("")
        for n in s.notes:
            w(f"- Aviso: {n}")
        if s.notes:
            w("")
    scope = "todos os FSELF" if args.all else "eboot.bin + fecho transitivo de dependencias (use `--all` para converter tudo)"
    w(f"Escopo da conversao: {scope}.")
    w("")
    w(f"- Convertidos: **{len(converted)}** arquivos em `{OUT_DIR.relative_to(ROOT).as_posix()}/` "
      f"({sum(c[2] for c in converted) / 1048576:.1f} MiB)")
    w(f"- Falhas: **{len(failed)}**")
    for p, why in failed:
        w(f"  - `{p.name}`: {why}")
    w("")

    e = eboot_info
    w("## 3. eboot.bin - cabecalhos")
    w("")
    w(f"- Tipo ELF: `{e.e_type:#06x}` ({ET_NAMES.get(e.e_type, 'desconhecido')})")
    w(f"- **Endereco base (menor p_vaddr de PT_LOAD/PT_SCE_RELRO): `{e.base:#x}`**")
    w(f"- Fim da imagem: `{e.image_end:#x}` (tamanho mapeado: {(e.image_end - e.base) / 1048576:.1f} MiB)")
    w(f"- **Entry point (e_entry): `{e.e_entry:#x}`** (offset da base: `{e.e_entry - e.base:#x}`)")
    if e.procparam:
        pp = e.procparam
        w(f"- `.sce_process_param` @ `{pp['vaddr']:#x}`: magic `{pp['magic']}`, size `{pp['size']:#x}`, "
          f"entry_count {pp['entry_count']}, **SDK version `{pp['sdk_version']:#010x}`**")
    if e.module_info:
        w(f"- Module info: `{e.module_info[1]}` v{e.module_info[2]}")
    if e.original_filename:
        w(f"- Nome original: `{e.original_filename}`")
    if e.fingerprint:
        w(f"- Fingerprint: `{e.fingerprint}`")
    w(f"- Simbolos importados (NIDs): **{len(e.imports)}**; exportados: {e.num_exports}")
    w("")
    w("### Program Headers")
    w("")
    w("| # | tipo | flags | offset | vaddr | filesz | memsz | align |")
    w("|---|------|-------|--------|-------|--------|-------|-------|")
    for i, (pt, pf, off, va, _pa, fsz, msz, al) in enumerate(e.phdrs):
        w(f"| {i} | {PT_NAMES.get(pt, hex(pt))} | {flags_str(pf)} | `{off:#x}` | `{va:#x}` | "
          f"`{fsz:#x}` | `{msz:#x}` | `{al:#x}` |")
    w("")

    w("## 4. Bibliotecas importadas pelo eboot.bin")
    w("")
    w(f"DT_NEEDED: **{len(e.needed)}** modulos. Em `sys_modules/`: **{len(have)}**. Faltando: **{len(miss)}**.")
    w("")
    w("| # | DT_NEEDED | em sys_modules? | arquivo | NIDs importados |")
    w("|---|-----------|-----------------|---------|-----------------|")
    for i, n in enumerate(e.needed, 1):
        p = direct[n]
        cnt = nid_count(n)
        w(f"| {i} | `{n}` | {'SIM' if p else '**NAO**'} | {('`' + p.name + '`') if p else '-'} | {cnt} |")
    w("")
    w("### Imports por biblioteca (DT_SCE_IMPORT_LIB)")
    w("")
    w("| biblioteca | modulo | versao | NIDs | FUNC | OBJECT |")
    w("|------------|--------|--------|------|------|--------|")
    kinds_per = defaultdict(Counter)
    for _nid, lib, mod, k in e.imports:
        kinds_per[(lib, mod)][k] += 1
    libver = {n: v for n, v in e.import_libs.values()}
    for (lib, mod), c in sorted(per_lib.items(), key=lambda x: (-x[1], x[0])):
        kc = kinds_per[(lib, mod)]
        w(f"| `{lib}` | `{mod}` | {libver.get(lib, '')} | {c} | {kc.get('FUNC', 0)} | {kc.get('OBJECT', 0)} |")
    w("")
    w("### Faltando em sys_modules (alvo obrigatorio de HLE, ou vem do `sce_module/` do jogo)")
    w("")
    if miss:
        for n in miss:
            w(f"- `{n}` - {nid_count(n)} NIDs")
    else:
        w("- Nenhuma: todos os modulos DT_NEEDED do eboot existem em `sys_modules/`.")
    w("")
    w("### Dependencias transitivas (pedidas pelos modulos, nao pelo eboot)")
    w("")
    w(f"Presentes ({len(trans_have)}):")
    w("")
    for n in trans_have:
        w(f"- `{n}` <- {', '.join(sorted(requested_by[n]))}")
    w("")
    w(f"Faltando ({len(trans_miss)}):")
    w("")
    for n in trans_miss:
        w(f"- `{n}` <- {', '.join(sorted(requested_by[n]))}")
    if not trans_miss:
        w("- Nenhuma.")
    w("")
    w("## 5. Arquivos convertidos")
    w("")
    w("| origem | ELF | tamanho |")
    w("|--------|-----|---------|")
    for src, dst, size in converted:
        w(f"| `{src.relative_to(ROOT)}` | `{dst.relative_to(ROOT)}` | {size} |")
    w("")
    mod_notes = [(p, i.notes + (s.notes if s else [])) for p, (i, _k, s) in analyzed.items() if i]
    mod_notes = [(p, n) for p, n in mod_notes if n and p != eboot]
    if mod_notes:
        w("## 6. Avisos por modulo")
        w("")
        for p, notes in mod_notes:
            for n in notes:
                w(f"- `{p.name}`: {n}")
        w("")
    ipmi = sorted(requested_by.get("libSceIpmi.prx", ()))
    w("## Observacoes para a Fase 2")
    w("")
    w(f"- O eboot e PIE (`ET_SCE_DYNEXEC`): a base `{e.base:#x}` e de link, nao de execucao. O loader "
      f"escolhe a base real e o entry vira `base + {e.e_entry:#x}`.")
    w("- \"Existe em sys_modules\" nao significa \"da para rodar em LLE\". Modulos que falam com o kernel "
      "do Orbis ou com servicos do sistema precisam de HLE de qualquer jeito (libkernel, libSceGnmDriver, "
      "libSceVideoOut, libSceAudioOut, libScePad, libSceUserService, libSceSystemService, libSceSaveData, "
      "libSceNet*, libSceNp*, dialogs).")
    w(f"- Indicador pratico: {len(ipmi)} modulos do fecho dependem de `libSceIpmi.prx` (IPC com servicos "
      f"do sistema que nao existem no PC): {', '.join('`' + n + '`' for n in ipmi)}.")
    w("- Candidatos reais a LLE (codigo puro de userland): `libc.prx`, `libSceLibcInternal.prx`, "
      "`libSceFios2.prx`, `libSceNgs2.prx`, `libSceXml.prx`, `libSceRtc.prx`.")
    w("- A lista completa de NIDs importados esta em `FASE1_imports.json` (base para os stubs HLE).")
    w("")
    REPORT.write_text("\n".join(L), encoding="utf-8")

    IMPORTS_JSON.write_text(json.dumps({
        "eboot": {
            "type": e.e_type, "base": e.base, "entry": e.e_entry, "image_end": e.image_end,
            "sdk_version": e.procparam["sdk_version"] if e.procparam else None,
        },
        "needed": [{"name": n, "present": bool(direct[n]),
                    "file": direct[n].name if direct[n] else None} for n in e.needed],
        "imports": [{"nid": nid, "library": lib, "module": mod, "type": k}
                    for nid, lib, mod, k in e.imports],
    }, indent=1), encoding="utf-8")

    # ---- resumo no terminal ----------------------------------------------
    print("=" * 72)
    print("FASE 1 - DIAGNOSTICO")
    print("=" * 72)
    print(f"Arquivos verificados : {len(files)}  " + ", ".join(f"{k}={c}" for k, c in kind_count.items()))
    print(f"sys_modules          : {sys_modules}")
    if s:
        print(f"eboot.bin            : SELF, {'CRIPTOGRAFADO' if s.encrypted else 'fake signed (FSELF)'}, "
              f"{s.num_segments} segmentos")
    print(f"Convertidos          : {len(converted)} -> {OUT_DIR}  (falhas: {len(failed)})")
    for p, why in failed:
        print(f"   FALHA {p.name}: {why}")
    print("-" * 72)
    print(f"Tipo ELF             : {e.e_type:#06x} {ET_NAMES.get(e.e_type, '')}")
    print(f"Endereco base        : {e.base:#x}")
    print(f"Entry point          : {e.e_entry:#x}")
    print(f"Tamanho mapeado      : {(e.image_end - e.base) / 1048576:.1f} MiB")
    if e.procparam:
        print(f"SDK version          : {e.procparam['sdk_version']:#010x}")
    print(f"NIDs importados      : {len(e.imports)}")
    print("-" * 72)
    print(f"Modulos DT_NEEDED ({len(e.needed)}):")
    for n in e.needed:
        mark = "OK   " if direct[n] else "FALTA"
        print(f"  [{mark}] {n:<36} {nid_count(n):>5} NIDs")
    print("-" * 72)
    print(f"Presentes em sys_modules : {len(have)}")
    print(f"Faltando (alvo de HLE)   : {len(miss)}  {', '.join(miss)}")
    print(f"Transitivas presentes    : {len(trans_have)}")
    print(f"Transitivas faltando     : {len(trans_miss)}  {', '.join(trans_miss)}")
    if s and s.notes:
        for n in s.notes:
            print("AVISO eboot:", n)
    print("-" * 72)
    print(f"Relatorio : {REPORT}")
    print(f"Imports   : {IMPORTS_JSON}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
