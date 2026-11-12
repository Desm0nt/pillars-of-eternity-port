"""Helpers: release type tree (tpk.py) -> unityfs Node tree; parse / rebuild objects of tree-less serialized files (format 17)."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import typehash, tpk
from ucommon import unityfs, common, sers, REPO, CACHE
import sfile
# tools/setup_poe.py sets these: the player's RomFS/Data and the overlay being built (its work folder)
DATA = os.environ.get('ASTC_SET_DATA', '')
OVERLAY = os.environ.get('ASTC_SET_OVERLAY', os.path.join(REPO, 'work', 'setup', 'overlay', 'Data'))

def node_tree(flat):
    root = None; stack = []
    for n in flat:
        nd = unityfs.Node(n['type'], n['name'], n['size'], n['flags'])
        del stack[n['level']:]
        if stack: stack[-1].kids.append(nd)
        else: root = nd
        stack.append(nd)
    return root
_trees = {}
def tree_for(cid, h=None):
    """Node tree for a class; if h (hex) is given the TPK tree's recomputed hash must equal it"""
    if cid not in _trees:
        f = tpk.release_tree(cid); _trees[cid] = (node_tree(f), typehash.tree_hash(f)) if f else (None, None)
    t, th = _trees[cid]
    if t is None: raise KeyError(f'class {cid}: no release tree in the database')
    if h is not None and th != h: raise ValueError(f'class {cid}: stored hash {h} != database tree hash {th}')
    return t
def read_obj(d, s, pid_entry, tree):
    pid, start, size, ti, ent = pid_entry
    r = unityfs.Reader(d[s['doff'] + start:s['doff'] + start + size], s['le'])
    o = unityfs._read(r, tree)
    return o, r.p, size
def objects(d):
    s = sfile.parse_serialized(d); return s
def externals(d):
    """externals list of a format-17 file: [(guid hex, type, path)] ; also returns script types count. Parsed after the object table."""
    s = sfile.parse_serialized(d); e = '<' if s['le'] else '>'
    p = s['objs'][-1][4] + 20 if s['objs'] else None
    if p is None: raise ValueError('no objects')
    n, = struct.unpack_from(e + 'i', d, p); p += 4          # script types
    for _ in range(n):
        p = (p + 3) & ~3; p += 4 + 8
    n, = struct.unpack_from(e + 'i', d, p); p += 4; ext = []
    for _ in range(n):
        z = d.index(b'\0', p); p = z + 1                    # asset path (empty)
        guid = d[p:p + 16].hex(); p += 16
        t, = struct.unpack_from(e + 'i', d, p); p += 4
        z = d.index(b'\0', p); path = d[p:z].decode(); p = z + 1
        ext.append((guid, t, path))
    return ext

# every game file that holds Shader objects (tools/data/scan_bundles.py found them; the f2-shaders pass knew only the first four + unity default resources)
CONVERTED_LOOSE = ['Resources/unity_builtin_extra', 'sharedassets1.assets', 'resources.assets', 'globalgamemanagers.assets', 'sharedassets0.assets',
                   'sharedassets199.assets', 'sharedassets3.assets']
CONVERTED_BUNDLES = ['StreamingAssets/assetbundles/objectbundle', 'StreamingAssets/gripdigitaldebugging']
