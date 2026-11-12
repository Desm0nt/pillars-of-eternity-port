"""Write side for the nx2android unityfs reader: object dict -> bytes by type tree; serialized file (format 17) with replaced objects; UnityFS bundle repack (stored blocks)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ucommon import unityfs
PRIM = unityfs.PRIM

def write_obj(n, v, out, e='<'):
    def align():
        out.extend(b'\0' * (-len(out) % 4))
    if n.type in PRIM and not n.kids:
        out += struct.pack(e + PRIM[n.type], v)
    elif n.type == 'string':
        b = v.encode('utf-8') if isinstance(v, str) else bytes(v)
        out += struct.pack(e + 'i', len(b)) + b
        if n.kids and n.kids[0].flags & 0x4000: align()
    elif n.kids and n.kids[0].type == 'Array':
        arr = n.kids[0]; el = arr.kids[1]
        out += struct.pack(e + 'i', len(v))
        if el.type in ('UInt8', 'char', 'SInt8') and not el.kids: out += bytes(v)
        elif el.type in PRIM and not el.kids: out += struct.pack(e + PRIM[el.type] * len(v), *v)
        else:
            for x in v: write_obj(el, x, out, e)
        if arr.flags & 0x4000: align()
    elif n.type == 'TypelessData':
        out += struct.pack(e + 'i', len(v)) + bytes(v)
    else:
        for k in n.kids: write_obj(k, v[k.name], out, e)
    if n.flags & 0x4000: align()

def obj_bytes(root, o, le=True):
    out = bytearray(); write_obj(root, o, out, '<' if le else '>'); return bytes(out)

def parse_serialized(d):
    """format 17 serialized file -> header fields, types [(cid, hash)], objects [(pid, start, size, type index, table entry offset)]"""
    msz, fsz, ver, doff = struct.unpack_from('>IIII', d, 0)
    assert ver == 17, ver
    le = d[16] == 0; e = '<' if le else '>'
    p = 20; z = d.index(b'\0', p); uver = d[p:z].decode(); p = z + 1
    plat_off = p; plat, = struct.unpack_from(e + 'i', d, p); p += 4
    tt = d[p]; p += 1
    ntypes, = struct.unpack_from(e + 'i', d, p); p += 4; types = []
    for _ in range(ntypes):
        cid, = struct.unpack_from(e + 'i', d, p); p += 4 + 1 + 2
        if cid == 114: p += 16
        h = d[p:p + 16]; p += 16
        if tt:
            nn, sb = struct.unpack_from(e + 'ii', d, p); p += 8 + 24 * nn + sb
        types.append((cid, h))
    nobj, = struct.unpack_from(e + 'i', d, p); p += 4; objs = []
    for _ in range(nobj):
        p = (p + 3) & ~3
        pid, start, size, ti = struct.unpack_from(e + 'qIIi', d, p)
        objs.append((pid, start, size, ti, p)); p += 20
    return dict(msz=msz, fsz=fsz, doff=doff, le=le, uver=uver, plat=plat, plat_off=plat_off, tt=tt, types=types, objs=objs)

def replace_objects(d, repl, platform=None):
    """serialized file bytes + {path ID: new object bytes} -> new file bytes. Objects are laid out again in their original order, each 8-aligned (as Unity writes them);
    the metadata keeps its size, only the object table (start, size) and the header file size change. platform: optional new target platform id."""
    s = parse_serialized(d); e = '<' if s['le'] else '>'
    head = bytearray(d[:s['doff']]); body = bytearray()
    for pid, start, size, ti, ent in sorted(s['objs'], key=lambda o: o[1]):
        body.extend(b'\0' * (-len(body) % 8))
        data = repl[pid] if pid in repl else d[s['doff'] + start:s['doff'] + start + size]
        struct.pack_into(e + 'II', head, ent + 8, len(body), len(data))
        body += data
    if platform is not None: struct.pack_into(e + 'i', head, s['plat_off'], platform)
    out = head + body
    struct.pack_into('>I', out, 4, len(out))
    return bytes(out)

def _cstr(d, p):
    z = d.index(b'\0', p); return d[p:z], z + 1

def repack_bundle(d, files):
    """UnityFS bundle bytes + {node path: new bytes} -> new bundle with one stored (uncompressed) block and an uncompressed block info right after the header"""
    assert d[:8] == b'UnityFS\0'
    fmt, = struct.unpack_from('>I', d, 8)
    v1, p = _cstr(d, 12); v2, p = _cstr(d, p)
    size, csz, usz, flags = struct.unpack_from('>qIII', d, p); hdr_end = p + 20
    nodes = unityfs.bundle_files(d)
    info_raw = d[len(d) - csz:] if flags & 0x80 else d[hdr_end:hdr_end + csz]
    info = unityfs._decomp(info_raw, usz, flags & 0x3f)
    q = 16; nb, = struct.unpack_from('>i', info, q); q += 4 + 10 * nb
    nn, = struct.unpack_from('>i', info, q); q += 4; nflags = []
    for _ in range(nn):
        off, sz, f = struct.unpack_from('>qqI', info, q); q += 20; nm, q = _cstr(info, q); nflags.append(f)
    datas = [files.get(nm, x) for nm, x in nodes]
    binfo = bytearray(info[:16]); total = sum(len(x) for x in datas)
    assert total < 2**32
    binfo += struct.pack('>i', 1) + struct.pack('>IIH', total, total, 0)
    binfo += struct.pack('>i', len(nodes)); off = 0
    for (nm, _), x, f in zip(nodes, datas, nflags):
        binfo += struct.pack('>qqI', off, len(x), f) + nm.encode() + b'\0'; off += len(x)
    hflags = flags & ~0x3f & ~0x80 & ~0x200
    head = bytearray(b'UnityFS\0' + struct.pack('>I', fmt) + v1 + b'\0' + v2 + b'\0')
    full = len(head) + 20 + len(binfo) + total
    head += struct.pack('>qIII', full, len(binfo), len(binfo), hflags)
    return bytes(head + binfo + b''.join(datas))
