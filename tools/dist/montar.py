#!/usr/bin/env python3
"""Fase 49: monta a distribuicao do port (sem NADA do jogo).

Saida em dist/:
  LEGO Dimensions PC/               pasta montada (a mesma do .zip)
  LEGO_Dimensions_PC_portatil.zip   versao portatil (descompactar e abrir LegoDimensionsLauncher.exe)
  LEGO_Dimensions_PC_Setup.exe      instalador (o mesmo conteudo colado no fim do instalador)

Vai junto: o port, o compilador de shaders ao vivo, o launcher, o runtime do Visual C++ (redistribuivel),
um Python so com a biblioteca padrao (licenca PSF), as ferramentas de preparacao, os indices dos
pacotes (nomes e posicoes dos arquivos), o dicionario de formatos de vertice e os estados de pipeline
ja vistos (so numeros). NAO vai: o dump, as DLCs, o eboot, os modulos do sistema do PS4, os shaders
extraidos, o pacote AOT, os saves - o launcher gera tudo a partir dos arquivos de quem instalou.

uso (da raiz do projeto, depois de build.bat e tools/dist/build_dist.bat):
    python tools/dist/montar.py
"""
import importlib.util
import os
import shutil
import struct
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DIST = os.path.join(ROOT, 'dist')
NAME = 'LEGO Dimensions PC'
STAGE = os.path.join(DIST, NAME)
PYTHON = os.path.dirname(sys.executable)
REDIST = r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT'
VC_DLLS = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']
MAGIC = b'LDPCPAY1'
HOME = os.path.expanduser('~').lower()  # pasta do usuario que monta: nao pode aparecer nos textos distribuidos

# do Python, so o necessario para a preparacao (sem pacotes de terceiros, testes, tkinter, idle...)
PY_SKIP_DIRS = {'site-packages', 'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', '__pycache__', 'lib2to3',
                '__phello__', 'pydoc_data'}
PY_SKIP_FILES = {'_tkinter.pyd', 'tcl86t.dll', 'tk86t.dll', 'tcl90.dll', 'tk90.dll', 'zlib1.dll.bak'}


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, skip_dirs=(), skip_files=(), suffixes=None):
    for base, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        for f in files:
            if f in skip_files or (suffixes and not f.lower().endswith(suffixes)):
                continue
            rel = os.path.relpath(os.path.join(base, f), src)
            copy(os.path.join(base, f), os.path.join(dst, rel))


def keep(path):
    os.makedirs(path, exist_ok=True)
    open(os.path.join(path, '.manter'), 'w').close()  # o zip nao guarda pasta vazia


def stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    S = lambda *p: os.path.join(STAGE, *p)  # noqa: E731
    R = lambda *p: os.path.join(ROOT, *p)  # noqa: E731
    # programas
    copy(R('tools', 'dist', 'build', 'LegoDimensionsLauncher.exe'), S('LegoDimensionsLauncher.exe'))
    copy(R('tools', 'dist', 'build', 'LD_PC_Transfer.exe'), S('LD_PC_Transfer.exe'))  # Fase 63: conversor de saves
    # o port: o build mais novo entre build\ e build_dist\ (este ultimo e compilado por tools/dist/build_jogo_dist.bat
    # quando o build\ esta em uso pelo jogo aberto)
    game = max((R(d, 'lego_dimensions.exe') for d in ('build', 'build_dist') if os.path.exists(R(d, 'lego_dimensions.exe'))), key=os.path.getmtime)
    print('port: ' + game)
    copy(game, S('build', 'lego_dimensions.exe'))
    copy(R('build', 'pkg_extractor.exe'), S('build', 'pkg_extractor.exe'))  # Fase 68: botao Extrair PKG do launcher
    copy(R('tools', 'shader_compiler_tool', 'build', 'shader_compiler_tool.exe'), S('tools', 'shader_compiler_tool', 'build', 'shader_compiler_tool.exe'))
    for d in (S('build'), S('tools', 'shader_compiler_tool', 'build')):
        for dll in VC_DLLS:
            copy(os.path.join(REDIST, dll), os.path.join(d, dll))
    # ferramentas de preparacao
    for f in ('nu2_shader_extractor.py', 'gcn.py', 'nu2dat.py', 'nu2toc.py', 'winpack.py'):  # Fase 60: nu2toc le a tabela do .DAT
        copy(R('tools', 'nu2_shader_extractor', f), S('tools', 'nu2_shader_extractor', f))
    copy(R('tools', 'check_binaries.py'), S('tools', 'check_binaries.py'))
    copy(R('tools', 'setup', 'preparar.py'), S('tools', 'setup', 'preparar.py'))
    copy(R('tools', 'setup', 'atualizar.py'), S('tools', 'setup', 'atualizar.py'))  # Fase 61: botao Atualizar do launcher
    copy(R('tools', 'setup', 'botoes.py'), S('tools', 'setup', 'botoes.py'))  # Fase 62: icones dos botoes de outras plataformas
    copy(R('tools', 'setup', 'ld_transfer.py'), S('tools', 'setup', 'ld_transfer.py'))  # Fase 63: LD PC Transfer
    copy(R('tools', 'setup', 'mods.py'), S('tools', 'setup', 'mods.py'))  # Fase 64: carregador de mods
    # 7-Zip 26.03 (LGPL + restricao do unRAR; License.txt vai junto): abre os .rar v6 / dicionario grande que o tar.exe
    # do Windows nao abre. Copia oficial em tools\7zip da arvore de desenvolvimento, fora do repositorio, como o Python.
    for f in ('7z.exe', '7z.dll', 'License.txt', 'readme.txt'):
        copy(R('tools', '7zip', f), S('tools', '7zip', f))
    keep(S('mods'))
    keep(S('assets', 'buttons'))
    copy(R('tools', 'dist', 'VERSION'), S('version.txt'))  # Fase 61: versao instalada (comparada com a release do GitHub)
    copy_tree(R('tools', 'data', 'dat_index'), S('tools', 'data', 'dat_index'), suffixes=('.tsv',))
    copy(R('tools', 'data', 'layout_dictionary.json'), S('tools', 'data', 'layout_dictionary.json'))
    copy(R('tools', 'data', 'figuras_ocultas.json'), S('tools', 'data', 'figuras_ocultas.json'))  # Fase 67: figuras desligadas no jogo
    copy(R('tools', 'data', 'vs_reais.txt'), S('tools', 'data', 'vs_reais.txt'))  # Fase 51: leituras de vertice reais (so numeros)
    # Python portatil
    for f in ('python.exe', 'pythonw.exe', 'python3.dll', 'python%d%d.dll' % sys.version_info[:2], 'vcruntime140.dll', 'vcruntime140_1.dll',
              'LICENSE.txt'):
        if os.path.exists(os.path.join(PYTHON, f)):
            copy(os.path.join(PYTHON, f), S('tools', 'python', f))
    copy_tree(os.path.join(PYTHON, 'DLLs'), S('tools', 'python', 'DLLs'), skip_files=PY_SKIP_FILES)
    copy_tree(os.path.join(PYTHON, 'Lib'), S('tools', 'python', 'Lib'), skip_dirs=PY_SKIP_DIRS)
    # dados do port (sem nada do jogo)
    copy(R('assets', 'portal.json'), S('assets', 'portal.json'))
    # Fase 71: receitas de pipeline. A base distribuida = a base anterior + o que a pasta de desenvolvimento aprendeu
    # (sem repetidos); o pipeline_states.txt do jogador nao vai no pacote (o port cria o dele, so com o que faltar).
    spec = importlib.util.spec_from_file_location('pipeline_states', R('tools', 'pipeline_states.py'))
    states = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(states)
    base = R('assets', 'shaders_spv', 'pipeline_states_base.txt')
    keys, _ = states.merge([p for p in (base, R('assets', 'shaders_spv', 'pipeline_states.txt')) if os.path.isfile(p)])
    states.write_file(base, keys)
    print('receitas de pipeline: %d chaves na base' % len(keys))
    copy(base, S('assets', 'shaders_spv', 'pipeline_states_base.txt'))
    copy(R('assets', 'shaders_spv', 'pair_warm.tsv'), S('assets', 'shaders_spv', 'pair_warm.tsv'))
    for d in ('shaders_aot', 'decrypted_elfs', 'dumped_shaders', 'savedata0', 'temp0', 'download0', os.path.join('shaders_spv', 'pairs')):
        keep(S('assets', d))
    keep(S('build', 'logs'))
    copy(R('tools', 'dist', 'LEIA-ME.txt'), S('LEIA-ME.txt'))
    copy(R('tools', 'dist', 'README.txt'), S('README.txt'))  # Fase 60: o mesmo em ingles
    # caminhos da maquina de desenvolvimento nos textos trocados pelos mesmos genericos do repositorio
    spec = importlib.util.spec_from_file_location('exportar_repo', R('tools', 'dist', 'exportar_repo.py'))
    exportar_repo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exportar_repo)
    # conferencia: nada do jogo dentro
    for base, _, files in os.walk(STAGE):
        for f in files:
            low = f.lower()
            full = os.path.join(base, f)
            if low.endswith(exportar_repo.TEXT) and not full.startswith(S('tools', 'python')):
                with open(full, 'rb') as fh:
                    data = fh.read()
                text = data.decode('latin-1')
                for a, b in exportar_repo.REPLACE:
                    text = text.replace(a, b)
                if text.encode('latin-1') != data:
                    with open(full, 'wb') as fh:
                        fh.write(text.encode('latin-1'))
                assert HOME not in text.lower() and HOME.replace('\\', '/') not in text.lower(), full
            assert not low.endswith(('.dat', '.dat2', '.hdr', '.hdr2', '.elf', '.prx', '.sprx', '.self', '.pak', '.spv')), f
            assert low != 'eboot.bin', f


def write_zip(path, prefix):
    if os.path.exists(path):
        os.remove(path)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _, files in os.walk(STAGE):
            for f in sorted(files):
                full = os.path.join(base, f)
                rel = os.path.relpath(full, STAGE).replace(os.sep, '/')
                z.write(full, (prefix + '/' + rel) if prefix else rel)
    return os.path.getsize(path)


def main():
    stage()
    files = sum(len(f) for _, _, f in os.walk(STAGE))
    size = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(STAGE) for f in fs)
    print('montado: %d arquivos, %.0f MB em %s' % (files, size / 2 ** 20, STAGE))
    portable = os.path.join(DIST, 'LEGO_Dimensions_PC_portatil.zip')
    print('zip portatil: %.0f MB' % (write_zip(portable, NAME) / 2 ** 20))
    payload = os.path.join(DIST, 'payload.zip')
    write_zip(payload, '')
    setup = os.path.join(DIST, 'LEGO_Dimensions_PC_Setup.exe')
    with open(setup, 'wb') as out:
        with open(os.path.join(ROOT, 'tools', 'dist', 'build', 'installer_stub.exe'), 'rb') as f:
            out.write(f.read())
        size = os.path.getsize(payload)
        with open(payload, 'rb') as f:
            shutil.copyfileobj(f, out, 8 << 20)
        out.write(MAGIC + struct.pack('<Q', size))
    os.remove(payload)
    print('instalador: %.0f MB -> %s' % (os.path.getsize(setup) / 2 ** 20, setup))


if __name__ == '__main__':
    main()
