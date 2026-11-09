#!/usr/bin/env python3
"""1.9.1: compila os shaders de pos-processamento do port (FSR 1) com o glslc do Vulkan SDK e grava o SPIR-V em
src/render/fsr1_spv.h (o header gerado vai no repositorio: quem compila o port nao precisa do Vulkan SDK).

uso: python tools/shaders/gerar_fsr1.py   (precisa do glslc: Vulkan SDK, variavel VULKAN_SDK)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'src', 'render', 'shaders')
FSR = os.path.join(ROOT, 'externals', 'fidelityfx-fsr1')
OUT = os.path.join(ROOT, 'src', 'render', 'fsr1_spv.h')
SHADERS = [('kFullscreenVert', 'fullscreen.vert'), ('kFsr1EasuFrag', 'fsr1_easu.frag'), ('kFsr1RcasFrag', 'fsr1_rcas.frag'),
           ('kTaaFrag', 'taa.frag'), ('kTemporalInputsFrag', 'temporal_inputs.frag'),
           ('kDownscaleFrag', 'downscale.frag')]


def glslc():
    sdk = os.environ.get('VULKAN_SDK')
    if sdk and os.path.isfile(os.path.join(sdk, 'Bin', 'glslc.exe')):
        return os.path.join(sdk, 'Bin', 'glslc.exe')
    found = shutil.which('glslc')
    if not found:
        sys.exit('glslc nao encontrado (instale o Vulkan SDK)')
    return found


def main():
    tool = glslc()
    lines = ['// GERADO por tools/shaders/gerar_fsr1.py a partir de src/render/shaders/*. Nao editar.',
             '// FSR 1: AMD FidelityFX Super Resolution 1.0, (c) 2021 Advanced Micro Devices, licenca MIT',
             '// (externals/fidelityfx-fsr1/license.txt).',
             '#pragma once', '#include <cstdint>', '']
    with tempfile.TemporaryDirectory() as tmp:
        for name, file in SHADERS:
            out = os.path.join(tmp, file + '.spv')
            subprocess.run([tool, '-O', '--target-env=vulkan1.3', '-I', FSR, '-I', SRC, os.path.join(SRC, file), '-o', out], check=True)
            data = open(out, 'rb').read()
            words = struct.unpack('<%dI' % (len(data) // 4), data)
            lines.append('inline constexpr uint32_t %s[] = {' % name)
            for i in range(0, len(words), 8):
                lines.append('    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
            lines.append('};')
            lines.append('')
            print('%s: %d palavras' % (file, len(words)))
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    print('->', OUT)


if __name__ == '__main__':
    main()
