"""set_platform.py : header target platform 38 -> 13 in the shader-converted overlay files (in place; 4 bytes each; idempotent).
Loose files: the int after the Unity version string.  Bundles (stored block, uncompressed block info written by sfile.repack_bundle): the same field of
every serialized node.  Not needed once the player's check is patched (data/platform_check.md), but it lets these files load on an unpatched player and
keeps the overlay self-consistent."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gm import *
import bundle_info
NUL = bytes(1)

def patch_at(f, base):
    """base = file offset of a serialized file. Returns the old platform or None if it is not a format-17 serialized file."""
    f.seek(base); head = f.read(4096)
    if len(head) < 48 or struct.unpack_from('>I', head, 8)[0] != 17: return None
    fmt = '<i' if head[16] == 0 else '>i'
    off = head.index(NUL, 20) + 1
    old, = struct.unpack_from(fmt, head, off)
    f.seek(base + off); f.write(struct.pack(fmt, 13))
    return old

for rel in CONVERTED_LOOSE:
    with open(os.path.join(OVERLAY, rel), 'r+b') as f:
        print(rel, 'platform', patch_at(f, 0), '-> 13')
for brel in CONVERTED_BUNDLES:
    bp = os.path.join(OVERLAY, brel)
    r = bundle_info.info(bp, verbose=False)
    assert r['flags'] & 0x3f == 0 and not r['flags'] & 0x80 and r['nblocks'] == 1 and not r['blockflags'].get(64), 'expected a bundle written by sfile.repack_bundle'
    data0 = r['hdr_end'] + r['csz']
    with open(bp, 'r+b') as f:
        for nm, off, sz, fl in r['nodes']:
            if '.' in nm: continue
            f.seek(data0 + off); head = f.read(8)
            if struct.unpack_from('>I', head, 4)[0] != sz: continue
            print(brel + ':' + nm, 'platform', patch_at(f, data0 + off), '-> 13')
