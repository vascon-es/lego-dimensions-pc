#!/usr/bin/env python3
"""Fase 49: preparacao do port a partir do dump do PROPRIO usuario (rodada pelo launcher).

Nada do jogo vem junto com o port: este script le o dump (pasta CUSA01176) e as DLCs de quem
instalou e gera, dentro da pasta do port:
  1. assets/decrypted_elfs/eboot.bin.elf  (o eboot.bin do dump e um FSELF sem criptografia: so tira o
     cabecalho, sem chave nenhuma)
  2. assets/shaders_aot/aot_pairs.pak/.idx (shaders do jogo e das DLCs traduzidos para Vulkan antes de
     jogar: extract + plan + compile do nu2_shader_extractor)

Saida para o launcher: linhas "PROGRESSO <0-100> <texto>" e "ERRO <texto>"; o resto e log.
uso: preparar.py --app0 PASTA_DO_JOGO [--addcont PASTA_DAS_DLCS] [--jobs N] [--sem-aot]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
# Fase 78: sobe quando o pacote AOT muda de forma que valha refazer (shaders do eboot, leitura de vertices em lote unico,
# tipos de textura por par, 3D). Gravado em assets/preparado.txt; o launcher compara com o dele (kPackVersion).
PACK_VERSION = 78
sys.path.insert(0, os.path.join(ROOT, 'tools'))
EXTRACTOR = os.path.join(ROOT, 'tools', 'nu2_shader_extractor', 'nu2_shader_extractor.py')
from game_regions import read_sfo, title_id, SUPPORTED_TITLE_IDS


def _portuguese():
    """Fase 60: textos em portugues se o launcher pediu (LD_LANG) ou se o Windows esta em portugues; senao ingles."""
    return os.environ.get('LD_LANG', 'en').lower().startswith('pt')


PT = _portuguese()

# Fase 60: a saida vai por um pipe para o launcher, que le UTF-8. Sem isto o Python usa a pagina ANSI e
# para com UnicodeEncodeError num nome de pasta de DLC com caractere fora dela (ex.: dois-pontos U+FF1A).
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


def game_file(app0, patch, *parts):
    """Fase 58: o arquivo da atualizacao (pasta separada) vence o do jogo base."""
    if patch and os.path.isfile(os.path.join(patch, *parts)):
        return os.path.join(patch, *parts)
    return os.path.join(app0, *parts)


def check_game(app0, patch=''):
    sfo = game_file(app0, patch, 'sce_sys', 'param.sfo')
    if not os.path.isfile(game_file(app0, patch, 'eboot.bin')) or not os.path.isfile(sfo):
        fail(T('a pasta do jogo precisa ter eboot.bin e sce_sys\\param.sfo (dump do PS4 do LEGO Dimensions, CUSA01176 / CUSA00935)',
               'the game folder must contain eboot.bin and sce_sys\\param.sfo (PS4 dump of LEGO Dimensions, CUSA01176 / CUSA00935)'))
    try:
        info = read_sfo(sfo)
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        fail('Invalid game metadata: ' + str(error))
    if info.get('TITLE_ID') not in SUPPORTED_TITLE_IDS:
        fail(T('o dump e de outro jogo ou regiao (%s); este port e do LEGO Dimensions %s',
               'the dump is from another game or region (%s); this port is for LEGO Dimensions %s') % (info.get('TITLE_ID'), ', '.join(SUPPORTED_TITLE_IDS)))
    base_title = title_id(app0)
    if not base_title or (patch and title_id(patch) != base_title):
        fail('The base game and update must have the same supported title ID')
    dats = set()
    for base in [app0] + ([patch] if patch else []):
        if os.path.isdir(os.path.join(base, 'data')):
            dats.update(f.upper() for d in os.listdir(os.path.join(base, 'data')) if d.lower().startswith('chunk')
                        for f in os.listdir(os.path.join(base, 'data', d)) if f.upper().endswith('.DAT'))
    if not dats:
        fail(T('nenhum pacote .DAT em data\\chunk*: o dump esta incompleto', 'no .DAT package in data\\chunk*: the dump is incomplete'))
    print(T('jogo: %s %s, versao %s, %d pacotes .DAT%s', 'game: %s %s, version %s, %d .DAT packages%s')
          % (info.get('TITLE_ID'), info.get('TITLE', ''), info.get('APP_VER', '?'), len(dats),
             (T(' (atualizacao em %s)', ' (update in %s)') % patch) if patch else ''), flush=True)
    if not any(d.startswith('PATCH') for d in dats):
        print(T('AVISO: sem os pacotes PATCH*.DAT da atualizacao. O port foi feito para a versao 01.24: se a atualizacao estiver '
                'numa pasta separada, escolha-a em "Atualizacao" no launcher.',
                'WARNING: the update PATCH*.DAT packages are missing. The port was made for version 01.24: if the update is '
                'in a separate folder, choose it under "Update" in the launcher.'), flush=True)
    elif info.get('APP_VER', '01.24') != '01.24':
        print(T('AVISO: versao %s; o port foi feito e testado com a 01.24', 'WARNING: version %s; the port was made and tested with 01.24')
              % info.get('APP_VER'), flush=True)
    return info


def check_dlcs(addcont):
    if not addcont:
        return 0
    if not os.path.isdir(addcont):
        print(T('AVISO: pasta de DLCs nao encontrada (%s): seguindo sem DLC', 'WARNING: DLC folder not found (%s): continuing without DLC')
              % addcont, flush=True)
        return 0
    n = 0
    for d in sorted(os.listdir(addcont)):
        sfo = os.path.join(addcont, d, 'sce_sys', 'param.sfo')
        if os.path.isfile(sfo):
            info = read_sfo(sfo)
            if str(info.get('CATEGORY', '')).startswith('ac'):
                n += 1
                print('DLC: %s (%s)' % (d, info.get('CONTENT_ID', '')), flush=True)
    print(T('%d DLC(s) encontradas', '%d DLC(s) found') % n, flush=True)
    return n


def convert_eboot(app0, patch=''):
    import check_binaries
    out_dir = os.path.join(ROOT, 'assets', 'decrypted_elfs')
    os.makedirs(out_dir, exist_ok=True)
    target_elf = os.path.join(out_dir, 'eboot.bin.elf')
    src_file = game_file(app0, patch, 'eboot.bin')
    if not os.path.isfile(src_file):
        fail(T('eboot.bin nao encontrado em %s' % src_file, 'eboot.bin not found at %s' % src_file))
    try:
        data = open(src_file, 'rb').read()
    except Exception as err:
        fail(T('nao foi possivel ler %s: %s' % (src_file, err), 'could not read %s: %s' % (src_file, err)))
    if data[:4] == b'\x7fELF':
        elf = data
    else:
        elf, info = check_binaries.self_to_elf(data)
        if elf is None:
            fail(T('eboot.bin criptografado (SELF de varejo): use um dump descriptografado do jogo. ',
                   'encrypted eboot.bin (retail SELF): use a decrypted dump of the game. ') + '; '.join(info.notes))
    try:
        with open(target_elf, 'wb') as f:
            f.write(elf)
    except Exception as err:
        fail(T('nao foi possivel gravar %s (feche o jogo se estiver aberto): %s' % (target_elf, err),
               'could not write %s (close the game if running): %s' % (target_elf, err)))
    print('eboot.bin.elf: %d bytes' % len(elf), flush=True)


def english_line(line):
    """Fase 60: as linhas de resumo do extrator (EXTRAIDO / PLANO / COMPILADO) em ingles no log do launcher."""
    for pt, en in (('EXTRAIDO:', 'EXTRACTED:'), ('PLANO:', 'PLAN:'), ('COMPILADO:', 'COMPILED:'), (' arquivos', ' files'),
                   (' shaders unicos', ' unique shaders'), (' pares', ' pairs'), (' lotes em', ' batches in'), (' em ', ' in '),
                   (' blocos unicos', ' unique blocks'), ('de SPIR-V/texto', 'of SPIR-V/text'), ('no pacote', 'in the pack'),
                   ('lote ', 'batch ')):
        line = line.replace(pt, en)
    return line


def run_step(args, label, start, end):
    """Roda uma etapa do extrator; 'lote N/M' vira progresso entre start e end."""
    proc = subprocess.Popen([sys.executable, '-u', EXTRACTOR] + args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
    for line in proc.stdout:
        line = line.rstrip()
        print(line if PT else english_line(line), flush=True)
        m = re.search(r'(?:lote|batch) (\d+)/(\d+)', line)
        if m and int(m.group(2)):
            progress(start + (end - start) * int(m.group(1)) // int(m.group(2)), '%s (%s/%s)' % (label, m.group(1), m.group(2)))
        m = re.search(r'^\s+(\d+) (?:arquivos|files), ', line)
        if m:
            progress(start + min(end - start - 1, (end - start) * int(m.group(1)) // 14200), label)
    if proc.wait() != 0:
        fail(T('%s falhou (codigo %d)', '%s failed (code %d)') % (label, proc.returncode))


def prewarm(args, start, end):
    """Fase 77: o port cria todos os pipelines das receitas no cache do driver de video (o mesmo exe que o launcher abre:
    o cache da AMD e por caminho do exe). Le "[PREWARM] progress aot A/B F pipelines P/Q": ate o pacote AOT terminar de
    carregar, 40% da faixa; depois, os pipelines. Sem saida por 10 min = travado (fecha). Falha aqui nao impede jogar:
    a tela de carregamento do jogo cria o que faltar."""
    exe = os.path.join(ROOT, 'build', 'lego_dimensions.exe')
    if not os.path.isfile(exe):
        exe = os.path.join(ROOT, 'lego_dimensions.exe')
    if not os.path.isfile(exe):
        print(T('AVISO lego_dimensions.exe nao encontrado: pipelines ficam para a primeira abertura do jogo',
                'WARNING lego_dimensions.exe not found: pipelines are left for the first game start'), flush=True)
        return
    label = T('criando os pipelines Vulkan no cache do driver', 'building the Vulkan pipelines into the driver cache')
    progress(start, label)
    cmd_extra = []
    if args.prever:  # receitas previstas para os pares que ninguem jogou ainda (prever_pipelines.py, segundos)
        guess_cmd = [sys.executable, '-u', os.path.join(ROOT, 'tools', 'setup', 'prever_pipelines.py'), '--root', ROOT, '--app0', args.app0]
        if args.addcont and os.path.isdir(args.addcont):  # Fase 78: os materiais vem dos .DAT do jogo, da atualizacao e das DLCs
            guess_cmd += ['--addcont', args.addcont]
        if args.patch:
            guess_cmd += ['--patch', args.patch]
        guess = subprocess.run(guess_cmd, cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace')
        for line in (guess.stdout + guess.stderr).strip().splitlines()[-3:]:
            print(line, flush=True)
        cmd_extra = ['--prewarm-predicted']
    cmd = [exe, '--prewarm-only', '--app0', args.app0] + cmd_extra
    if args.addcont and os.path.isdir(args.addcont):
        cmd += ['--addcont', args.addcont]
    if args.patch:
        cmd += ['--patch', args.patch]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    last = [time.time()]

    def watchdog():
        while proc.poll() is None:
            if time.time() - last[0] > 600:
                print(T('AVISO o pre-aquecimento parou de responder: fechado', 'WARNING the pre-warm stopped responding: closed'), flush=True)
                proc.kill()
                return
            time.sleep(5)

    threading.Thread(target=watchdog, daemon=True).start()
    shown = -1
    for line in proc.stdout:
        last[0] = time.time()
        line = line.rstrip()
        m = re.match(r'\[PREWARM\] progress aot (\d+)/(\d+) (\d) pipelines (\d+)/(\d+)', line)
        if not m:
            if line.startswith('[PREWARM]') or line.startswith('[LOADING]') or 'ERRO' in line or 'ERROR' in line:
                print(line, flush=True)
            continue
        aot_done, aot_total, aot_finished, done, total = (int(g) for g in m.groups())
        frac = 0.4 * (aot_done / aot_total if aot_total else 0.0) if not aot_finished else 0.4 + 0.6 * (done / total if total else 1.0)
        pct = start + int((end - start) * min(frac, 1.0))
        text = '%s (%d/%d)' % (label, done, total) if aot_finished else label
        if pct != shown or aot_finished:
            shown = pct
            progress(pct, text)
    code = proc.wait()
    if code != 0:
        print(T('AVISO pre-aquecimento terminou com codigo %d: o que faltar e criado na primeira abertura do jogo',
                'WARNING pre-warm ended with code %d: what is missing is built on the first game start') % code, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app0', required=True)
    ap.add_argument('--addcont', default='')
    ap.add_argument('--patch', default='', help='separate update folder (default: auto-detect matching CUSA01176-patch / CUSA00935-patch)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--sem-aot', action='store_true', help='eboot only (shaders deferred to live compiler)')
    ap.add_argument('--limite', type=int, default=0, help='only first N translation batches (test)')
    ap.add_argument('--so-pipelines', action='store_true', help='only build the known pipelines into the GPU driver cache (Phase 77)')
    ap.add_argument('--prever', action='store_true', default=True,
                    help='(default) also predicted pipelines for content nobody played, from the game materials (Phase 78: '
                         '~1.5 min more and about +0.6 GB of RAM in game; halves the first-visit pipeline misses)')
    ap.add_argument('--sem-prever', dest='prever', action='store_false', help='skip the predicted pipelines')
    args = ap.parse_args()
    t0 = time.time()
    progress(1, T('conferindo o jogo', 'checking the game'))
    if not args.patch:  # Fase 58
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'nu2_shader_extractor'))
        import nu2dat
        args.patch = nu2dat.find_patch(args.app0)
    check_game(args.app0, args.patch)
    check_dlcs(args.addcont)
    if args.so_pipelines:
        prewarm(args, 1, 99)
        progress(100, T('pronto em %.0f min', 'done in %.0f min') % ((time.time() - t0) / 60))
        return
    progress(3, T('convertendo o eboot.bin', 'converting eboot.bin'))
    convert_eboot(args.app0, args.patch)
    progress(4, T('montando os icones dos botoes de cada controle', 'building the button icons for each controller'))  # Fase 62
    icons = subprocess.run([sys.executable, '-u', os.path.join(ROOT, 'tools', 'setup', 'botoes.py'), '--app0', args.app0] +
                           (['--patch', args.patch] if args.patch else []), cwd=ROOT, capture_output=True, text=True, encoding='utf-8',
                           errors='replace')
    if icons.returncode != 0:
        lines = (icons.stdout + icons.stderr).strip().splitlines()
        print(T('AVISO os icones de Xbox / Wii U nao foram montados (fica so o do PS4): ',
                'WARNING the Xbox / Wii U icons were not built (only the PS4 ones stay): ') + (lines[-1] if lines else ''), flush=True)
    for d in ('shaders_spv', 'shaders_aot', 'dumped_shaders', 'savedata0', 'temp0', 'download0'):
        os.makedirs(os.path.join(ROOT, 'assets', d), exist_ok=True)
    if not args.sem_aot:
        addcont = ['--addcont', args.addcont] if args.addcont and os.path.isdir(args.addcont) else ['--addcont', '']
        progress(5, T('extraindo os shaders dos pacotes do jogo e das DLCs', 'extracting shaders from the game and DLC packages'))
        run_step(['extract', '--app0', args.app0] + addcont + (['--patch', args.patch] if args.patch else []), T('extraindo shaders', 'extracting shaders'), 5, 15)
        progress(15, T('planejando as variantes', 'planning the variants'))
        run_step(['plan'], T('planejando', 'planning'), 15, 17)
        progress(17, T('traduzindo os shaders para Vulkan (a etapa mais longa)', 'translating the shaders to Vulkan (the longest step)'))
        run_step(['compile', '--jobs', str(args.jobs)] + (['--limit', str(args.limite)] if args.limite else []), T('traduzindo shaders', 'translating shaders'), 17, 85)
        prewarm(args, 85, 99)
    with open(os.path.join(ROOT, 'assets', 'preparado.txt'), 'w', encoding='utf-8') as f:
        # Fase 78: versao do pacote AOT (o launcher pede um Preparar novo a quem tem um pacote mais antigo)
        f.write('app0=%s\npatch=%s\naddcont=%s\naot=%s\npacote=%d\n' % (args.app0, args.patch, args.addcont, 'nao' if args.sem_aot else 'sim',
                                                                       PACK_VERSION))
    progress(100, T('pronto em %.0f min', 'done in %.0f min') % ((time.time() - t0) / 60))


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        import traceback
        traceback.print_exc()
        fail(f'{type(e).__name__}: {e}')
