"""run_convert.py <serialized file or UnityFS bundle> <out file> [--only name1,name2,...] [--report prefix] [--no-val]
Rewrites every (or the named) Switch Shader object of the file as a Vulkan one and writes a patched copy; validates each emitted SPIR-V module with spirv-val (Vulkan 1.0)."""
import sys, os, argparse, subprocess, hashlib, tempfile, collections, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ucommon import *
import sfile, convert, dmsvlib
# F2_SPIRV_VAL: spirv-val (Android NDK shader-tools or the Vulkan SDK); optional -- without it the modules are written but not
# validated (tools/setup_poe.py finds one when there is one, else passes --no-val).
# F2_SHADERS_DMSV: shaders.dmsv, the player's own shaders translated to SPIR-V by tools/poe_unpack.py (nx2android's translator).
SPIRV_VAL = os.environ.get('F2_SPIRV_VAL', '')
DMSV = os.environ.get('F2_SHADERS_DMSV', '')
_val = {}
def spirv_val(spv, tmpdir):
    h = hashlib.sha1(spv).hexdigest()
    if h not in _val:
        fn = os.path.join(tmpdir, h + '.spv'); open(fn, 'wb').write(spv)
        r = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.0', fn], capture_output=True, text=True)
        _val[h] = (r.returncode, (r.stdout + r.stderr).strip()); os.unlink(fn)
    return _val[h]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('src'); ap.add_argument('out'); ap.add_argument('--only', default=''); ap.add_argument('--report'); ap.add_argument('--no-val', action='store_true')
    ap.add_argument('--dump-spv', help='folder that receives the emitted modules'); ap.add_argument('--dry', action='store_true', help='convert and validate, write no file')
    a = ap.parse_args()
    only = set(x for x in a.only.split(',') if x)
    trees = shader_tree(); root = next(iter(trees.values()))
    dm = dmsvlib.Dmsv(DMSV); rep = convert.Report(); collect = []
    d = open(a.src, 'rb').read(); bundle = d[:8] == b'UnityFS\0'
    nodes = unityfs.bundle_files(d) if bundle else [(os.path.basename(a.src), d)]
    newfiles = {}; done = []; failed = []
    for nm, x in nodes:
        if not unityfs.is_serialized(x): continue
        repl = {}
        for cid, pid, o in unityfs.serialized_objects(x, common(), {48}, trees):
            name = o['m_ParsedForm']['m_Name']
            if only and name not in only: continue
            try:
                o2, _ = convert.convert_shader(o, dm, rep, collect)
                repl[pid] = sfile.obj_bytes(root, o2); done.append(name)
            except Exception as e:
                failed.append((name, f'{type(e).__name__}: {e}')); rep.note('FAILED', name, f'{type(e).__name__}: {e}')
        if repl and not a.dry: newfiles[nm] = sfile.replace_objects(x, repl)
    if not a.dry:
        out = sfile.repack_bundle(d, newfiles) if bundle else newfiles.get(nodes[0][0], d)
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True); open(a.out, 'wb').write(out)
    bad = 0; mods = set()
    if not a.no_val and SPIRV_VAL:
        with tempfile.TemporaryDirectory() as td:
            for where, st, spv, keys in collect:
                rc, msg = spirv_val(spv, td); mods.add(hashlib.sha1(spv).hexdigest())
                if rc: bad += 1; rep.note('spirv-val FAILED', where, f'{st} {keys[0 if st == "vs" else 1]:016x}: {msg[:300]}')
    if a.dump_spv:
        os.makedirs(a.dump_spv, exist_ok=True)
        for where, st, spv, keys in collect: open(os.path.join(a.dump_spv, f'{keys[0 if st == "vs" else 1]:016x}_{st}.spv'), 'wb').write(spv)
    print(f'{a.src}: {len(done)} shaders converted, {len(failed)} failed; {len(collect)} stage modules emitted ({len(mods)} distinct), spirv-val failures: {bad}')
    print('counts:', dict(rep.count))
    for n, why in failed[:20]: print('  FAILED', n, why)
    if a.report:
        with open(a.report + '.tsv', 'w', encoding='utf8') as f:
            f.write('kind\twhere\ttext\n')
            for r in rep.rows: f.write('\t'.join(r) + '\n')
    return 1 if failed or bad else 0

if __name__ == '__main__': sys.exit(main())
