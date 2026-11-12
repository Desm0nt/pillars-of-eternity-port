"""Unity 2018.1.4f1 binary subprogram blob (what LoadGpuProgramFromData @0x9c1c5c parses; version 201708220), and the per-platform blob (u32 count, {offset,length} x count, LZ4).
parse(bytes) -> dict, build(dict) -> bytes (byte-exact round trip on Unity's own blobs)."""
import struct
VERSION = 201708220
class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u(s): v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v
    def i(s): v = struct.unpack_from('<i', s.b, s.p)[0]; s.p += 4; return v
    def s(s):
        n = s.u(); v = s.b[s.p:s.p + n]; s.p += (n + 3) & ~3; return v.decode('utf8')
def _param(r):
    name = r.s(); t, rows, dim, ismat, arr, idx = r.i(), r.i(), r.i(), r.i(), r.i(), r.i()
    return dict(name=name, type=t, rows=rows, dim=dim, matrix=ismat, array=arr, index=idx)
def parse(b):
    r = R(b); o = {}
    o['version'], o['type'] = r.u(), r.u(); o['stats'] = [r.u() for _ in range(4)]
    assert o['version'] == VERSION, o['version']
    o['keywords'] = [r.s() for _ in range(r.u())]
    n = r.u(); o['code'] = b[r.p:r.p + n]; r.p += (n + 3) & ~3
    o['sourceMap'] = r.u(); o['channels'] = [(r.u(), r.u()) for _ in range(r.u())]
    o['groups'] = []
    for g in range(r.u()):
        grp = dict(name=r.s(), size=r.i()); grp['params'] = [_param(r) for _ in range(r.u())]
        grp['structs'] = []
        for _ in range(r.u()):
            st = dict(name=r.s(), index=r.i(), array=r.i(), size=r.i()); st['members'] = [_param(r) for _ in range(r.u())]
            grp['structs'].append(st)
        o['groups'].append(grp)
    o['bindings'] = []
    for _ in range(r.u()):
        e = dict(name=r.s(), type=r.i(), index=r.i(), extra=r.i())
        if e['type'] == 0: e['ms'] = r.u()
        o['bindings'].append(e)
    o['_end'] = r.p
    return o
def _ws(out, t):
    b = t.encode('utf8'); out += struct.pack('<I', len(b)) + b + b'\0' * (-len(b) % 4)
def _wparam(out, p):
    _ws(out, p['name']); out += struct.pack('<6i', p['type'], p['rows'], p['dim'], p['matrix'], p['array'], p['index'])
def build(o):
    out = bytearray(struct.pack('<6I', o.get('version', VERSION), o['type'], *o.get('stats', [0, 0, 0, 0])))
    out += struct.pack('<I', len(o['keywords']))
    for k in o['keywords']: _ws(out, k)
    c = o['code']; out += struct.pack('<I', len(c)) + c + b'\0' * (-len(c) % 4)
    out += struct.pack('<II', o['sourceMap'], len(o['channels']))
    for s, t in o['channels']: out += struct.pack('<II', s, t)
    out += struct.pack('<I', len(o['groups']))
    for g in o['groups']:
        _ws(out, g['name']); out += struct.pack('<iI', g['size'], len(g['params']))
        for p in g['params']: _wparam(out, p)
        out += struct.pack('<I', len(g['structs']))
        for st in g['structs']:
            _ws(out, st['name']); out += struct.pack('<3iI', st['index'], st['array'], st['size'], len(st['members']))
            for p in st['members']: _wparam(out, p)
    out += struct.pack('<I', len(o['bindings']))
    for e in o['bindings']:
        _ws(out, e['name']); out += struct.pack('<3i', e['type'], e['index'], e['extra'])
        if e['type'] == 0: out += struct.pack('<I', e.get('ms', 0))
    return bytes(out)

def split_platform_blob(seg):
    n = struct.unpack_from('<I', seg, 0)[0]
    return [seg[o:o + l] for o, l in (struct.unpack_from('<II', seg, 4 + 8 * k) for k in range(n))]
def join_platform_blob(subs):
    """u32 count, {offset, length} x count, then the subprograms each 4-aligned (as Unity lays them out)"""
    off = 4 + 8 * len(subs); table = b''; body = b''
    for s in subs:
        table += struct.pack('<II', off + len(body), len(s)); body += s + b'\0' * (-len(s) % 4)
    return struct.pack('<I', len(subs)) + table + body
