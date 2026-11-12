"""Unity 2018.1.4f1 release type trees of the classes in the game's globalgamemanagers (the Switch build stores none in that file).

unity_types_2018.1.4.json holds, for each of the 16 classes tools/overlay/ggm_patch.py rewrites or round-trips, the flat node list
of its release type tree: [level, type, name, byte size, meta flags, version, type flags]. They are facts about Unity's serialization
layout, taken from the type-tree database AssetRipper embeds (docs/THIRD_PARTY.md); gm.tree_for() recomputes each tree's hash
(typehash.py) and refuses one that does not equal the hash the player's own file stores.
   release_tree(class_id, version=(2018,1,4)) -> flat node list of dicts (level, type, name, size, index, flags, version, typeflags)"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, 'unity_types_2018.1.4.json')
_db = None
def load():
    global _db
    if _db is None:
        _db = json.load(open(DB, encoding='utf-8'))
    return _db
def _check(version):
    if tuple(version) != (2018, 1, 4): raise KeyError(f'only Unity 2018.1.4 trees are stored, not {version}')
def release_tree(cid, version=(2018, 1, 4)):
    _check(version); c = load()['classes'].get(str(cid))
    if not c: return None
    return [dict(level=lv, type=t, name=n, size=sz, index=i, flags=mf, version=v, typeflags=tf)
            for i, (lv, t, n, sz, mf, v, tf) in enumerate(c['nodes'])]
def class_name(cid, version=(2018, 1, 4)):
    _check(version); c = load()['classes'].get(str(cid)); return c['name'] if c else None
if __name__ == '__main__':
    import sys
    cid = int(sys.argv[1]) if len(sys.argv) > 1 else 141
    print(class_name(cid))
    for n in release_tree(cid): print('  ' * n['level'] + f"{n['type']} {n['name']}  size {n['size']} flags {n['flags']:#x}")
