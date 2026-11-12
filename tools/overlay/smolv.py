"""SMOL-V, the old encoding (smolv version 0) that Unity 2018.1.4f1's player decodes (libunity.so smolv::Decode @0xa5f664; its table kSpirvOpData, 331 ops x 5 bytes, is smol-v's own: smolv_opdata.py).
Ported from the player's disassembly (dis/smolv.txt). decode(bytes)->SPIR-V bytes, encode(SPIR-V bytes)->bytes."""
import struct, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smolv_opdata import table_bytes   # SMOL-V's own opcode table (public domain), the one the player uses
_T = table_bytes()
NOPS = len(_T) // 5
def _has_result(op): return op < NOPS and _T[5 * op] != 0
def _has_type(op): return op < NOPS and _T[5 * op + 1] != 0
def _delta(op):
    if op >= NOPS: return 0
    v = _T[5 * op + 2]; return v - 256 if v >= 128 else v      # negative: zigzag-coded
def _varrest(op): return op < NOPS and _T[5 * op + 3] != 0
_SWAP = [(71, 0), (61, 1), (62, 2), (65, 3), (79, 4), (72, 7), (248, 8), (59, 9), (133, 10), (129, 11), (32, 14), (127, 15)]
_REMAP = {}
for a, b in _SWAP: _REMAP[a] = b; _REMAP[b] = a
def _remap(op): return _REMAP.get(op, op)
_LENADJ = {79: 4, 13: 4, 71: 2, 61: 3, 65: 3}
MAGIC = 0x534D4F4C
def _zig_d(v): return ((v >> 1) ^ (-(v & 1) & 0xffffffff)) & 0xffffffff
def _zig_e(v):
    v &= 0xffffffff
    return ((v << 1) & 0xffffffff) ^ (0xffffffff if v & 0x80000000 else 0)

def decode(b):
    magic, ver, gen, bound, schema, size = struct.unpack_from('<6I', b, 0)
    if magic != MAGIC: raise ValueError('not SMOL-V')
    if ver | 0x100 != 0x00010100: raise ValueError(f'SMOL-V version {ver:#x} the 2018.1 player refuses')
    out = [0x07230203, ver, gen, bound, schema]
    p = 24; n = len(b); prev_res = prev_dec = 0
    def varint():
        nonlocal p
        v = s = 0
        while p < n:
            c = b[p]; p += 1; v |= (c & 0x7f) << s; s += 7
            if not c & 0x80: break
        return v & 0xffffffff
    while p < n:
        val = varint()
        ln = ((val >> 20) << 4) | ((val >> 4) & 0xf); op0 = _remap(((val >> 4) & 0xfff0) | (val & 0xf))
        ln += 1 + _LENADJ.get(op0, 0)
        op = 79 if op0 == 13 else op0
        ins = [op | ln << 16]
        if _has_type(op): ins.append(varint())
        if _has_result(op): prev_res = (prev_res + _zig_d(varint())) & 0xffffffff; ins.append(prev_res)
        if op in (71, 72): prev_dec = (prev_dec + varint()) & 0xffffffff; ins.append(prev_dec)
        d = _delta(op)
        for _ in range(abs(d)):
            if len(ins) >= ln: break
            v = varint()
            if d < 0: v = _zig_d(v)
            ins.append((prev_res - v) & 0xffffffff)
        if op0 == 13 and ln <= 9:
            s = b[p]; p += 1
            ins += [s >> 6, (s >> 4) & 3, (s >> 2) & 3, s & 3][:max(0, ln - 5)]
        elif _varrest(op):
            while len(ins) < ln: ins.append(varint())
        else:
            while len(ins) < ln: ins.append(struct.unpack_from('<I', b, p)[0]); p += 4
        out += ins
    r = struct.pack(f'<{len(out)}I', *out)
    if len(r) != size: raise ValueError(f'SMOL-V decoded to {len(r)} bytes, header says {size}')
    return r

def encode(spv):
    w = struct.unpack(f'<{len(spv) // 4}I', spv)
    if w[0] != 0x07230203: raise ValueError('not SPIR-V')
    if w[1] | 0x100 != 0x00010100: raise ValueError(f'SPIR-V version {w[1]:#x}: the 2018.1 player decodes only 1.0 / 1.1 headers')
    out = bytearray(struct.pack('<6I', MAGIC, w[1], w[2], w[3], w[4], len(spv)))
    def varint(v):
        v &= 0xffffffff
        while v > 127: out.append((v & 127) | 128); v >>= 7
        out.append(v)
    i = 5; prev_res = prev_dec = 0
    while i < len(w):
        ln, op = w[i] >> 16, w[i] & 0xffff
        if ln < 1 or i + ln > len(w): raise ValueError('malformed SPIR-V')
        ins = w[i:i + ln]; i += ln
        swz = None; eop = op
        if op == 79 and ln <= 9 and ln >= 5 and all(x < 4 for x in ins[5:]):
            eop = 13; c = list(ins[5:]) + [0] * 4; swz = c[0] << 6 | c[1] << 4 | c[2] << 2 | c[3]
        el = ln - 1 - _LENADJ.get(eop, 0)
        if el < 0 or el > 0xffff: raise ValueError('instruction shorter than its fixed part')
        rop = _remap(eop)
        varint(((el >> 4) << 20) | ((rop >> 4) << 8) | ((el & 0xf) << 4) | (rop & 0xf))
        k = 1
        if _has_type(op): varint(ins[k]); k += 1
        if _has_result(op): varint(_zig_e(ins[k] - prev_res)); prev_res = ins[k]; k += 1
        if op in (71, 72): varint(ins[k] - prev_dec); prev_dec = ins[k]; k += 1
        d = _delta(op)
        for _ in range(abs(d)):
            if k >= ln: break
            v = (prev_res - ins[k]) & 0xffffffff
            varint(_zig_e(v) if d < 0 else v); k += 1
        if swz is not None: out.append(swz)
        elif _varrest(op):
            for x in ins[k:]: varint(x)
        else:
            for x in ins[k:]: out += struct.pack('<I', x)
    return bytes(out)
