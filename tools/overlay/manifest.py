"""manifest.py : overlay/MANIFEST.tsv (relative path, size, sha1, source size, why)"""
import sys, os, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gm import *
WHY = {
 'Data/globalgamemanagers': 'BuildSettings.m_GraphicsAPIs [22]->[21]; GraphicsSettings shader-define entry platform 19->18; PlayerSettings orientation -> landscape autorotation; header platform 13 (tools/data/ggm_patch.py)',
 'Data/unity default resources': "the ANDROID player's own file (AndroidPlayer/Data), unmodified: same 84 objects, Shaders for [GLES2, GLES3, Vulkan], internalskinning with a Vulkan variant. Android looks for it at the Data root",
 'apk_assets/bin/Data/boot.config': 'goes INTO THE APK as assets/bin/Data/boot.config (read through Java AssetManager, not the file layer)',
}
SHADERS = 'Shader objects rewritten to the Vulkan platform (18) with our SPIR-V (f2-shaders/tools/run_convert.py); header platform 13; every other object byte-identical'
def sha1(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
root = os.path.dirname(OVERLAY); rows = []; total = 0   # OVERLAY's parent: Data/ + apk_assets/ (env ASTC_SET_OVERLAY overrides both)
for dp, dn, fn in os.walk(root):
    for n in fn:
        if n == 'MANIFEST.tsv': continue
        p = os.path.join(dp, n); rel = os.path.relpath(p, root).replace(os.sep, '/')
        src = DATA + '/' + rel[5:] if rel.startswith('Data/') else None
        ssz = os.path.getsize(src) if src and os.path.exists(src) else ''
        why = WHY.get(rel) or (SHADERS + ('; UnityFS repacked as one stored block' if 'StreamingAssets' in rel else ''))
        rows.append((rel, os.path.getsize(p), sha1(p), ssz, why)); total += os.path.getsize(p)
with open(os.path.join(root, 'MANIFEST.tsv'), 'w', encoding='utf8', newline='\n') as f:
    f.write('path\tsize\tsha1\tsize_of_the_switch_file\twhy\n')
    for r in sorted(rows): f.write('\t'.join(map(str, r)) + '\n')
print(len(rows), 'files', total, 'bytes', round(total / 2**20, 1), 'MiB')
for r in sorted(rows): print(f'  {r[1]:>11}  {r[0]}')
