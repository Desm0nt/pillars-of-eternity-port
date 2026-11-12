"""Unity type-tree hash (the 16-byte "old type hash" stored per type in a serialized file) -- MD4 over the tree.
   tree_hash(nodes) -> hex      nodes = flat list of dicts: level, type, name, size, flags (meta flag), version, typeflags
   Recipe (calibrated against every tree-carrying type of the 2018.1.4f1 Android/Vulkan reference bundles, see __main__):
   per node, depth first: type string, name string, int32 byte size, int32 type flags, int32 version, int32 (meta flag & 0x4000).
   raw_types(file bytes) -> [(class id, stored hash hex, nodes or None, script id)] for a serialized file (format 17..21)."""
import struct, sys, os

def _md4(data):
    def lrot(x, n): return ((x << n) | (x >> (32 - n))) & 0xffffffff
    msg = bytearray(data); ml = len(data) * 8
    msg.append(0x80)
    while len(msg) % 64 != 56: msg.append(0)
    msg += struct.pack('<Q', ml)
    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    for off in range(0, len(msg), 64):
        X = struct.unpack_from('<16I', msg, off); aa, bb, cc, dd = a, b, c, d
        for i in range(16):
            k = i; s = (3, 7, 11, 19)[i % 4]
            a = lrot((a + ((b & c) | (~b & d)) + X[k]) & 0xffffffff, s); a, b, c, d = d, a, b, c
        for i in range(16):
            k = (i % 4) * 4 + i // 4; s = (3, 5, 9, 13)[i % 4]
            a = lrot((a + ((b & c) | (b & d) | (c & d)) + X[k] + 0x5a827999) & 0xffffffff, s); a, b, c, d = d, a, b, c
        order = (0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)
        for i in range(16):
            k = order[i]; s = (3, 9, 11, 15)[i % 4]
            a = lrot((a + (b ^ c ^ d) + X[k] + 0x6ed9eba1) & 0xffffffff, s); a, b, c, d = d, a, b, c
        a = (a + aa) & 0xffffffff; b = (b + bb) & 0xffffffff; c = (c + cc) & 0xffffffff; d = (d + dd) & 0xffffffff
    return struct.pack('<4I', a, b, c, d)

def tree_hash(nodes):
    buf = bytearray()
    for n in nodes:
        buf += n['type'].encode('utf-8'); buf += n['name'].encode('utf-8')
        buf += struct.pack('<iiii', n['size'], n.get('typeflags', 0), n.get('version', 1), n['flags'] & 0x4000)
    return _md4(bytes(buf)).hex()

def raw_types(d, common=None):
    msz, fsz, ver, doff = struct.unpack_from('>IIII', d, 0)
    assert 17 <= ver <= 21, ver
    le = d[16] == 0; e = '<' if le else '>'
    p = 20; z = d.index(b'\0', p); p = z + 1
    plat, = struct.unpack_from(e + 'i', d, p); p += 4
    tt = d[p]; p += 1
    ntypes, = struct.unpack_from(e + 'i', d, p); p += 4; out = []
    for _ in range(ntypes):
        cid, = struct.unpack_from(e + 'i', d, p); p += 4
        stripped = d[p]; p += 1
        sidx, = struct.unpack_from(e + 'h', d, p); p += 2
        sid = None
        if cid == 114: sid = d[p:p + 16].hex(); p += 16
        h = d[p:p + 16].hex(); p += 16; nodes = None
        if tt:
            nn, sb = struct.unpack_from(e + 'ii', d, p); p += 8
            rsz = 32 if ver >= 19 else 24
            raw = [struct.unpack_from(e + 'HBBIIiiI', d, p + rsz * i) for i in range(nn)]; p += rsz * nn
            buf = d[p:p + sb]; p += sb
            def st(o):
                if o & 0x80000000:
                    return common.get(o & 0x7fffffff, f'?{o & 0x7fffffff}') if common else f'?{o & 0x7fffffff}'
                return buf[o:buf.index(b'\0', o)].decode('utf-8', 'replace')
            nodes = [dict(level=lv, type=st(to), name=st(no), size=bs, index=ix, flags=mf, version=v, typeflags=tf) for v, lv, tf, to, no, bs, ix, mf in raw]
            if ver >= 21:
                n, = struct.unpack_from(e + 'i', d, p); p += 4 + 4 * n
        out.append((cid, h, nodes, sid))
    return dict(platform=plat, tt=tt, types=out, ver=ver)

if __name__ == '__main__':   # check the recipe against files that carry type trees: python typehash.py <bundle or serialized file>...
    from ucommon import common, sers, unityfs
    ok = bad = 0
    for path in sys.argv[1:]:
        for nm, x in sers(path):
            if not unityfs.is_serialized(x): continue
            for cid, h, nodes, sid in raw_types(x, common())['types']:
                if nodes is None or cid == 114: continue
                if tree_hash(nodes) == h: ok += 1
                else: bad += 1; print('MISMATCH', path, cid, h, tree_hash(nodes))
    print('type hash recipe: ok', ok, 'bad', bad)
