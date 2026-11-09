"""1.9.1: reflexao dos shaders PSSL (o bloco logo depois do 'OrbShdr' de cada shader nos .ps4_shaders).

Formato achado (little-endian; nomes sao deslocamentos relativos ao proprio campo):
  cabecalho: [n_buffers, n_elementos, ...] (8 palavras)
  buffers (6 palavras): [primeiro_elemento, nome, slot_api, tamanho, tipo (0x6a0416 = constant buffer), n]
  elementos (8 palavras): [nome, fim do membro (deslocamento + 16), indice, tipo, 0, 0, 0, 0]
So o necessario para achar a matriz de camera: o slot de API do g_CameraVS_CB e o deslocamento de vs_viewProj[0].
"""
import struct


def _cstr(data, p):
    end = data.find(b'\0', p)
    if end < 0 or end - p > 200:
        return None
    try:
        return data[p:end].decode('ascii')
    except UnicodeDecodeError:
        return None


def vs_cameras(data, fnv_bytes, stage_vs=1):
    """{hash do VS: (slot_api, deslocamento)} dos VS de um .ps4_shaders (mesma busca de registros de gcn.shaders_in)."""
    import re
    out = {}
    for m in re.finditer(b'Shdr\x07\x00', data):
        p = m.start()
        if data[p - 3:p] == b'Orb' or data[p + 8] != stage_vs:
            continue
        code = p + 16 + struct.unpack_from('<I', data, p + 24)[0]
        if code + 8 > len(data) or data[code:code + 4] != b'\xff\x03\xeb\xbe':
            continue
        info = code + (struct.unpack_from('<I', data, code + 4)[0] + 1) * 8
        if data[info:info + 7] != b'OrbShdr':
            continue
        cam = camera_info(data, info)
        if cam:
            out['%016x' % fnv_bytes(data[code:info + 28])] = cam[:2]
    return out


def camera_info(data, orb):
    """(slot_api, deslocamento de vs_viewProj[0], tamanho do buffer) do g_CameraVS_CB do shader cujo 'OrbShdr' esta em
    `orb`, ou None. Procura os campos de nome que apontam para as strings conhecidas (robusto a pequenas variacoes)."""
    window_end = min(len(data), orb + 0x4000)
    names = data.find(b'g_CameraVS_CB\0', orb, window_end)
    if names < 0:
        return None
    member = data.find(b'g_CameraVS.vs_viewProj[0]\0', names, window_end)
    slot = size = offset = None
    for p in range(orb + 28, names - 4):
        rel = struct.unpack_from('<I', data, p)[0]
        if p + rel == names and p + 16 <= len(data):
            slot, size, kind = struct.unpack_from('<3I', data, p + 4)
            if kind != 0x6a0416:
                slot = None
                continue
        elif member >= 0 and p + rel == member:
            end = struct.unpack_from('<I', data, p + 4)[0]
            offset = end - 16
    if slot is None:
        return None
    if offset is None:
        offset = 0  # a reflexao so lista os membros usados; a estrutura g_CameraVS e a mesma no jogo todo (vs_viewProj em 0)
    return slot, offset, size
