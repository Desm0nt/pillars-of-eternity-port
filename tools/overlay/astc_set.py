#!/usr/bin/env python3
"""astc_set.py: the game's BC (DXT1 / DXT5 / BC4 / BC5) Texture2D objects re-encoded as ASTC, written as a second overlay tree
overlay-astc/Data (same relative paths as RomFS/Data). On a GPU without BC the native side (native/boot/f2_texset.c) reads that tree before
overlay/Data and RomFS/Data, so the stock player uploads ASTC instead of decompressing BC to RGBA32 on the CPU (4x the memory for DXT5, 8x for DXT1).

Run with a Python that has the packages of tools/requirements.txt (astc-encoder-py etc.): python tools/overlay/astc_set.py <command>
(tools/setup_poe.py runs it with ASTC_SET_DATA / ASTC_SET_OVERLAY / ASTC_SET_OUT / ASTC_SET_WORK pointing at its work folder)
  scan     [-j N]                        every texture of RomFS/Data (overlay copy where the overlay has the file) -> build/astc/scan.jsonl + summary
  measure  [--n N]                       PSNR / size of the candidate block sizes on a sample of each class -> build/astc/measure.json
  build    [-j N] [--only SUBSTR[,..]] [--files LIST] [--resume] [--block-color 6x6] [--psnr-every N]
                                         writes overlay-astc/Data/<rel>; records per texture in build/astc/build.jsonl, per file in done.jsonl
  verify   [--every N] [--only SUBSTR]   re-parses every written file, compares it with its source, decodes a sample back -> build/astc/verify.json
  formats  [--tree overlay-astc]         texture count / formats of the source vs the written tree
  sets                                   prints the file lists of the named subsets (menu, first area) used with --files

What a texture becomes (the player's own table vk::s_TextureFormatInfo, Release 0x10594c8: DXT1 -> VK BC1_RGB, DXT5 -> BC3, BC4 -> BC4_UNORM,
BC5 -> BC5_UNORM, ASTC_RGB(A)_NxN -> ASTC_NxN; fallback RGBA32 when the VkFormat is not sampleable = CPU decompression):
  nm    DXT5 tiles HGT_* / NM_* of the pre-rendered areas, R = 255, data in G and A (Trenton/PE_BackgroundPassMRT reads _DepthTex.a and
        _NormalTex.ga; Trenton/NormalMapPass reads .ga): ASTC_RGBA (G, G, G, A) = luminance + alpha endpoints, --block-data (4x4)
  rgb   DXT1 (sampled as BC1_RGB: alpha 1, a punch-through texel is opaque black) and opaque DXT5: ASTC_RGB, --block-color
  rgba  DXT5 with alpha: ASTC_RGBA, --block-alpha (4x4)
  rg    BC5 (sampled (R, G, 0, 1)): ASTC_RGB (R, G, 0, 255), --block-data;  r  BC4: ASTC_RGB (R, 0, 0, 255), --block-data
  BC6H / BC7 / crunched: not present in this game; left as they are (reported)
Profile LDR (the project is in gamma space: textures are sampled through the linear VkFormat). All-zero ASTC blocks (= error blocks, port-kb
TEX-04) are replaced by the transparent-black void extent; astcenc does not produce them, the check stays.

Placement: a texture's new data goes where its BC data was when it fits (ASTC 4x4 = DXT5 / BC5 size; 6x6 < DXT1), else it is appended to the
end of the same .resS (16-aligned). Every other byte of the .resS keeps its offset, so objects this tool does not parse (Texture3D without a
type tree, meshes, ...) are unaffected. Inline image data is replaced in the object (the serialized file is laid out again by sfile.replace_objects).
Bundles are written back with one stored block (sfile.repack_bundle, as the shader overlay does).
"""
import argparse, collections, json, os, struct, sys, time, pickle, re
from concurrent.futures import ProcessPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ucommon import unityfs, common as _common_strings  # noqa: E402
import sfile  # noqa: E402
import numpy as np  # noqa: E402

APP = os.path.normpath(os.path.join(HERE, '..', '..'))   # the repository; the defaults below are its work/setup folder
# roots: command-line options --data / --overlay / --out / --work, passed to the worker processes through the environment
DATA = os.environ.get('ASTC_SET_DATA', '')
OVERLAY = os.environ.get('ASTC_SET_OVERLAY', os.path.join(APP, 'work', 'setup', 'overlay', 'Data'))
OUT = os.environ.get('ASTC_SET_OUT', os.path.join(APP, 'work', 'setup', 'overlay-astc', 'Data'))
WORK = os.environ.get('ASTC_SET_WORK', os.path.join(APP, 'work', 'setup', 'astc-build'))
TEX_CLASSES = {28: 'Texture2D', 89: 'Cubemap', 117: 'Texture3D', 187: 'Texture2DArray', 188: 'CubemapArray'}
BC = {10: ('DXT1', 8), 12: ('DXT5', 16), 26: ('BC4', 8), 27: ('BC5', 16)}
BC_OTHER = {24: 'BC6H', 25: 'BC7', 28: 'DXT1Crunched', 29: 'DXT5Crunched'}
ASTC_RGB = {4: 48, 5: 49, 6: 50, 8: 51, 10: 52, 12: 53}
ASTC_RGBA = {4: 54, 5: 55, 6: 56, 8: 57, 10: 58, 12: 59}
ASTC_BLOCK = {v: k for d in (ASTC_RGB, ASTC_RGBA) for k, v in d.items()}
FMT_NAME = {1: 'Alpha8', 3: 'RGB24', 4: 'RGBA32', 5: 'ARGB32', 7: 'RGB565', 10: 'DXT1', 12: 'DXT5', 13: 'RGBA4444', 14: 'BGRA32', 26: 'BC4', 27: 'BC5',
            **{v: f'ASTC_RGB_{k}x{k}' for k, v in ASTC_RGB.items()}, **{v: f'ASTC_RGBA_{k}x{k}' for k, v in ASTC_RGBA.items()}, **BC_OTHER}
QUALITY = {'fastest': 0.0, 'fast': 10.0, 'medium': 60.0, 'thorough': 98.0}
VOID_BLACK = bytes.fromhex('fcfdffffffffffff0000000000000000')     # LDR void extent, colour (0, 0, 0, 0)
# the Switch 'Resources/unity default resources' is never opened by the Android player (it opens <Data>/unity default resources, which the
# overlay replaces with the Android player's own file; PROGRESS_data.txt decision 1)
EXCLUDE = {'Resources/unity default resources'}


def keep_bc(o):
    """A BC texture the game reads on the CPU (m_IsReadable) stays as it is, bit for bit.  FootstepMapLoader looks every pixel of the area's
    BKGSM_*_fs map up in a Dictionary<Color, GroundMaterial> (exact colours), SaveGameInfo.Save resizes the small portraits with GetPixel:
    a re-encoded picture would give other numbers than on the console.  221 textures, 53 MB; none of them is an area tile."""
    return bool(o.get('m_IsReadable'))
NM_NAME = re.compile(r'^(HGT|NM)_')
READ_CH = {'nm': [1, 3], 'rgb': [0, 1, 2], 'rgba': [0, 1, 2, 3], 'rg': [0, 1], 'r': [0]}   # what the shaders read of each class

# subsets for a partial build: main menu (the files the menu opens on the phone, phone-runs) + the first game area (Gilded Vale wilderness, Encampment)
SETS = {'menu': ['resources.assets', 'sharedassets0.assets', 'sharedassets1.assets', 'globalgamemanagers.assets', 'level0', 'level1',
                 'sharedassets2.assets', 'sharedassets3.assets', 'Resources/unity_builtin_extra'],
        'first_area': ['st_ar_0701_gilded_vale_wilderness']}


def blk(s):
    a, b = s.lower().split('x'); assert a == b, 'the 2018.1 TextureFormat enum has square ASTC blocks only'; return int(a)


def src_path(rel):
    """the file the tree is built from: the existing overlay's copy when it has one (its rewrites are kept), else the Switch file"""
    o = os.path.join(OVERLAY, rel)
    return o if os.path.exists(o) else os.path.join(DATA, rel)


def all_files():
    out = []
    for root, _, names in os.walk(DATA):
        for n in names:
            p = os.path.join(root, n)
            if n.endswith(('.resS', '.resource')) or os.path.getsize(p) < 48: continue
            with open(p, 'rb') as f: head = f.read(48)
            if head[:8] == b'UnityFS\0' or _ser_head(head, os.path.getsize(p)):
                out.append(os.path.relpath(p, DATA).replace('\\', '/'))
    return sorted(out, key=lambda r: -os.path.getsize(os.path.join(DATA, r)))


def _ser_head(d, size):
    ver = struct.unpack_from('>I', d, 8)[0]
    if not 17 <= ver <= 22 or d[16] not in (0, 1): return False
    msz, fsz, _, doff = struct.unpack_from('>IIII', d, 0)
    return fsz == size and 0 < msz < fsz and doff <= fsz


_C = None
def C():
    global _C
    if _C is None: _C = _common_strings()
    return _C


_TREES = {}
def trees():
    """{(class, type hash): tree} of the texture classes, from the bundles (the loose files carry none); cached in build/astc/trees.pkl"""
    if _TREES: return _TREES
    cache = os.path.join(WORK, 'trees.pkl')
    if os.path.exists(cache):
        _TREES.update(pickle.load(open(cache, 'rb'))); return _TREES
    ab = os.path.join(DATA, 'StreamingAssets', 'assetbundles')
    for n in sorted(os.listdir(ab), key=lambda n: os.path.getsize(os.path.join(ab, n))):
        d = open(os.path.join(ab, n), 'rb').read()
        if d[:8] != b'UnityFS\0': continue
        for _, x in unityfs.bundle_files(d):
            if unityfs.is_serialized(x):
                for _ in unityfs.serialized_objects(x, C(), set(TEX_CLASSES), _TREES): break
        if any(c == 28 for c, _ in _TREES) and any(c == 89 for c, _ in _TREES): break
    os.makedirs(WORK, exist_ok=True); pickle.dump(dict(_TREES), open(cache, 'wb'))
    return _TREES


def tex_objects(ser, tr, classes=(28,)):
    """texture objects of one serialized file: yields (pid, class, table entry, object dict, tree, raw bytes); a class with no tree -> (pid, cid, ent, None, None, raw)"""
    s = sfile.parse_serialized(ser)
    if s['tt']:
        for _ in unityfs.serialized_objects(ser, C(), set(classes), tr): break
    for pid, start, size, ti, ent in s['objs']:
        cid, h = s['types'][ti]
        if cid not in classes: continue
        raw = ser[s['doff'] + start:s['doff'] + start + size]
        root = tr.get((cid, h))
        if root is None: yield pid, cid, ent, None, None, raw; continue
        r = unityfs.Reader(raw, s['le']); o = unityfs._read(r, root)
        yield pid, cid, ent, o, root, raw


def level_dims(w, h, n):
    return [(max(1, w >> i), max(1, h >> i)) for i in range(max(1, n))]


def bc_level_size(fmt, w, h):
    return ((w + 3) // 4) * ((h + 3) // 4) * BC[fmt][1]


def astc_level_size(b, w, h):
    return ((w + b - 1) // b) * ((h + b - 1) // b) * 16


def tex_data(o, blobs, rel, loose_res):
    """the object's image bytes: inline image data, a bundle node (archive:/...) or a loose .resS next to the file"""
    img = o.get('image data') or b''
    if len(img): return bytes(img), 'inline', None
    sd = o['m_StreamData']; path = sd['path']
    if not path: return b'', 'none', None
    if path.startswith('archive:/'):
        nm = path.rsplit('/', 1)[-1]; blob = blobs[nm]
        return bytes(blob[sd['offset']:sd['offset'] + sd['size']]), 'node', nm
    key = os.path.join(os.path.dirname(rel), path).replace('\\', '/')
    if key not in loose_res: loose_res[key] = bytearray(open(src_path(key), 'rb').read())
    blob = loose_res[key]
    return bytes(blob[sd['offset']:sd['offset'] + sd['size']]), 'file', key


# ------------------------------------------------------------------ BC decode / ASTC encode
def decode_bc(fmt, data, w, h):
    """one BC level -> (h, w, 4) uint8 RGBA as the GPU samples the player's VkFormat for it"""
    import texture2ddecoder as t2d
    fn = {10: t2d.decode_bc1, 12: t2d.decode_bc3, 26: t2d.decode_bc4, 27: t2d.decode_bc5}[fmt]
    img = np.frombuffer(fn(data, w, h), np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
    if fmt == 10: img[..., 3] = 255                     # BC1_RGB: no alpha (texture2ddecoder already gives index 3 of a 3-colour block opaque black)
    if fmt == 26: img[..., 1:3] = 0; img[..., 3] = 255
    if fmt == 27: img[..., 2] = 0; img[..., 3] = 255
    return img


_ctx = {}
def ctx_for(b, q, la=False):
    """la: luminance + alpha input (G, G, G, A): alpha weight 3 so that A counts as much as the luminance replicated in R, G, B (a smaller total
    error of the two channels the shaders read; on the worst NM tiles of st_ar_0705 alpha 33.4 -> 35.0 dB, G 43.4 -> 39.0 dB)"""
    import astc_encoder as ae
    k = (b, q, la)
    if k not in _ctx:
        cfg = ae.ASTCConfig(ae.ASTCProfile.LDR, b, b, 1, QUALITY[q])
        if la: cfg.cw_a_weight = 3.0
        _ctx[k] = (ae, ae.ASTCContext(cfg, 1), ae.ASTCSwizzle())
    return _ctx[k]


def encode(img, b, q, la=False):
    ae, c, sw = ctx_for(b, q, la); h, w = img.shape[:2]
    return c.compress(ae.ASTCImage(ae.ASTCType.U8, w, h, 1, np.ascontiguousarray(img).tobytes()), sw)


def decode_astc(blocks, w, h, b, q='medium'):
    ae, c, sw = ctx_for(b, q)
    im = c.decompress(blocks, ae.ASTCImage(ae.ASTCType.U8, w, h, 1), sw)
    return np.frombuffer(im.data, np.uint8).reshape(h, w, 4)


def zero_blocks(b):
    a = np.frombuffer(b, np.uint8).reshape(-1, 16); return int((~a.any(axis=1)).sum())


def sanitize(b):
    a = np.frombuffer(b, np.uint8).reshape(-1, 16); z = ~a.any(axis=1)
    if not z.any(): return b, 0
    a = a.copy(); a[z] = np.frombuffer(VOID_BLACK, np.uint8); return a.tobytes(), int(z.sum())


def enc_input(cls, img):
    if cls == 'nm': return img[..., [1, 1, 1, 3]]
    o = img.copy()
    if cls in ('rgb', 'rg', 'r'): o[..., 3] = 255
    if cls == 'rg': o[..., 2] = 0
    if cls == 'r': o[..., 1:3] = 0
    return o


def psnr(a, b, ch):
    mse = float(np.mean((a[..., ch].astype(np.float64) - b[..., ch]) ** 2))
    return 99.0 if mse == 0 else round(10 * np.log10(255.0 ** 2 / mse), 2)


def classify(fmt, name, levels):
    """levels = decoded RGBA images of every mip level"""
    if fmt == 10: return 'rgb'
    if fmt == 27: return 'rg'
    if fmt == 26: return 'r'
    if NM_NAME.match(name) and all((im[..., 0] == 255).all() for im in levels): return 'nm'
    return 'rgb' if all((im[..., 3] == 255).all() for im in levels) else 'rgba'


def transcode(fmt, name, w, h, mips, data, opts, measure_psnr=False):
    """BC image (all mip levels) -> (new TextureFormat, ASTC bytes, info)"""
    lv = level_dims(w, h, mips); sizes = [bc_level_size(fmt, x, y) for x, y in lv]
    if sum(sizes) > len(data): raise ValueError(f'{name}: {len(data)} bytes < {sum(sizes)} for {len(lv)} levels')
    p = 0; imgs = []
    for (x, y), n in zip(lv, sizes):
        imgs.append(decode_bc(fmt, data[p:p + n], x, y)); p += n
    cls = classify(fmt, name, imgs)
    b = {'nm': opts['block_data'], 'rg': opts['block_data'], 'r': opts['block_data'], 'rgba': opts['block_alpha'], 'rgb': opts['block_color']}[cls]
    nf = (ASTC_RGBA if cls in ('nm', 'rgba') else ASTC_RGB)[b]
    out = []; nz = 0; q = None
    for i, ((x, y), im) in enumerate(zip(lv, imgs)):
        e = encode(enc_input(cls, im), b, opts['quality'], cls == 'nm')
        assert len(e) == astc_level_size(b, x, y), (name, x, y, len(e))
        e, z = sanitize(e); nz += z; out.append(e)
        if measure_psnr and i == 0:
            got = decode_astc(e, x, y, b)
            ref = imgs[0]
            q = {'all': psnr(ref, got, READ_CH[cls])}
            if cls == 'nm':
                q['g'] = psnr(ref, got, [1]); q['a'] = psnr(ref, got, [3])
                q['maxerr_g'] = int(np.abs(ref[..., 1].astype(int) - got[..., 1]).max()); q['maxerr_a'] = int(np.abs(ref[..., 3].astype(int) - got[..., 3]).max())
    return nf, b''.join(out), dict(cls=cls, block=b, zero_fixed=nz, psnr=q)


# ------------------------------------------------------------------ scan
def scan_file(rel):
    tr = dict(trees()); rows = []; err = None
    try:
        path = src_path(rel); d = open(path, 'rb').read()
        sers = [(nm, x) for nm, x in unityfs.bundle_files(d) if unityfs.is_serialized(x)] if d[:8] == b'UnityFS\0' else [('', d)]
        for nm, x in sers:
            for pid, cid, ent, o, root, raw in tex_objects(x, tr, tuple(TEX_CLASSES)):
                if o is None: rows.append(dict(file=rel, node=nm, pid=pid, cls=TEX_CLASSES[cid], notree=1)); continue
                fmt = o.get('m_TextureFormat', o.get('m_Format')); sd = o.get('m_StreamData') or {}
                rows.append(dict(file=rel, node=nm, pid=pid, cls=TEX_CLASSES[cid], name=o.get('m_Name', ''), fmt=fmt, w=o.get('m_Width'), h=o.get('m_Height'),
                                 mips=o.get('m_MipCount', 1), images=o.get('m_ImageCount', 1), complete=o.get('m_CompleteImageSize'), cs=o.get('m_ColorSpace'),
                                 readable=int(bool(o.get('m_IsReadable'))), inline=len(o.get('image data') or b''), spath=sd.get('path', ''),
                                 soff=sd.get('offset', 0), ssize=sd.get('size', 0), src='overlay' if path.startswith(OVERLAY) else 'romfs'))
    except Exception as e:
        err = f'{type(e).__name__}: {e}'
    return rel, rows, err


def cmd_scan(a):
    os.makedirs(WORK, exist_ok=True); trees()
    files = all_files(); t0 = time.time(); n = 0; errs = []
    with open(os.path.join(WORK, 'scan.jsonl'), 'w') as f, ProcessPoolExecutor(a.j) as ex:
        for rel, rows, err in ex.map(scan_file, files, chunksize=4):
            for r in rows: f.write(json.dumps(r) + '\n'); n += 1
            if err: errs.append((rel, err))
    print(f'{len(files)} files, {n} texture objects, {len(errs)} errors, {time.time() - t0:.0f} s')
    if errs: sys.exit('scan errors')
    json.dump(dict(data=DATA, overlay=OVERLAY), open(os.path.join(WORK, 'scan.meta.json'), 'w'))
    for e in errs[:20]: print('  ERR', e)
    summary(os.path.join(WORK, 'scan.jsonl'))


def load_scan():
    return [json.loads(l) for l in open(os.path.join(WORK, 'scan.jsonl'))]


def summary(path, title='source'):
    rows = [json.loads(l) for l in open(path)]
    c = collections.Counter(); mb = collections.Counter(); notree = collections.Counter()
    for r in rows:
        if r.get('notree'): notree[r['cls']] += 1; continue
        k = (r['cls'], FMT_NAME.get(r['fmt'], str(r['fmt']))); c[k] += 1; mb[k] += (r['inline'] or r['ssize'])
    print(f'{title}: texture objects by class / format (count, MB of image data)')
    for k, v in sorted(c.items(), key=lambda kv: -mb[kv[0]]): print(f'  {k[0]:10s} {k[1]:16s} {v:7d} {mb[k] / 2**20:9.1f}')
    for k, v in notree.items(): print(f'  {k:10s} (no type tree, not read) {v}')
    return c, mb


# ------------------------------------------------------------------ build
def build_file(job):
    rel, opts = job
    t0 = time.time(); recs = []; tr = dict(trees())
    path = src_path(rel); d = open(path, 'rb').read()
    bundle = d[:8] == b'UnityFS\0'
    if bundle:
        nodes = unityfs.bundle_files(d); blobs = {nm: x for nm, x in nodes}; sers = [(nm, x) for nm, x in nodes if unityfs.is_serialized(x)]
    else:
        blobs = {}; sers = [('', d)]
    newblobs = {}; loose_res = {}; new_ser = {}
    for nm, x in sers:
        repl = {}
        le = sfile.parse_serialized(x)['le']
        for pid, cid, ent, o, root, raw in tex_objects(x, tr):
            if o is None: continue
            fmt = o['m_TextureFormat']
            if fmt in BC_OTHER: recs.append(dict(file=rel, node=nm, pid=pid, name=o['m_Name'], fmt=fmt, skipped='format not handled')); continue
            if fmt not in BC: continue
            if keep_bc(o): recs.append(dict(file=rel, node=nm, pid=pid, name=o['m_Name'], fmt=fmt, skipped='readable: kept as BC')); continue
            if sfile.obj_bytes(root, o, le) != raw: raise ValueError(f'{rel} {nm} {pid}: Texture2D does not round-trip through its type tree')
            if o['m_ImageCount'] != 1 or o['m_TextureDimension'] != 2: raise ValueError(f'{rel} {pid}: image count {o["m_ImageCount"]} dim {o["m_TextureDimension"]}')
            data, where, key = tex_data(o, blobs, rel, loose_res)
            w, h, mips = o['m_Width'], o['m_Height'], o['m_MipCount']
            sizes = sum(bc_level_size(fmt, x_, y_) for x_, y_ in level_dims(w, h, mips))
            if sizes != o['m_CompleteImageSize']: raise ValueError(f'{rel} {pid} {o["m_Name"]}: complete size {o["m_CompleteImageSize"]} != {sizes}')
            want_q = bool(opts['psnr_every']) and (opts['psnr_every'] == 1 or (pid % opts['psnr_every']) == 0) and w >= 16 and h >= 16
            nf, astc, info = transcode(fmt, o['m_Name'], w, h, mips, data, opts, want_q)
            o2 = dict(o); o2['m_TextureFormat'] = nf; o2['m_CompleteImageSize'] = len(astc)
            place = where
            if where == 'inline':
                o2['image data'] = astc
            else:
                sd = dict(o['m_StreamData'])
                if where == 'node':
                    if key not in newblobs: newblobs[key] = bytearray(blobs[key])
                    blob = newblobs[key]
                else:
                    blob = loose_res[key]
                if len(astc) <= sd['size']:
                    blob[sd['offset']:sd['offset'] + sd['size']] = astc + b'\0' * (sd['size'] - len(astc)); place += ' in place'
                else:
                    blob.extend(b'\0' * (-len(blob) % 16)); sd['offset'] = len(blob); blob.extend(astc); place += ' appended'
                    if len(blob) >= 2**32: raise ValueError('resS over 4 GB')
                sd['size'] = len(astc); o2['m_StreamData'] = sd
            repl[pid] = sfile.obj_bytes(root, o2, le)
            recs.append(dict(file=rel, node=nm, pid=pid, name=o['m_Name'], fmt=fmt, nfmt=nf, w=w, h=h, mips=mips, old=len(data), new=len(astc),
                             place=place, **info))
        if repl: new_ser[nm] = sfile.replace_objects(x, repl)
    if not new_ser: return rel, recs, dict(skipped='no BC textures', seconds=round(time.time() - t0, 1))
    outs = []
    if bundle:
        files = dict(new_ser); files.update({k: bytes(v) for k, v in newblobs.items()})
        outs.append((rel, sfile.repack_bundle(d, files)))
    else:
        outs.append((rel, new_ser['']))
        for key, blob in loose_res.items(): outs.append((key, bytes(blob)))
    written = []
    for r, b in outs:
        p = os.path.join(OUT, r); os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + '.tmp', 'wb') as f: f.write(b)
        os.replace(p + '.tmp', p); written.append((r, len(b)))
    st = dict(src=os.path.abspath(path).replace('\\', '/'), src_size=len(d), written=written, textures=len([r for r in recs if 'nfmt' in r]),
              seconds=round(time.time() - t0, 1), opts=opts_key(opts))
    return rel, recs, st


def opts_key(o):
    return f"data{o['block_data']} color{o['block_color']} alpha{o['block_alpha']} {o['quality']}"


def bc_files():
    """files with BC textures, from scan.jsonl, largest BC payload first"""
    pay = collections.Counter()
    for r in load_scan():
        if not r.get('notree') and r['fmt'] in BC and not r.get('readable') and r['file'] not in EXCLUDE: pay[r['file']] += r['inline'] or r['ssize']
    return [f for f, _ in pay.most_common()]


def pick(files, a):
    if a.files:
        want = [x for s in a.files.split(',') for x in (SETS.get(s, [s]))]
        files = [f for f in files if any(f == w or f.endswith('/' + w) or (w.startswith('st_') and f.rsplit('/', 1)[-1].startswith(w)) for w in want)]
    if a.only: files = [f for f in files if any(s.lower() in f.lower() for s in a.only.split(','))]
    return files


def load_done():
    p = os.path.join(WORK, 'done.jsonl'); out = {}
    if os.path.exists(p):
        for l in open(p):
            r = json.loads(l); out[r['file']] = r
    return out


def cmd_build(a):
    opts = dict(block_data=blk(a.block_data), block_color=blk(a.block_color), block_alpha=blk(a.block_alpha), quality=a.quality, psnr_every=a.psnr_every)
    trees()
    files = pick(bc_files(), a)
    done = load_done()
    if a.resume:
        files = [f for f in files if not (f in done and done[f].get('opts') == opts_key(opts) and all(os.path.exists(os.path.join(OUT, r)) for r, _ in done[f].get('written', [])))]
    import shutil
    os.makedirs(OUT, exist_ok=True); free = shutil.disk_usage(OUT).free
    need = sum(os.path.getsize(src_path(f)) for f in files)
    print(f'{len(files)} files to write, about {need / 2**30:.1f} GB; {free / 2**30:.0f} GB free on the drive', flush=True)
    if need * 1.1 > free: sys.exit('not enough free space')
    t0 = time.time(); tot = collections.Counter(); out_b = 0
    with open(os.path.join(WORK, 'build.jsonl'), 'a') as fb, open(os.path.join(WORK, 'done.jsonl'), 'a') as fd, ProcessPoolExecutor(a.j) as ex:
        for i, (rel, recs, st) in enumerate(ex.map(build_file, [(f, opts) for f in files], chunksize=1)):
            for r in recs:
                fb.write(json.dumps(r) + '\n')
                if 'nfmt' in r: tot[r['cls']] += 1; tot['zero_fixed'] += r['zero_fixed']
            fd.write(json.dumps(dict(file=rel, **st)) + '\n'); fd.flush(); fb.flush()
            out_b += sum(n for _, n in st.get('written', []))
            if i % 10 == 0 or i == len(files) - 1:
                print(f'{i + 1}/{len(files)} files, {out_b / 2**30:.2f} GB written, {dict(tot)}, {time.time() - t0:.0f} s', flush=True)
    print(f'done in {time.time() - t0:.0f} s')


# ------------------------------------------------------------------ measure (block size choice)
def cmd_measure(a):
    import random
    rows = [r for r in load_scan() if not r.get('notree') and r['fmt'] in BC and r['w'] >= 64 and r['h'] >= 64]
    random.seed(1)
    groups = collections.defaultdict(list)
    for r in rows:
        k = 'HGT' if r['name'].startswith('HGT_') else 'NM' if r['name'].startswith('NM_') else FMT_NAME[r['fmt']]
        groups[k].append(r)
    res = {}
    for k, rs in groups.items():
        sample = random.sample(rs, min(a.n, len(rs)))
        for r in sample:
            data = read_tex(r)
            lv = level_dims(r['w'], r['h'], 1); img = decode_bc(r['fmt'], data[:bc_level_size(r['fmt'], r['w'], r['h'])], r['w'], r['h'])
            cls = classify(r['fmt'], r['name'], [img])
            for b in (4, 5, 6, 8):
                e = encode(enc_input(cls, img), b, 'medium', cls == 'nm'); got = decode_astc(e, r['w'], r['h'], b)
                q = psnr(img, got, READ_CH[cls])
                res.setdefault((k, cls, b), []).append((q, len(e) / bc_level_size(r['fmt'], r['w'], r['h']), r['name']))
    out = {}
    for (k, cls, b), v in sorted(res.items()):
        qs = [x[0] for x in v]
        out[f'{k} {cls} {b}x{b}'] = dict(n=len(v), mean=round(float(np.mean(qs)), 2), p10=round(float(np.percentile(qs, 10)), 2), min=min(qs),
                                        size_vs_bc=round(float(np.mean([x[1] for x in v])), 3), worst=min(v)[2])
        print(f'{k:6s} {cls:5s} {b}x{b}: n {len(v):3d}  PSNR mean {np.mean(qs):6.2f}  p10 {np.percentile(qs, 10):6.2f}  min {min(qs):6.2f}  size/BC {out[f"{k} {cls} {b}x{b}"]["size_vs_bc"]}')
    json.dump(out, open(os.path.join(WORK, 'measure.json'), 'w'), indent=1)


def read_tex(r):
    """image bytes of a scan row from its SOURCE file"""
    if r['inline']:
        d = open(src_path(r['file']), 'rb').read()
        sers = dict((nm, x) for nm, x in unityfs.bundle_files(d)) if d[:8] == b'UnityFS\0' else {'': d}
        for pid, cid, ent, o, root, raw in tex_objects(sers[r['node']], dict(trees())):
            if pid == r['pid']: return bytes(o['image data'])
    if r['spath'].startswith('archive:/'):
        d = open(src_path(r['file']), 'rb').read()
        blob = dict(unityfs.bundle_files(d))[r['spath'].rsplit('/', 1)[-1]]
        return blob[r['soff']:r['soff'] + r['ssize']]
    with open(src_path(os.path.join(os.path.dirname(r['file']), r['spath']).replace('\\', '/')), 'rb') as f:
        f.seek(r['soff']); return f.read(r['ssize'])


# ------------------------------------------------------------------ verify
def verify_file(job):
    rel, every = job
    rep = collections.Counter(); bad = []; q = []
    tr = dict(trees())
    src = open(src_path(rel), 'rb').read(); out = open(os.path.join(OUT, rel), 'rb').read()
    bundle = src[:8] == b'UnityFS\0'
    if bundle:
        sn = unityfs.bundle_files(src); on = unityfs.bundle_files(out)
        if [n for n, _ in sn] != [n for n, _ in on]: bad.append('bundle node list differs')
        sb = dict(sn); ob = dict(on)
    else:
        sb, ob = {'': src}, {'': out}
    res_src, res_out = {}, {}; ranges = collections.defaultdict(list)
    for nm in sb:
        if not unityfs.is_serialized(sb[nm]): continue
        x, y = sb[nm], ob[nm]
        s1, s2 = sfile.parse_serialized(x), sfile.parse_serialized(y)
        if [(p, ti) for p, _, _, ti, _ in s1['objs']] != [(p, ti) for p, _, _, ti, _ in s2['objs']] or s1['types'] != s2['types']: bad.append(f'{nm}: object / type table differs'); continue
        if x[:4] + x[8:s1['plat_off'] + 4] != y[:4] + y[8:s2['plat_off'] + 4]: bad.append(f'{nm}: header differs')   # (bytes 4..8 = file size)
        # every object re-parses where a tree exists (bundles: all classes; loose: the texture classes)
        if s2['tt']:
            try:
                for _ in unityfs.serialized_objects(y, C(), None, {}): rep['objects_reparsed'] += 1
            except Exception as e: bad.append(f'{nm}: re-parse failed: {e}')
        so = {p: (st, sz, ti) for p, st, sz, ti, _ in s1['objs']}; oo = {p: (st, sz, ti) for p, st, sz, ti, _ in s2['objs']}
        texo = {}
        for pid, cid, ent, o, root, raw in tex_objects(y, tr): texo[pid] = (o, root, raw)
        texs = {}
        for pid, cid, ent, o, root, raw in tex_objects(x, tr): texs[pid] = (o, root, raw)
        for pid, (st, sz, ti) in so.items():
            a_ = x[s1['doff'] + st:s1['doff'] + st + sz]; b_ = y[s2['doff'] + oo[pid][0]:s2['doff'] + oo[pid][0] + oo[pid][1]]
            if pid not in texs or texs[pid][0] is None or texs[pid][0]['m_TextureFormat'] not in BC or keep_bc(texs[pid][0]):
                if a_ != b_: bad.append(f'{nm} {pid}: object changed although not a BC texture to convert')
                else: rep['objects_identical'] += 1
                if pid in texs and texs[pid][0] is not None and texs[pid][0]['m_TextureFormat'] in BC: rep['readable BC kept'] += 1
                continue
            o1 = texs[pid][0]; o2, root2, raw2 = texo[pid]
            if sfile.obj_bytes(root2, o2, s2['le']) != raw2: bad.append(f'{nm} {pid}: written object does not round-trip')
            changed = {k for k in o1 if o1[k] != o2[k]}
            if not changed <= {'m_TextureFormat', 'm_CompleteImageSize', 'm_StreamData', 'image data'}: bad.append(f'{nm} {pid}: unexpected field change {changed}')
            nf = o2['m_TextureFormat']
            if nf not in ASTC_BLOCK: bad.append(f'{nm} {pid}: format {nf} is not ASTC'); continue
            b = ASTC_BLOCK[nf]; w, h, mips = o2['m_Width'], o2['m_Height'], o2['m_MipCount']
            lv = level_dims(w, h, mips); need = sum(astc_level_size(b, x_, y_) for x_, y_ in lv)
            if o2['m_CompleteImageSize'] != need: bad.append(f'{nm} {pid}: complete size {o2["m_CompleteImageSize"]} != {need}')
            d2 = vdata(o2, ob, rel, res_out, OUT); d1 = vdata(o1, sb, rel, res_src, None)
            sd1 = o1['m_StreamData']
            if not len(o1.get('image data') or b''): ranges[stream_key(sd1, rel)].append((sd1['offset'], sd1['size']))
            if len(d2) != need: bad.append(f'{nm} {pid} {o2["m_Name"]}: data {len(d2)} != {need}'); continue
            z = zero_blocks(d2); rep['zero_blocks'] += z
            if z: bad.append(f'{nm} {pid}: {z} all-zero ASTC blocks')
            rep[f'converted {FMT_NAME[o1["m_TextureFormat"]]} -> {FMT_NAME[nf]}'] += 1
            if every and (pid % every == 0) and w >= 16 and h >= 16:
                fmt = o1['m_TextureFormat']; ref = decode_bc(fmt, d1[:bc_level_size(fmt, w, h)], w, h); got = decode_astc(d2[:astc_level_size(b, w, h)], w, h, b)
                cls = classify(fmt, o1['m_Name'], [ref])
                e = dict(file=rel, name=o1['m_Name'], cls=cls, block=b, w=w, h=h, psnr=psnr(ref, got, READ_CH[cls]))
                if cls == 'nm':
                    e.update(psnr_g=psnr(ref, got, [1]), psnr_a=psnr(ref, got, [3]), maxerr_g=int(np.abs(ref[..., 1].astype(int) - got[..., 1]).max()),
                             maxerr_a=int(np.abs(ref[..., 3].astype(int) - got[..., 3]).max()))
                q.append(e)
    # the stream files: every byte outside the converted textures' old ranges is unchanged (in-place placement keeps all other offsets)
    streams = [(k, sb[k], ob[k]) for k in ob if not unityfs.is_serialized(ob[k])] if bundle else               [(k, open(src_path(k), 'rb').read(), open(os.path.join(OUT, k), 'rb').read()) for k in ranges if os.path.exists(os.path.join(OUT, k))]
    for k, a_, b_ in streams:
        if len(b_) < len(a_): bad.append(f'{k}: shorter than the source'); continue
        m = np.ones(len(a_), bool)
        for off, n in ranges.get(k, []): m[off:off + n] = False
        A_ = np.frombuffer(a_, np.uint8); B_ = np.frombuffer(b_[:len(a_)], np.uint8)
        diff = int((A_[m] != B_[m]).sum())
        if diff: bad.append(f'{k}: {diff} bytes outside the converted textures changed')
        rep['stream_bytes_kept'] += int(m.sum()); rep['stream_bytes_appended'] += len(b_) - len(a_)
    return rel, dict(rep), bad, q


def stream_key(sd, rel):
    p = sd['path']
    return p.rsplit('/', 1)[-1] if p.startswith('archive:/') else os.path.join(os.path.dirname(rel), p).replace(chr(92), '/')


def vdata(o, blobs, rel, cache, root_out):
    img = o.get('image data') or b''
    if len(img): return bytes(img)
    sd = o['m_StreamData']
    if sd['path'].startswith('archive:/'): return bytes(blobs[sd['path'].rsplit('/', 1)[-1]][sd['offset']:sd['offset'] + sd['size']])
    key = os.path.join(os.path.dirname(rel), sd['path']).replace('\\', '/')
    p = os.path.join(root_out, key) if root_out else src_path(key)
    if p not in cache: cache[p] = open(p, 'rb').read()
    return cache[p][sd['offset']:sd['offset'] + sd['size']]


def cmd_verify(a):
    done = load_done()
    files = [f for f, r in done.items() if r.get('written')]
    if a.only: files = [f for f in files if any(s.lower() in f.lower() for s in a.only.split(','))]
    t0 = time.time(); tot = collections.Counter(); bads = []; qs = []
    with ProcessPoolExecutor(a.j) as ex:
        for rel, rep, bad, q in ex.map(verify_file, [(f, a.every) for f in files], chunksize=1):
            tot.update(rep); bads += [f'{rel}: {b}' for b in bad]; qs += q
    by = collections.defaultdict(list)
    for e in qs: by[(e['cls'], e['block'])].append(e)
    psn = {}
    for (cls, b), v in sorted(by.items()):
        p = [e['psnr'] for e in v]
        psn[f'{cls} {b}x{b}'] = dict(n=len(v), mean=round(float(np.mean(p)), 2), min=min(p), worst=min(v, key=lambda e: e['psnr'])['name'])
        if cls == 'nm':
            for ch in ('g', 'a'):
                pc = [e['psnr_' + ch] for e in v]
                psn[f'{cls} {b}x{b}'][ch] = dict(mean=round(float(np.mean(pc)), 2), min=min(pc), max_abs_err=max(e['maxerr_' + ch] for e in v))
    rep = dict(files=len(files), seconds=round(time.time() - t0), totals=dict(tot), problems=len(bads), problem_list=bads[:200], psnr=psn)
    json.dump(rep, open(os.path.join(WORK, 'verify.json'), 'w'), indent=1)
    with open(os.path.join(WORK, 'verify_psnr.jsonl'), 'w') as f:
        for e in qs: f.write(json.dumps(e) + '\n')
    print(json.dumps({k: v for k, v in rep.items() if k != 'problem_list'}, indent=1))
    for b in bads[:30]: print('  PROBLEM', b)
    if bads: sys.exit(f'verify: {len(bads)} problems')


def cmd_formats(a):
    """texture formats: source (scan.jsonl) vs what the player will read with the ASTC set (written files replace their sources)"""
    rows = load_scan(); recs = {}
    p = os.path.join(WORK, 'build.jsonl')
    if os.path.exists(p):
        for l in open(p):
            r = json.loads(l)
            if 'nfmt' in r: recs[(r['file'], r['node'], r['pid'])] = r
    done = load_done()
    before = collections.Counter(); after = collections.Counter(); mb_b = collections.Counter(); mb_a = collections.Counter()
    for r in rows:
        if r.get('notree'): continue
        k = FMT_NAME.get(r['fmt'], str(r['fmt'])); sz = r['inline'] or r['ssize']
        before[k] += 1; mb_b[k] += sz
        c = recs.get((r['file'], r['node'], r['pid']))
        if c and r['file'] in done: k2 = FMT_NAME[c['nfmt']]; sz = c['new']
        else: k2 = k
        after[k2] += 1; mb_a[k2] += sz
    print(f"{'format':18s} {'before':>8s} {'MB':>9s}   {'after':>8s} {'MB':>9s}")
    for k in sorted(set(before) | set(after), key=lambda k: -(mb_b[k] + mb_a[k])):
        print(f'{k:18s} {before[k]:8d} {mb_b[k] / 2**20:9.1f}   {after[k]:8d} {mb_a[k] / 2**20:9.1f}')
    print(f"total {sum(before.values())} -> {sum(after.values())} objects")


def cmd_sets(a):
    files = bc_files()
    class A: pass
    x = A(); x.only = None
    for s in SETS:
        x.files = s; print(s, pick(files, x))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); sp = ap.add_subparsers(dest='cmd', required=True)
    j = max(1, (os.cpu_count() or 4) - 2)
    s = sp.add_parser('scan'); s.add_argument('-j', type=int, default=j)
    m = sp.add_parser('measure'); m.add_argument('--n', type=int, default=24)
    b = sp.add_parser('build'); b.add_argument('-j', type=int, default=j); b.add_argument('--only'); b.add_argument('--files'); b.add_argument('--resume', action='store_true')
    b.add_argument('--block-data', default='4x4'); b.add_argument('--block-color', default='6x6'); b.add_argument('--block-alpha', default='4x4')
    b.add_argument('--quality', default='medium', choices=sorted(QUALITY)); b.add_argument('--psnr-every', type=int, default=0)
    v = sp.add_parser('verify'); v.add_argument('-j', type=int, default=j); v.add_argument('--every', type=int, default=7); v.add_argument('--only')
    sp.add_parser('formats'); sp.add_parser('sets')
    for x in (s, m, b, v, sp.choices['formats'], sp.choices['sets']):
        x.add_argument('--data', help='game Data folder (RomFS/Data), read only'); x.add_argument('--overlay', help='existing overlay Data folder, read only')
        x.add_argument('--out', help='output Data folder (overlay-astc/Data)'); x.add_argument('--work', help='work folder (scan.jsonl, done.jsonl, logs)')
    a = ap.parse_args()
    for k, env in (('data', 'ASTC_SET_DATA'), ('overlay', 'ASTC_SET_OVERLAY'), ('out', 'ASTC_SET_OUT'), ('work', 'ASTC_SET_WORK')):
        if getattr(a, k, None): os.environ[env] = os.path.abspath(getattr(a, k)).replace(chr(92), '/')
    global DATA, OVERLAY, OUT, WORK
    DATA, OVERLAY = os.environ.get('ASTC_SET_DATA', DATA), os.environ.get('ASTC_SET_OVERLAY', OVERLAY)
    OUT, WORK = os.environ.get('ASTC_SET_OUT', OUT), os.environ.get('ASTC_SET_WORK', WORK)
    os.makedirs(WORK, exist_ok=True)
    meta = os.path.join(WORK, 'scan.meta.json'); want = dict(data=DATA, overlay=OVERLAY)
    if a.cmd in ('build', 'measure', 'sets', 'formats') and (not os.path.exists(os.path.join(WORK, 'scan.jsonl')) or not os.path.exists(meta)
                                                          or json.load(open(meta)) != want):
        print('scan.jsonl missing or made from other roots: scanning first', flush=True)
        a.j = getattr(a, 'j', None) or j; cmd_scan(a)
    {'scan': cmd_scan, 'measure': cmd_measure, 'build': cmd_build, 'verify': cmd_verify, 'formats': cmd_formats, 'sets': cmd_sets}[a.cmd](a)


if __name__ == '__main__':
    main()
