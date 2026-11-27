"""Prepara uma cópia local dos retratos disponíveis no projeto LegoToypad."""
import argparse
import hashlib
import os
import struct
import subprocess
import tempfile
import urllib.request


REVISION = '0dfb8b4678049f9e4dcc35dce6f8013df8408782'
ARCHIVE_SHA256 = '39caab948b1931f89952374b558a3fdc1820c231d22dadbfeafa43ca6f0e7a25'
URL = f'https://codeload.github.com/harrysof/LegoToypad/zip/{REVISION}'
ARCHIVE_LIMIT = 96 * 1024 * 1024
SKIN_DIR = 'skin-harrysof'
SKIN = {
    'wordmark.png': 'Assets/Branding/Lego_Toypad_Wordmark.png',
    'by_harrysof.png': 'Assets/Branding/by_harrysof.png',
    'settings.png': 'Assets/Buttons/Settings_text.png',
    'background.jpg': 'Assets/Wallpapers/Background.jpg',
    'left_upper.png': 'Assets/Pads/default/left_upper.png',
    'Center.png': 'Assets/Pads/default/Center.png',
    'right_upper.png': 'Assets/Pads/default/right_upper.png',
    'left_lower_left.png': 'Assets/Pads/default/left_lower_left.png',
    'left_lower_right.png': 'Assets/Pads/default/left_lower_right.png',
    'right_lower_left.png': 'Assets/Pads/default/right_lower_left.png',
    'right_lower_right.png': 'Assets/Pads/default/right_lower_right.png',
}


def download(path):
    total = 0
    with urllib.request.urlopen(URL, timeout=30) as request, open(path, 'wb') as target:
        while chunk := request.read(128 * 1024):
            total += len(chunk)
            if total > ARCHIVE_LIMIT:
                raise ValueError('O arquivo compactado excede o tamanho esperado.')
            target.write(chunk)


def build(output, archive):
    home = os.path.dirname(os.path.abspath(__file__))
    manifest = os.path.join(home, 'toypad_portraits.tsv')
    with open(manifest, 'rb') as stream:
        contents = stream.read()
    version = hashlib.sha256(contents + REVISION.encode('ascii')).hexdigest()
    index = os.path.join(output, 'catalog-v4.tsv')
    images = os.path.join(output, version)
    skin = os.path.join(output, SKIN_DIR)
    have_skin = all(os.path.isfile(os.path.join(skin, asset)) for asset in SKIN)
    have_portraits = False
    if os.path.isfile(index) and os.path.isdir(images):
        with open(index, encoding='utf-8') as stream:
            have_portraits = stream.readline().strip() == version
    if have_portraits and have_skin:
        return

    os.makedirs(output, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output, prefix='preparando-') as staging:
        if not archive:
            archive = os.path.join(staging, 'LegoToypad.zip')
            download(archive)
        with open(archive, 'rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != ARCHIVE_SHA256:
                raise ValueError('O arquivo compactado difere da revisão fixada do LegoToypad.')
        ready = os.path.join(staging, 'retratos')
        os.mkdir(ready)
        prefix = f'LegoToypad-{REVISION}/'
        rows = [row.split('\t') for row in contents.decode('utf-8').splitlines()]
        selected = os.path.join(staging, 'selecionados.txt')
        wanted = ['LICENSE']
        if not have_portraits:
            wanted += [row[3] for row in rows]
        if not have_skin:
            wanted += list(SKIN.values())
        with open(selected, 'w', encoding='utf-8', newline='\n') as stream:
            for resource in wanted:
                stream.write(prefix + resource + '\n')
        unpacked = os.path.join(staging, 'originais')
        seven_zip = os.path.join(home, '..', '7zip', '7z.exe')
        subprocess.run([seven_zip, 'x', '-y', '-bd', '-spd', '-scsUTF-8', '-o' + unpacked,
                        '-i@' + selected, archive], check=True, stdout=subprocess.DEVNULL)
        if not have_skin:
            os.makedirs(skin, exist_ok=True)
            for asset, resource in SKIN.items():
                os.replace(os.path.join(unpacked, prefix + resource), os.path.join(skin, asset))
        if have_portraits:
            return
        lines = [version]
        for identifier, pt, en, resource, color in rows:
            if not identifier.isascii() or not identifier.isdecimal():
                raise ValueError('Identificador de figura inválido.')
            src = os.path.join(unpacked, prefix + resource)
            with open(src, 'rb') as stream:
                header = stream.read(24)
            if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n' or header[12:16] != b'IHDR':
                raise ValueError(f'Retrato PNG inválido: {identifier}')
            w, h = struct.unpack_from('>II', header, 16)
            if not (0 < w <= 2048 and 0 < h <= 2048):
                raise ValueError(f'Dimensões do retrato inválidas: {identifier}')
            os.rename(src, os.path.join(ready, f'{identifier}.png'))
            lines.append(f'{identifier}\t{pt}\t{en}\t{color}')
        os.rename(os.path.join(unpacked, prefix + 'LICENSE'), os.path.join(ready, 'LICENSE-LegoToypad.txt'))
        with open(os.path.join(ready, 'FONTES.txt'), 'w', encoding='utf-8') as target:
            target.write(f'LegoToypad por harrysof — https://github.com/harrysof/LegoToypad\nRevisão {REVISION}\n'
                         'Agradecimentos a harrysof e João Pedro Favoni.\n')
        try:
            os.rename(ready, images)
        except FileExistsError:
            # Duas instâncias podem terminar juntas. Ambas conferiram a mesma revisão.
            for name in os.listdir(ready):
                os.replace(os.path.join(ready, name), os.path.join(images, name))
        temp = os.path.join(staging, 'catalog.tsv')
        with open(temp, 'w', encoding='utf-8', newline='\n') as target:
            target.write('\n'.join(lines) + '\n')
        os.replace(temp, index)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--archive', help='Usa uma cópia local da mesma revisão do LegoToypad.')
    args = parser.parse_args()
    build(args.output, args.archive)
