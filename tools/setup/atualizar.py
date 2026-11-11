#!/usr/bin/env python3
"""Fase 61: atualizacao do port pelo launcher, a partir das releases do GitHub.

  atualizar.py --check            consulta a ultima release; imprime "VERSAO_NOVA <nova> <atual>" (codigo 0)
                                  ou "SEM_ATUALIZACAO <atual>" (codigo 2), mais linhas "NOTA <texto>"
  atualizar.py --apply            baixa o zip portatil da ultima release e aplica por cima desta pasta

So entram os arquivos do port (os mesmos do zip). Ficam como estao: o que a preparacao gerou (eboot,
shaders AOT, pares), os saves, launcher.ini e assets/portal.json. Os arquivos que o port aprende jogando
(pipeline_states.txt e pair_warm.tsv) ganham as linhas novas sem perder as que ja tinham. Um arquivo em
uso (o proprio launcher, a DLL do Python) e renomeado para <nome>.antigo e o launcher apaga na proxima vez.

Saida para o launcher: "PROGRESSO <0-100> <texto>", "ERRO <texto>", "ATUALIZADO <versao>"; o resto e log.
"""
import argparse
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
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
REPO = os.environ.get('LD_REPO', 'vascon-es/lego-dimensions-pc')
API = 'https://api.github.com/repos/%s/releases/latest' % REPO
ASSET_SUFFIX = '_portatil.zip'
KEEP_IF_PRESENT = {'assets/portal.json'}
MERGE_LINES = {'assets/shaders_spv/pipeline_states.txt', 'assets/shaders_spv/pair_warm.tsv'}
REQUIRED = {'LegoDimensionsLauncher.exe', 'build/lego_dimensions.exe'}


def _portuguese():
    return os.environ.get('LD_LANG', 'en').lower().startswith('pt')


PT = _portuguese()
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass


def T(pt, en):
    return pt if PT else en


def progress(pct, text):
    print('PROGRESSO %d %s' % (pct, text), flush=True)


def fail(text):
    print('ERRO ' + text, flush=True)
    sys.exit(1)


def version_tuple(text):
    return tuple(int(n) for n in re.findall(r'\d+', text or '')) or (0,)


def current_version(root):
    try:
        with open(os.path.join(root, 'version.txt'), encoding='utf-8') as f:
            return f.read().strip()
    except OSError:
        return '0'


def request(url):
    return urllib.request.Request(url, headers={'User-Agent': 'LegoDimensionsPC-updater', 'Accept': 'application/vnd.github+json'})


def latest_release():
    try:
        with urllib.request.urlopen(request(API), timeout=30) as r:
            release = json.load(r)
    except Exception as e:
        fail(T('nao consegui falar com o GitHub (%s). Confira a internet e tente de novo.' % e,
               'could not reach GitHub (%s). Check the internet connection and try again.' % e))
    asset = next((a for a in release.get('assets', []) if a.get('name', '').endswith(ASSET_SUFFIX)), None)
    if not asset:
        asset = next((a for a in release.get('assets', []) if a.get('name', '').endswith('.zip') and 'portatil' in a.get('name', '').lower()), None)
    if not asset:
        fail(T('a ultima release (%s) nao tem o zip portatil.' % release.get('tag_name'),
               'the latest release (%s) has no portable zip.' % release.get('tag_name')))
    return release, asset


def notes(release):
    """Os itens da secao de novidades ("What's new" / "Novidades"); sem ela, o comeco do texto."""
    sections, title = {}, ''
    for line in (release.get('body') or '').splitlines():
        line = line.strip()
        if line.startswith('#'):
            title = line.lstrip('#').strip().lower()
            continue
        line = line.lstrip('-*> ').replace('**', '').replace('`', '').strip()
        if line:
            sections.setdefault(title, []).append(line if len(line) <= 160 else line[:157].rstrip() + '...')
    news = next((lines for title, lines in sections.items() if 'new' in title or 'novidade' in title), None)
    return (news or next(iter(sections.values()), []))[:6]


def download(asset, target):
    total = asset.get('size') or 0
    digest = hashlib.sha256()
    got = 0
    with urllib.request.urlopen(request(asset['browser_download_url']), timeout=60) as r, open(target, 'wb') as out:
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            out.write(block)
            digest.update(block)
            got += len(block)
            if total:
                progress(5 + 75 * got // total, T('Baixando a atualizacao... %d de %d MB' % (got >> 20, total >> 20),
                                                  'Downloading the update... %d of %d MB' % (got >> 20, total >> 20)))
    expected = (asset.get('digest') or '').lower()
    if expected.startswith('sha256:') and expected[7:] != digest.hexdigest():
        fail(T('o arquivo baixado esta corrompido (SHA-256 diferente). Tente de novo.',
               'the downloaded file is corrupt (SHA-256 mismatch). Try again.'))
    if total and got != total:
        fail(T('download incompleto (%d de %d bytes).' % (got, total), 'incomplete download (%d of %d bytes).' % (got, total)))


def write_file(path, data):
    """Grava trocando o arquivo de uma vez; se estiver em uso, renomeia o antigo para .antigo."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + '.novo'
    with open(temp, 'wb') as f:
        f.write(data)
    try:
        os.replace(temp, path)
        return
    except PermissionError:
        pass
    old = path + '.antigo'
    n = 1
    while os.path.exists(old):
        try:
            os.remove(old)
        except OSError:
            old = '%s.antigo%d' % (path, n)
            n += 1
    os.rename(path, old)  # um .exe / .dll aberto pode ser renomeado, mas nao sobrescrito
    os.replace(temp, path)


def merge_lines(path, data):
    """Junta as linhas novas no arquivo que o port ja aprendeu, sem tirar nenhuma."""
    with open(path, 'rb') as f:
        mine = f.read()
    have = set(mine.splitlines())
    extra = [line for line in data.splitlines() if line and line not in have]
    if not extra:
        return False
    if mine and not mine.endswith(b'\n'):
        mine += b'\n'
    write_file(path, mine + b'\n'.join(extra) + b'\n')
    return True


def apply(zip_path, root):
    with zipfile.ZipFile(zip_path) as z:
        entries = []
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = info.filename.replace('\\', '/').split('/')
            rel = '/'.join(parts[1:])  # tira "LEGO Dimensions PC/"
            if not rel or '..' in parts or info.filename.startswith('/') or ':' in info.filename:
                continue
            entries.append((rel, info))
        names = {rel for rel, _ in entries}
        missing = REQUIRED - names
        if missing:
            fail(T('o zip da release nao parece o port (falta %s).' % ', '.join(sorted(missing)),
                   'the release zip does not look like the port (missing %s).' % ', '.join(sorted(missing))))
        changed = kept = merged = 0
        for i, (rel, info) in enumerate(entries):
            path = os.path.join(root, *rel.split('/'))
            if i % 25 == 0:
                progress(80 + 19 * i // len(entries), T('Aplicando a atualizacao...', 'Applying the update...'))
            if rel.endswith('/.manter'):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                continue
            exists = os.path.isfile(path)
            if exists and rel in KEEP_IF_PRESENT:
                kept += 1
                continue
            data = z.read(info)
            if exists and rel in MERGE_LINES:
                merged += merge_lines(path, data)
                continue
            if exists and os.path.getsize(path) == len(data):
                with open(path, 'rb') as f:
                    if f.read() == data:
                        continue
            write_file(path, data)
            changed += 1
        # Remove stale Python 3.11 single-file pathlib.py if package tools/python/Lib/pathlib/ is present
        if 'tools/python/Lib/pathlib/__init__.py' in names:
            stale_pathlib = os.path.join(root, 'tools', 'python', 'Lib', 'pathlib.py')
            if os.path.isfile(stale_pathlib):
                try:
                    os.remove(stale_pathlib)
                except OSError:
                    pass
    print(T('arquivos trocados: %d; mantidos: %d; aprendidos com linhas novas: %d' % (changed, kept, merged),
            'files replaced: %d; kept: %d; learned files with new lines: %d' % (changed, kept, merged)), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--root', default=ROOT, help='port folder (tests)')
    args = parser.parse_args()
    current = current_version(args.root)
    progress(2, T('Procurando atualizacoes no GitHub...', 'Looking for updates on GitHub...'))
    release, asset = latest_release()
    latest = release.get('tag_name', '').lstrip('vV')
    if version_tuple(latest) <= version_tuple(current):
        print('SEM_ATUALIZACAO %s' % current, flush=True)
        progress(100, T('Voce ja tem a versao mais nova (%s).' % current, 'You already have the latest version (%s).' % current))
        return 2
    if args.check:
        print('VERSAO_NOVA %s %s' % (latest, current), flush=True)
        for line in notes(release):
            print('NOTA ' + line, flush=True)
        progress(100, T('Versao %s disponivel.' % latest, 'Version %s available.' % latest))
        return 0
    if not args.apply:
        return 0
    folder = tempfile.mkdtemp(prefix='ldpc_update_')
    zip_path = os.path.join(folder, asset['name'])
    try:
        download(asset, zip_path)
        apply(zip_path, args.root)
        write_file(os.path.join(args.root, 'version.txt'), (latest + '\n').encode())  # mesmo se o zip nao trouxer
    finally:
        try:
            os.remove(zip_path)
            os.rmdir(folder)
        except OSError:
            pass
    progress(100, T('Atualizado para a versao %s.' % latest, 'Updated to version %s.' % latest))
    print('ATUALIZADO %s' % latest, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
