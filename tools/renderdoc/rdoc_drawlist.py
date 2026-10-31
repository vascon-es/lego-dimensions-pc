# Roda dentro do qrenderdoc (--python). Lista todos os draws de RDOC_FILE: evento, indices, instancias, alvo de cor e
# os shaders (VS/PS). Serve para achar na captura do port o draw que existe na do shadPS4 (ou vice-versa).
import os
import traceback

import renderdoc as rd

PATH = os.environ['RDOC_FILE']
OUT = os.environ.get('RDOC_OUT', os.path.splitext(PATH)[0]) + '_drawlist.txt'
lines = []


def log(text):
    lines.append(str(text))


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(PATH, '', None)
    controller = cap.OpenCapture(rd.ReplayOptions(), None)[1]
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def flat(actions, out):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                out.append(a)
            flat(a.children, out)
        return out

    def size(rid):
        t = textures.get(rid)
        return '%dx%d %s' % (t.width, t.height, t.format.Name()) if t else '-'

    draws = flat(controller.GetRootActions(), [])
    log('%d draws' % len(draws))
    for a in draws:
        controller.SetFrameEvent(a.eventId, False)
        vk = controller.GetVulkanPipelineState()
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        log('eid %6d  idx %7d inst %5d  vs %s ps %s  cor %s (%d alvos) prof %s' % (
            a.eventId, a.numIndices, a.numInstances, vk.vertexShader.resourceId, vk.fragmentShader.resourceId,
            size(outs[0]) if outs else '-', len(outs), size(a.depthOut)))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    log('ERRO\n' + traceback.format_exc())
log('script terminou')
with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
