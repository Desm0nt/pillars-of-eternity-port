import struct, mmap
def zstd_mod():
    try: from compression import zstd
    except ImportError:
        from backports import zstd
    return zstd
class Dmsv:
    def __init__(s, path):
        s.d = open(path,'rb').read()
        magic, s.ver, n = struct.unpack_from('<4sII', s.d, 0)
        assert magic == b'DMSV'
        s.e = {}
        for i in range(n):
            key, st, so, sl, ro, rl, ssz = struct.unpack_from('<QIIIIII', s.d, 12 + 32*i)
            s.e[key] = (st, so, sl, ro, rl, ssz)
    def spv(s, key):
        st, so, sl, ro, rl, ssz = s.e[key]
        b = s.d[so:so+sl]
        if ssz: b = zstd_mod().decompress(b)
        return b
    def refl(s, key):
        st, so, sl, ro, rl, ssz = s.e[key]
        b = s.d[ro:ro+rl]
        nbind, c1len, l0, l1, l2 = struct.unpack_from('<5I', b, 0)
        binds = []
        for i in range(nbind):
            kind, desc, dim, flags, binding, count, reg, samp, fmt, off, sarg = struct.unpack_from('<BBBBHHhBBii', b, 20 + 20*i)
            binds.append(dict(kind=kind, desc=desc, dim=dim, flags=flags, binding=binding, count=count, reg=reg, samp=samp, fmt=fmt, off=off, sarg=sarg))
        c1 = b[20+20*nbind:20+20*nbind+c1len]
        return dict(stage=st, hdr=(l0,l1,l2), binds=binds, c1=c1)
