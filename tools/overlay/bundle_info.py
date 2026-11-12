"""UnityFS header / blocks / nodes of a bundle."""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gm import *
def info(path, verbose=True):
    d = open(path, 'rb').read(); assert d[:8] == b'UnityFS\0'
    fmt, = struct.unpack_from('>I', d, 8); v1, p = sfile._cstr(d, 12); v2, p = sfile._cstr(d, p)
    size, csz, usz, flags = struct.unpack_from('>qIII', d, p); hdr_end = p + 20
    raw = d[len(d) - csz:] if flags & 0x80 else d[hdr_end:hdr_end + csz]
    inf = unityfs._decomp(raw, usz, flags & 0x3f)
    q = 16; nb, = struct.unpack_from('>i', inf, q); q += 4; blocks = []
    for _ in range(nb):
        u, c, f = struct.unpack_from('>IIH', inf, q); q += 10; blocks.append((u, c, f))
    nn, = struct.unpack_from('>i', inf, q); q += 4; nodes = []
    for _ in range(nn):
        off, sz, f = struct.unpack_from('>qqI', inf, q); q += 20; nm, q = sfile._cstr(inf, q); nodes.append((nm.decode(), off, sz, f))
    r = dict(fmt=fmt, v1=v1, v2=v2, size=size, filesize=len(d), flags=flags, nblocks=nb, blockflags=collections.Counter(b[2] for b in blocks), maxblock=max(b[0] for b in blocks), nodes=nodes, hdr_end=hdr_end, csz=csz)
    if verbose: print(path); print('  ', {k: v for k, v in r.items()})
    return r
if __name__ == '__main__':
    for p in sys.argv[1:]: info(p)
