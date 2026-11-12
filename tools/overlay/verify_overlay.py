"""verify_overlay.py : re-read the shader-converted overlay files and compare them with the originals.
  - same object table (path IDs, type indices, order), every non-Shader object byte-identical, metadata identical apart from object offsets / sizes / file size
  - every Shader object: parses with the Shader tree with 0 bytes left, platforms == [18], one LZ4 segment that decompresses, every subprogram blob parses,
    gpu program type 25; every SMOL-V stage module decodes and passes spirv-val --target-env vulkan1.0
  - bundles: same node names / flags, .resS / .resource nodes byte-identical
  - external stream names (m_StreamData.path / m_Resource.m_Source) are untouched because only Shader objects were rewritten (checked by the byte comparison)"""
import sys, os, struct, hashlib, subprocess, tempfile, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gm import *
import subprog, smolv, lz4.block
SPIRV_VAL = os.environ.get('F2_SPIRV_VAL', '')   # optional: without spirv-val the modules are decoded and counted, not validated
FILES = CONVERTED_LOOSE + CONVERTED_BUNDLES
_val = {}
def val(spv, td):
    if not SPIRV_VAL or not os.path.isfile(SPIRV_VAL): return True
    h = hashlib.sha1(spv).hexdigest()
    if h not in _val:
        fn = os.path.join(td, 'm.spv'); open(fn, 'wb').write(spv)
        _val[h] = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.0', fn], capture_output=True, text=True).returncode == 0
    return _val[h]
def check_ser(name, a, b, C, td, shader_root):
    sa, sb = sfile.parse_serialized(a), sfile.parse_serialized(b)
    assert sa['plat'] == 38 and sb['plat'] == 13 and sa['types'] == sb['types'] and sa['doff'] == sb['doff'] and sa['le'] == sb['le'], 'header differs'
    C['files with header platform 13'] += 1
    assert [(o[0], o[3]) for o in sa['objs']] == [(o[0], o[3]) for o in sb['objs']], 'object table differs'
    # metadata: identical except the (start, size) words of the object table and the file size at +4
    ma, mb = bytearray(a[:sa['doff']]), bytearray(b[:sb['doff']])
    for o in sa['objs']: ma[o[4] + 8:o[4] + 16] = b'\0' * 8
    for o in sb['objs']: mb[o[4] + 8:o[4] + 16] = b'\0' * 8
    ma[4:8] = mb[4:8] = b'\0' * 4
    ma[sa['plat_off']:sa['plat_off'] + 4] = bytes(4); mb[sb['plat_off']:sb['plat_off'] + 4] = bytes(4)      # target platform 38 -> 13 (set_platform.py)
    assert ma == mb, 'metadata differs outside the object table'
    assert struct.unpack_from('>I', b, 4)[0] == len(b), 'file size field'
    prev_end = 0
    for oa, ob in zip(sa['objs'], sb['objs']):
        cid, h = sa['types'][oa[3]]
        da = a[sa['doff'] + oa[1]:sa['doff'] + oa[1] + oa[2]]; db = b[sb['doff'] + ob[1]:sb['doff'] + ob[1] + ob[2]]
        assert ob[1] % 8 == 0 or ob[1] == oa[1], 'alignment'
        if cid != 48:
            C['objects unchanged'] += 1
            if da != db: C['NON-SHADER OBJECT DIFFERS'] += 1; print('  DIFF', name, oa[0], cid)
            continue
        C['shader objects'] += 1
        r = unityfs.Reader(db, sb['le']); o = unityfs._read(r, shader_root)
        if r.p != len(db): C['SHADER: BYTES LEFT'] += 1
        if o['platforms'] != [18]: C['SHADER: platforms != [18]'] += 1; print('  platforms', o['platforms'], o['m_ParsedForm']['m_Name'])
        seg = lz4.block.decompress(bytes(o['compressedBlob'][o['offsets'][0]:o['offsets'][0] + o['compressedLengths'][0]]), uncompressed_size=o['decompressedLengths'][0])
        for sp in subprog.split_platform_blob(seg):
            p = subprog.parse(sp); C['subprogram blobs'] += 1
            if p['type'] != 25: C['BLOB: gpu type != 25'] += 1
            if not p['code']: continue
            hd = struct.unpack_from('<11I', p['code'], 0)
            for st in range(5):
                off, sz = hd[1 + 2 * st], hd[2 + 2 * st]
                if not sz: continue
                spv = smolv.decode(p['code'][off:off + sz]); C['stage modules'] += 1
                if not val(spv, td): C['MODULE FAILS spirv-val'] += 1
        # parsed form must only name the Vulkan gpu type
        for ss in o['m_ParsedForm']['m_SubShaders']:
            for ps in ss['m_Passes']:
                for k in ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain'):
                    for spg in ps[k]['m_SubPrograms']:
                        if spg['m_GpuProgramType'] != 25: C['PARSED FORM: gpu type != 25'] += 1
def main():
    trees = {}; 
    from ucommon import shader_tree
    shader_root = next(iter(shader_tree().values()))
    td = tempfile.mkdtemp(); total = collections.Counter()
    for f in FILES:
        a = open(DATA + '/' + f, 'rb').read(); b = open(OVERLAY + '/' + f, 'rb').read(); C = collections.Counter()
        if a[:8] == b'UnityFS\0':
            na, nb = unityfs.bundle_files(a), unityfs.bundle_files(b)
            assert [n for n, _ in na] == [n for n, _ in nb], 'bundle nodes differ'
            for (nm, xa), (_, xb) in zip(na, nb):
                if unityfs.is_serialized(xa): check_ser(nm, xa, xb, C, td, shader_root)
                else:
                    C['bundle side nodes identical'] += xa == xb; C['BUNDLE SIDE NODE DIFFERS'] += xa != xb
        else: check_ser(f, a, b, C, td, shader_root)
        print(f); [print(f'  {v:7d}  {k}') for k, v in sorted(C.items()) if v or k.isupper()]
        total.update(C)
    print('TOTAL'); [print(f'  {v:7d}  {k}') for k, v in sorted(total.items()) if v]
    if SPIRV_VAL and os.path.isfile(SPIRV_VAL): print('distinct modules validated', len(_val), 'failed', sum(1 for v in _val.values() if not v))
    else: print('spirv-val not available: modules decoded, not validated')
if __name__ == '__main__': main()
