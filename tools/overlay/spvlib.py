"""Minimal SPIR-V module model: parse to instruction list, edit, serialize."""
import struct, collections
class Mod:
    def __init__(s, b):
        w = struct.unpack(f'<{len(b) // 4}I', b)
        assert w[0] == 0x07230203
        s.ver, s.gen, s.bound, s.schema = w[1:5]
        s.ins = []; i = 5
        while i < len(w):
            n, op = w[i] >> 16, w[i] & 0xffff
            assert n > 0 and i + n <= len(w)
            s.ins.append([op] + list(w[i + 1:i + n])); i += n
    def bytes(s):
        out = [0x07230203, s.ver, s.gen, s.bound, s.schema]
        for x in s.ins: out.append((len(x) << 16) | x[0]); out += x[1:]
        return struct.pack(f'<{len(out)}I', *out)
    def new_id(s): s.bound += 1; return s.bound - 1
    def names(s):
        return {x[1]: wstr(x[2:]) for x in s.ins if x[0] == 5}
    def decos(s):
        d = collections.defaultdict(dict)
        for x in s.ins:
            if x[0] == 71: d[x[1]][x[2]] = x[3:] if len(x) > 3 else [1]
        return d
def wstr(ws):
    return struct.pack(f'<{len(ws)}I', *ws).split(b'\0')[0].decode('utf8', 'replace')
def strw(t):
    b = t.encode() + b'\0'; b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))
