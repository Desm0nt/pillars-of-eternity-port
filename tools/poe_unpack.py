#!/usr/bin/env python3
"""poe_unpack.py: the part of nx2android's porter that Pillars of Eternity Mobile needs, without the Android NDK.

porter (third_party/nx2android, MIT) ports a Switch game to its own Android host: it extracts the dump, rewrites the game's
modules (dm/tools/patch_nso.py), translates the shaders to SPIR-V, builds the host with the NDK, packs an APK and a data pack.
This port runs the game in the stock Unity player instead, and needs only the first three of those steps:

  extract   NSZ / XCZ decompression, then nx_extract (.NET) -> <dump folder>/<title id>_build/extract/v<version>/{ExeFS, RomFS}
  rewrite   patch_nso.py for DM_TARGET=android -> android/patched_v<version>/{main.patched.bin, main.fixups.tsv, main.sites.tsv}
            (the x18 thread-slot rewrite of the game code; tools/setup_poe.py builds the game image from it)
  shaders   shader_split.py + the Ryujinx-based translator (dmsh_xlate, .NET) -> android/shaders.dmsv (SPIR-V of every shader)

They are porter's own methods (porter/pipeline.py Port.inputs / extract / check_graphics / rewrite / shaders), called in the same
order porter's Port._run calls them; the host, APK and data-pack steps are not run, so no NDK, JDK or Android SDK is needed.
Needs: Python 3.11+ with pycryptodome, zstd (backports.zstd before 3.14), lz4, capstone; the .NET 10 SDK.

usage: python tools/poe_unpack.py --nx2a DIR --folder DUMPDIR --keys prod.keys --dotnet DOTNET [--home DIR] [--jobs N]
  --nx2a    a copy of third_party/nx2android with this game's profile in nxnative/dm/games/<title id> (setup_poe.py stages it)
  --folder  a folder that holds only the base game and its update (porter writes <folder>/<title id>_build beside them)
  --home    porter's user folder (its translator build cache); default: <folder>/../nx2a-home
"""
import argparse
import os
import sys
from pathlib import Path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nx2a', required=True)
    ap.add_argument('--folder', required=True)
    ap.add_argument('--keys', required=True)
    ap.add_argument('--dotnet', required=True)
    ap.add_argument('--home')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--progress', default='lines', choices=('lines', 'bar', 'off'))
    a = ap.parse_args(argv)

    nx2a = Path(a.nx2a).resolve()
    folder = Path(a.folder).resolve()
    home = Path(a.home).resolve() if a.home else folder.parent / 'nx2a-home'
    home.mkdir(parents=True, exist_ok=True)
    os.environ['NX2A_HOME'] = str(home)            # porter's config.json and translator cache stay in our work folder
    os.environ.pop('DM_GAMES_DIR', None)            # the profile comes from the staged copy's dm/games only
    os.environ.pop('DM_GAMES_FIRST', None)
    sys.path.insert(0, str(nx2a))

    from porter import config, pipeline          # noqa: E402  (third_party/nx2android/porter)
    from porter.upstream import BuildError, PROG, nsp  # noqa: E402

    dotnet = Path(a.dotnet).resolve()
    if not dotnet.is_file():
        print(f'poe_unpack: no dotnet at {dotnet}', file=sys.stderr)
        return 2
    PROG.mode = nsp.progress_mode(a.progress)
    o = pipeline.Options(folder=folder, keys=Path(a.keys).resolve(), jobs=a.jobs)
    p = pipeline.Port(o)
    try:
        # ---- Port._run's set-up, with the toolchain reduced to dotnet (no NDK / adb: no host, APK or install here)
        o.folder = o.folder.resolve()
        if not o.folder.is_dir():
            raise BuildError(f'not a folder: {o.folder}')
        p.tools = {'dotnet': dotnet}
        o.out = o.folder
        os.environ['PATH'] = os.pathsep.join([str(dotnet.parent), os.environ.get('PATH', '')])
        os.environ['DOTNET_ROOT'] = str(dotnet.parent)
        os.environ.setdefault('DOTNET_CLI_TELEMETRY_OPTOUT', '1')
        os.environ.setdefault('DOTNET_NOLOGO', '1')
        p.env = dict(os.environ, J=str(o.jobs), PYTHONIOENCODING='utf-8', PYTHONUTF8='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')
        try:
            p.inputs()
            PROG.plan(1 + len(p.versions) + bool(p.fam['dlc']) + 3)   # nx_extract, the versions, the DLC, rewrite / split / Vulkan shaders
            p.extract()
        finally:
            p.drop_unpacked()
        p.check_graphics()
        p.fps_env, p.fps_files = [], {}
        p.rewrite()
        p.shaders()
        nsp.rmtree(p.work / 'tmp')
        PROG.close(ok=True)
        print(f'poe_unpack: done: {p.work}', flush=True)
        return 0
    except BuildError as e:
        PROG.close(ok=False)
        print(f'poe_unpack: FAILED: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
