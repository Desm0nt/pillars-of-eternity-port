"""Shared loaders: nx2android's unityfs (third_party/nx2android), Unity common strings, the Shader type tree (2018.1.4f1), file enumeration.

Everything game-specific comes from the player's own dump; tools/setup_poe.py sets these for every tool it runs:
  ASTC_SET_DATA     the dump's RomFS/Data
  POE_MAIN          the dump's ExeFS/main (Unity's common string table is read out of it, once, into POE_CACHE/common_strings.pkl)
  POE_CACHE         a work folder for those caches (default: <repo>/work/cache)
  POE_SHADER_TREE   a file of the dump that carries the Shader type tree (default: Data/StreamingAssets/assetbundles/objectbundle;
                    the loose .assets files carry no type trees, the game's asset bundles do, with the same type hash)
  POE_NXNATIVE      nx2android's nxnative folder (default: <repo>/third_party/nx2android/nxnative)
"""
import sys, os, pickle
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.dirname(os.path.dirname(HERE))
REF = os.environ.get('POE_NXNATIVE', os.path.join(REPO, 'third_party', 'nx2android', 'nxnative'))
sys.path.insert(0, REF + '/dm/gfx/tools'); sys.path.insert(0, REF + '/dm/tools')
import unityfs
DATA = os.environ.get('ASTC_SET_DATA', '')
CACHE = os.environ.get('POE_CACHE', os.path.join(REPO, 'work', 'cache'))
SHADER_TREE_SRC = os.environ.get('POE_SHADER_TREE') or os.path.join(DATA, 'StreamingAssets', 'assetbundles', 'objectbundle')
_c = os.path.join(CACHE, 'common_strings.pkl')
_common = None
def common():
    global _common
    if _common is not None: return _common
    if os.path.exists(_c):
        _common = pickle.load(open(_c, 'rb')); return _common
    main = os.environ.get('POE_MAIN', '')
    if not main or not os.path.isfile(main):
        raise SystemExit(f'ucommon: no {_c} yet and POE_MAIN does not name the dump\'s ExeFS/main ({main!r})')
    import nso_syms
    _common = unityfs.common_strings(nso_syms.load(main))
    os.makedirs(CACHE, exist_ok=True); tmp = _c + f'.{os.getpid()}.tmp'
    pickle.dump(_common, open(tmp, 'wb')); os.replace(tmp, _c)
    return _common
def sers(path):
    d = open(path, 'rb').read()
    return unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [(os.path.basename(path), d)]
def shader_tree(path=None):
    """(class 48 hash, tree root) from a file that carries type trees (the game's own object bundle)"""
    path = path or SHADER_TREE_SRC
    t = {}
    for nm, x in sers(path):
        if not unityfs.is_serialized(x): continue
        for _ in unityfs.serialized_objects(x, common(), {48}, t): break
    if not t: raise SystemExit(f'ucommon: no Shader type tree in {path}')
    return t
def shaders(path, trees):
    for nm, x in sers(path):
        if not unityfs.is_serialized(x): continue
        for cid, pid, o in unityfs.serialized_objects(x, common(), {48}, trees):
            yield nm, pid, o
if __name__ == '__main__':   # setup runs this once before the tools (fills the cache, checks the Shader tree)
    c = common(); t = shader_tree()
    print(f'ucommon: {len(c)} common strings, Shader type tree {next(iter(t))[1].hex()} from {SHADER_TREE_SRC}')
