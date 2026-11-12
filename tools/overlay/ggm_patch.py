"""ggm_patch.py : Switch globalgamemanagers -> overlay/Data/globalgamemanagers for the Android (Vulkan) player.
Every object is parsed with the release tree from the type database (hash == the hash stored in the file) and written back; an unmodified object must
come back byte-identical (checked for all 15 non-script objects) before any edit is applied.
Edits (the only differences from the Switch file, listed in data/ggm_edits.tsv):
  BuildSettings.m_GraphicsAPIs                         [22] (Switch/NVN) -> [21] (Vulkan)          InitializeGfxDevice reads BuildSettings+0x158
  GraphicsSettings.m_ShaderDefinesPerShaderCompiler[]  shaderPlatform 19 -> 18                     InitShaderDefines looks its compiler platform (18) up; the
                                                                                                   shaders are still the Switch-compiled ones, so the engine must keep the Switch defines
  PlayerSettings.defaultScreenOrientation              0 (Portrait) -> 4 (AutoRotation)            + portrait autorotation off -> sensor landscape
  PlayerSettings.allowedAutorotateToPortrait / ...UpsideDown   true -> false
  file header target platform                          38 -> 13
"""
import sys, os, struct, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gm import *
def main():
    src = DATA + '/globalgamemanagers'; dst = os.path.join(OVERLAY, 'globalgamemanagers')
    d = open(src, 'rb').read(); s = sfile.parse_serialized(d); repl = {}; edits = []
    for ent in s['objs']:
        cid, h = s['types'][ent[3]]
        if cid == 114: continue
        t = tree_for(cid, h.hex()); o, used, size = read_obj(d, s, ent, t)
        raw = d[s['doff'] + ent[1]:s['doff'] + ent[1] + ent[2]]
        back = sfile.obj_bytes(t, o, s['le'])
        assert used == size and back == raw, f'round trip failed for class {cid} ({len(back)} vs {len(raw)})'
        def ed(path, new):
            cur = o; keys = path.split('/')
            for k in keys[:-1]: cur = cur[int(k)] if isinstance(cur, list) else cur[k]
            k = keys[-1]; k = int(k) if isinstance(cur, list) else k
            old = cur[k]
            if old != new: cur[k] = new; edits.append((tpk.class_name(cid), path, repr(old), repr(new)))
        if cid == 141:
            assert o['m_GraphicsAPIs'] == [22], o['m_GraphicsAPIs']
            ed('m_GraphicsAPIs', [21])
        elif cid == 30:
            for i, e in enumerate(o['m_ShaderDefinesPerShaderCompiler']):
                if e['shaderPlatform'] == 19: ed(f'm_ShaderDefinesPerShaderCompiler/{i}/shaderPlatform', 18)
        elif cid == 129:
            ed('defaultScreenOrientation', 4)
            ed('allowedAutorotateToPortrait', False); ed('allowedAutorotateToPortraitUpsideDown', False)
            ed('allowedAutorotateToLandscapeRight', True); ed('allowedAutorotateToLandscapeLeft', True)
        new = sfile.obj_bytes(t, o, s['le'])
        if new != raw: repl[ent[0]] = new
    out = sfile.replace_objects(d, repl, platform=13)
    os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(out)
    # re-read: every object parses again with 0 left, untouched objects identical
    s2 = sfile.parse_serialized(out); assert s2['plat'] == 13
    for e1, e2 in zip(s['objs'], s2['objs']):
        cid, h = s['types'][e1[3]]
        a = d[s['doff'] + e1[1]:s['doff'] + e1[1] + e1[2]]; b = out[s2['doff'] + e2[1]:s2['doff'] + e2[1] + e2[2]]
        if e1[0] in repl:
            o2, used, size = read_obj(out, s2, e2, tree_for(cid, h.hex())); assert used == size
        else: assert a == b, e1[0]
    assert externals(out) == externals(d)
    os.makedirs(CACHE, exist_ok=True)
    with open(os.path.join(CACHE, 'ggm_edits.tsv'), 'w', encoding='utf8') as f:
        f.write('class\tfield\told\tnew\n'); f.write('header\ttarget platform\t38\t13\n')
        for e in edits: f.write('\t'.join(e) + '\n')
    print('written', dst, len(out), 'bytes (source', len(d), '); objects rewritten', sorted(repl), '; edits:')
    for e in edits: print('  ', e)
if __name__ == '__main__': main()
