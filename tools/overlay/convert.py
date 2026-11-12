"""Switch (NVN, platform 19, gpu type 26) Shader object -> the same object with one Vulkan platform (18, gpu type 25) whose programs are our SPIR-V, in Unity 2018.1.4f1's own container.
Route A: no libunity patch. See PROGRESS.txt for the format facts and tools/spvfix.py for the SPIR-V side.
  convert_shader(o, dmsv) -> (new object dict, report)
"""
import struct, hashlib, copy, collections, lz4.block, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import subprog, smolv, spvfix

PLAT_SWITCH, PLAT_VULKAN, GPU_SPIRV, GPU_SWITCH = 19, 18, 25, 26
STAGE_BIT = {'vs': 1 << 26, 'ps': 1 << 27}
SET_TEX, SET_UBO = 0, 1
TARGET_BASE = 13          # Unity's Vulkan channel target for SPIR-V input location 0 (reference bundles: location n <-> target 13 + n)
SPV_DIM = {2: 1, 3: 2, 4: 3, 5: 1, 6: 3}    # Unity texture dimension (2D, 3D, Cube, 2DArray, CubeArray) -> SPIR-V Dim

def switch_key(code):
    """the dmsv key of a Switch subprogram's code (0x10 header + control section + code area): shader_split.py skey"""
    nctl, ncode = struct.unpack_from('<2i', code, 8); ctl = code[0x10:0x10 + nctl]; cd = code[0x10 + nctl:0x10 + nctl + ncode]
    if ctl[:4] != bytes.fromhex('34127698') or 0x10 + nctl + ncode != len(code): raise ValueError('not a Switch subprogram code')
    n = struct.unpack_from('<I', ctl, 0x20)[0]; n = n if 0x760 <= n < 0x880 else 0x880
    return int(hashlib.sha256(ctl[:n] + cd).hexdigest()[:16], 16)

def best_match(cands, want):
    """ShaderLab::FindBestMatchingShaderKeywordSet @0x9cd570: score = matching - 16 * (candidate keywords not requested), first best wins; an exact set first (hash lookup)"""
    for i, c in enumerate(cands):
        if c == want: return i
    best, bi = None, -1
    for i, c in enumerate(cands):
        s = len(c & want) - 16 * len(c - want)
        if best is None or s > best: best, bi = s, i
    return bi

class Report:
    def __init__(s): s.rows = []; s.count = collections.Counter()
    def note(s, kind, where, text=''): s.rows.append((kind, where, text)); s.count[kind] += 1

def build_program(pv, pf, kws, dm, rep, where):
    """pv / pf: parsed Switch vertex / fragment subprogram blobs -> (parsed Vulkan vertex blob dict, extra info for the parsed-form mirror)"""
    kv, kf = switch_key(pv['code']), switch_key(pf['code'])
    for k, st in ((kv, 'vs'), (kf, 'ps')):
        if k not in dm.e: raise KeyError(f'{st} key {k:016x} is not in shaders.dmsv')
    stages = {'vs': (pv, kv), 'ps': (pf, kf)}
    # ---- constant buffers: Switch binding index = cbuf register; Vulkan: set 1, bindings fragment first (as Unity's own programs) ----
    cbs, groups, binds = {}, [dict(name='', size=0, params=[], structs=[])], []
    cb_of = {}     # (stage, reg) -> (binding, size)
    renames = {}   # (stage, old name) -> new name
    nb = 0
    for st in ('ps', 'vs'):
        p = stages[st][0]; gmap = {g['name']: g for g in p['groups'][1:]}
        if p['groups'][0]['params'] or p['groups'][0]['structs']: raise ValueError('loose value parameters outside constant buffers are not handled')
        for b in p['bindings']:
            if b['type'] != 1: continue
            g = gmap.get(b['name'])
            if g is None: raise ValueError(f'constant buffer binding {b["name"]} without a group')
            name = b['name']
            if name in cbs and (cbs[name]['group']['params'], cbs[name]['group']['structs'], cbs[name]['group']['size']) == (g['params'], g['structs'], g['size']):
                cbs[name]['stages'] |= STAGE_BIT[st]     # the same buffer in both stages: one binding
            else:
                if name in cbs:
                    name = name + ('_P' if st == 'ps' else '_V'); renames[(st, b['name'])] = name; rep.note('cb renamed', where, f'{b["name"]} -> {name}')
                cbs[name] = dict(binding=nb, stages=STAGE_BIT[st], group=dict(g, name=name)); nb += 1
            cb_of[(st, b['index'])] = (cbs[name]['binding'], g['size'])
        for b in p['bindings']:
            if b['type'] not in (0, 1): raise ValueError(f'binding type {b["type"]} ({b["name"]}) is not handled')
    # ---- textures: Switch unit = sampler index field; Vulkan: set 0, one combined image sampler binding each ----
    texs, tex_of = {}, {}
    nt = 0
    for st in ('ps', 'vs'):
        for b in sorted((b for b in stages[st][0]['bindings'] if b['type'] == 0), key=lambda b: (b['extra'] >> 8) & 0xffffff):
            unit = (b['extra'] >> 8) & 0xffffff; dim = b['extra'] & 0xff
            if unit == 0xffffff: raise ValueError(f'texture {b["name"]} without a sampler index')
            if b['index'] not in (0,) and b['index'] >> 10 != 1: raise ValueError(f'texture {b["name"]}: index {b["index"]:#x} in a form not known')
            if b['index'] >> 10 == 1: rep.note('sampler borrowed', where, f'{b["name"]} uses the sampler of unit {b["index"] & 0x3ff} on Switch; its own on Vulkan (as Unity 2018.1 Vulkan does)')
            name = b['name']
            if name in texs and texs[name]['dim'] == dim: texs[name]['stages'] |= STAGE_BIT[st]
            else:
                if name in texs: raise ValueError(f'texture {name} declared twice with different dimensions')
                texs[name] = dict(binding=nt, stages=STAGE_BIT[st], dim=dim, ms=b.get('ms', 0)); nt += 1
            tex_of[(st, unit)] = texs[name]['binding']
    # ---- SPIR-V ----
    mods, infos = {}, {}
    for st in ('vs', 'ps'):
        k = stages[st][1]; refl = dm.refl(k)
        if refl['stage'] != (0 if st == 'vs' else 1): raise ValueError(f'{k:016x} is not a {st} in the bundle')
        spv, info = spvfix.fix(dm.spv(k), st, refl, {r: v for (s, r), v in cb_of.items() if s == st}, {r: v for (s, r), v in tex_of.items() if s == st})
        mods[st], infos[st] = spv, info
        for n in info['notes']: rep.note('spirv note', where, f'{k:016x}: {n}')
        for b in stages[st][0]['bindings']:   # the declared dimension against the module's image type
            if b['type'] == 0:
                t = info['tex'].get((b['extra'] >> 8) & 0xffffff)
                if t and SPV_DIM.get(b['extra'] & 0xff) != t['dim']: rep.note('texture dim mismatch', where, f'{b["name"]}: Unity dim {b["extra"] & 0xff}, SPIR-V Dim {t["dim"]}')
    missing = sorted(set(infos['ps']['inputs']) - set(infos['vs']['outputs']))
    if missing:
        if all(infos['ps']['input_types'][l] == ('float', 4) for l in missing):
            mods['vs'], infos['vs'] = spvfix.fix(dm.spv(kv), 'vs', dm.refl(kv), {r: v for (s, r), v in cb_of.items() if s == 'vs'}, {r: v for (s, r), v in tex_of.items() if s == 'vs'}, pad_outputs=missing)
            rep.note('vs output padded', where, f'locations {missing} written as (0,0,0,1) (vs {kv:016x}, ps {kf:016x})')
        else: rep.note('ps input not written by vs', where, f'locations {missing} (vs {kv:016x}, ps {kf:016x}), types {[infos["ps"]["input_types"][l] for l in missing]}')
    targets = {t for s, t in pv['channels']}
    if set(infos['vs']['inputs']) - targets: rep.note('vs input without a channel', where, f'locations {sorted(set(infos["vs"]["inputs"]) - targets)}')
    # ---- the Vulkan subprogram blob ----
    fs, vs = smolv.encode(mods['ps']), smolv.encode(mods['vs'])
    assert smolv.decode(fs) == mods['ps'] and smolv.decode(vs) == mods['vs']
    code = struct.pack('<11I', 0, 44 + len(fs), len(vs), 44, len(fs), 0, 0, 0, 0, 0, 0) + fs + vs
    for name, c in cbs.items(): groups.append(c['group'])
    for name in sorted(texs):
        t = texs[name]; binds.append(dict(name=name, type=0, index=t['stages'] | SET_TEX << 16 | t['binding'], extra=t['dim'] | t['binding'] << 8, ms=t['ms']))
    for name, c in cbs.items():
        binds.append(dict(name=name, type=1, index=c['stages'] | SET_UBO << 16 | c['binding'], extra=0))
    blob = dict(version=subprog.VERSION, type=GPU_SPIRV, stats=[0, 0, 0, 0], keywords=list(kws), code=code, sourceMap=pv['sourceMap'],
                channels=[(s, TARGET_BASE + t) for s, t in pv['channels']], groups=groups, bindings=binds)
    return blob, dict(mods=mods, keys=(kv, kf), renames=renames)

def stub_blob(kws):
    return dict(version=subprog.VERSION, type=GPU_SPIRV, stats=[0, 0, 0, 0], keywords=list(kws), code=b'', sourceMap=0, channels=[],
                groups=[dict(name='', size=0, params=[], structs=[])], bindings=[])

def mirror(blob, name_index):
    """the parameter block of a Vulkan vertex blob as SerializedSubProgram fields (what Unity writes beside the blob)"""
    def ni(n):
        if n not in name_index: name_index[n] = max(name_index.values(), default=-1) + 1
        return name_index[n]
    cbufs = []
    for g in blob['groups'][1:]:
        cbufs.append(dict(m_NameIndex=ni(g['name']),
                          m_MatrixParams=[dict(m_NameIndex=ni(p['name']), m_Index=p['index'], m_ArraySize=p['array'], m_Type=p['type'], m_RowCount=p['rows']) for p in g['params'] if p['matrix']],
                          m_VectorParams=[dict(m_NameIndex=ni(p['name']), m_Index=p['index'], m_ArraySize=p['array'], m_Type=p['type'], m_Dim=p['dim']) for p in g['params'] if not p['matrix']],
                          m_StructParams=[], m_Size=g['size']))
        if g['structs']: raise ValueError('struct parameters are not handled')
    s32 = lambda v: v - (1 << 32) if v >= 1 << 31 else v
    return dict(m_VectorParams=[], m_MatrixParams=[], m_BufferParams=[], m_UAVParams=[], m_Samplers=[],
                m_TextureParams=[dict(m_NameIndex=ni(b['name']), m_Index=s32(b['index']), m_SamplerIndex=s32(b['index']), m_MultiSampled=bool(b['ms']), m_Dim=b['extra'] & 0xff)
                                 for b in blob['bindings'] if b['type'] == 0],
                m_ConstantBuffers=cbufs,
                m_ConstantBufferBindings=[dict(m_NameIndex=ni(b['name']), m_Index=s32(b['index'])) for b in blob['bindings'] if b['type'] == 1])

EMPTY_PARAMS = dict(m_VectorParams=[], m_MatrixParams=[], m_TextureParams=[], m_BufferParams=[], m_ConstantBuffers=[], m_ConstantBufferBindings=[], m_UAVParams=[], m_Samplers=[])

def convert_shader(o, dm, rep=None, collect=None):
    """o: a Shader object dict as unityfs reads it (platform 19 only). Returns the Vulkan one; collect: optional list that receives (where, stage, spirv bytes)"""
    rep = rep or Report(); o = copy.deepcopy(o); name = o['m_ParsedForm']['m_Name']
    if list(o['platforms']) != [PLAT_SWITCH]: raise ValueError(f'{name}: platforms {o["platforms"]}, expected [19]')
    seg = lz4.block.decompress(bytes(o['compressedBlob'][o['offsets'][0]:o['offsets'][0] + o['compressedLengths'][0]]), uncompressed_size=o['decompressedLengths'][0])
    old = [subprog.parse(s) for s in subprog.split_platform_blob(seg)]
    new_blobs, prog_cache, stub_cache = [], {}, {}
    for si, ss in enumerate(o['m_ParsedForm']['m_SubShaders']):
        for pi, p in enumerate(ss['m_Passes']):
            if p['m_Type'] != 0: continue
            where = f'{name}#s{si}p{pi}'
            for f in ('progGeometry', 'progHull', 'progDomain'):
                if p[f]['m_SubPrograms']: raise ValueError(f'{where}: {f} is not handled')
            V, F = p['progVertex']['m_SubPrograms'], p['progFragment']['m_SubPrograms']
            if not V and not F: continue
            if not V or not F: raise ValueError(f'{where}: a pass with only one of vertex / fragment programs')
            for sp in V + F:
                if sp['m_GpuProgramType'] != GPU_SWITCH: raise ValueError(f'{where}: gpu program type {sp["m_GpuProgramType"]}')
            kv = [frozenset(old[sp['m_BlobIndex']]['keywords']) for sp in V]; kf = [frozenset(old[sp['m_BlobIndex']]['keywords']) for sp in F]
            variants = list(kv) + [k for k in kf if k not in set(kv)]
            if kv != kf: rep.note('keyword lists differ', where, f'{len(kv)} vertex, {len(kf)} fragment variants -> {len(variants)} programs')
            name_index = {e['first']: e['second'] for e in p['m_NameIndices']}
            newV, newF = [], []
            for K in variants:
                vi, fi = best_match(kv, K), best_match(kf, K)
                if kv[vi] != K or kf[fi] != K: rep.note('variant paired by best match', where, f'{sorted(K)}: vertex {sorted(kv[vi])}, fragment {sorted(kf[fi])}')
                src = V[vi] if kv[vi] == K else F[fi]     # the entry whose keyword indices spell K
                kws = old[src['m_BlobIndex']]['keywords']
                ck = (V[vi]['m_BlobIndex'], F[fi]['m_BlobIndex'], tuple(kws))
                if ck not in prog_cache:
                    blob, extra = build_program(old[V[vi]['m_BlobIndex']], old[F[fi]['m_BlobIndex']], kws, dm, rep, f'{where} {sorted(K)}')
                    prog_cache[ck] = (len(new_blobs), blob); new_blobs.append(subprog.build(blob))
                    if collect is not None:
                        for st in ('vs', 'ps'): collect.append((f'{where} {sorted(K)}', st, extra['mods'][st], extra['keys']))
                bi, blob = prog_cache[ck]
                sk = tuple(kws)
                if sk not in stub_cache: stub_cache[sk] = len(new_blobs); new_blobs.append(subprog.build(stub_blob(kws)))
                ve = dict(copy.deepcopy(V[vi]), m_BlobIndex=bi, m_GpuProgramType=GPU_SPIRV, m_KeywordIndices=list(src['m_KeywordIndices']), **mirror(blob, name_index))
                ve['m_Channels'] = dict(m_Channels=[dict(source=s, target=t) for s, t in blob['channels']], m_SourceMap=blob['sourceMap'])
                fe = dict(copy.deepcopy(F[fi]), m_BlobIndex=stub_cache[sk], m_GpuProgramType=GPU_SPIRV, m_KeywordIndices=list(src['m_KeywordIndices']), **copy.deepcopy(EMPTY_PARAMS))
                fe['m_Channels'] = dict(m_Channels=[], m_SourceMap=0)
                newV.append(ve); newF.append(fe)
            p['progVertex']['m_SubPrograms'], p['progFragment']['m_SubPrograms'] = newV, newF
            p['m_NameIndices'] = [dict(first=k, second=v) for k, v in sorted(name_index.items(), key=lambda kv_: kv_[0].encode())]
    seg2 = subprog.join_platform_blob(new_blobs)
    comp = lz4.block.compress(seg2, mode='high_compression', store_size=False)
    o['platforms'] = [PLAT_VULKAN]; o['offsets'] = [0]; o['compressedLengths'] = [len(comp)]; o['decompressedLengths'] = [len(seg2)]; o['compressedBlob'] = comp
    rep.count['shaders'] += 1; rep.count['programs'] += len(prog_cache); rep.count['old subprograms'] += len(old); rep.count['new subprograms'] += len(new_blobs)
    return o, rep
