#!/usr/bin/env python3
"""setup_poe.py: Pillars of Eternity Mobile setup. Prepares the game folder for the phone from YOUR OWN Pillars of Eternity:
Complete Edition (Switch) dumps and console keys. Started by setup.bat / setup.sh (which provide Python and its packages).

  setup.bat | ./setup.sh                    finds the base game, the update and prod.keys in put-game-files-here/ (any names)
  setup.bat FILES...                        the same with explicit files or folders (NSP / NSZ / XCI / XCZ + prod.keys)
  setup.bat check                           what is found / installed / prepared, changes nothing
  options: --texture bc|astc|both  --out DIR  --work DIR  --keys FILE  --steps unpack,image,overlay,astc,layout  --jobs N  --yes

What it needs: base game 0100D6200E130000 v0 and update 0100D6200E130800 v393216 (2.60.00), prod.keys of your console; Python 3.11+
(setup.bat / setup.sh prepare it), the .NET 10 SDK (offered into tools/.runtime/dotnet when missing), internet for the first run
(Python packages, the .NET SDK, the translator's NuGet packages), about 90 GB free during the run, 20-40 minutes.

STEPS (each resumable: a later run skips what is already there)
  1. unpack   tools/poe_unpack.py: nx2android's porter (third_party/nx2android, MIT) without its NDK steps -- decompresses and
              extracts the dumps (ExeFS, RomFS), rewrites the game's main for Android (patch_nso.py: the x18 thread-slot sites and
              the profile's patches in profile/0100D6200E130000/patches.py) and translates the game's shaders to SPIR-V
              (shaders.dmsv). Work folder: <work>/dump/0100D6200E130000_build (hard links of your files in <work>/dump).
  2. image    game/libf2game.so: the flat game image the app maps (decompressed main + the x18 game-site rewrite).
  3. overlay  overlay/Data: the shader-carrying game files rewritten for the Android Vulkan player (tools/overlay/run_convert.py),
              globalgamemanagers set to Vulkan / landscape (ggm_patch.py), header platform 13 (set_platform.py), checked
              (verify_overlay.py; spirv-val when one is found).
  4. astc     overlay-astc/Data: the BC textures re-encoded as ASTC for GPUs without BC (Mali, PowerVR, Xclipse), astc_set.py.
  5. layout   <out> = PillarsOfEternityMobile/{RomFS/Data, overlay/Data, [overlay-astc/Data], game/libf2game.so, save/, README.txt},
              by hard link from the work folder (no second copy on the same drive). Copy it to the phone as
              Download/PillarsOfEternityMobile.
--texture: bc = Adreno only (smallest); astc = every GPU, no file twice; both = every GPU, largest. Not given: asked.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent               # <repo>/tools
REPO = HERE.parent
OVL = HERE / 'overlay'                                # the overlay / ASTC tools
NX2A_SRC = REPO / 'third_party' / 'nx2android'        # vendored subset of nx2android (MIT)
PROFILE_SRC = REPO / 'profile'                        # this port's game profile for porter (profile/<title id>)
RUNTIME = HERE / '.runtime'
DROP = REPO / 'put-game-files-here'
WIN = os.name == 'nt'
TITLE_ID = '0100D6200E130000'
GAME_NAME = 'PillarsOfEternityMobile'                 # Download/<this> on the phone (app: GameFolder.NAME)
DUMP_EXTS = ('.nsp', '.nsz', '.xci', '.xcz')
GB = 1 << 30
OVERLAY_FILES = ["Resources/unity_builtin_extra", "sharedassets1.assets", "resources.assets", "globalgamemanagers.assets",
                 "sharedassets0.assets", "sharedassets199.assets", "sharedassets3.assets",
                 "StreamingAssets/assetbundles/objectbundle", "StreamingAssets/gripdigitaldebugging"]
# ---- game image constants: fixed for the one game build this port supports (main build id 68976c702865e548ad33367b1ed393db, v393216).
# GEN_END / RT_CHUNKS mark il2cpp-generated game code vs the il2cpp runtime code interleaved with it (only GAME x18 sites are
# taken from the rewrite; the runtime is replaced by our own libil2cpp.so); BSS_START is where the file image ends and the patch area
# of the rewrite begins.
MAIN_BUILD_ID = '68976c702865e548ad33367b1ed393db'
GEN_END = 0x1713000
BSS_START = 0x38b4000
RT_CHUNKS = [(0xa68e20, 0xa6b2e0), (0xa6e040, 0xa73a00), (0xa74df0, 0xa77830), (0xa786b0, 0xa86bc0), (0xa871f0, 0xa89540),
             (0xa8b710, 0xa90d10), (0xa91650, 0xab3250), (0xab33c0, 0xab79f0), (0xab7ee0, 0xac3cc0), (0xac3e40, 0xaea5d0)]


class Fail(Exception):
    pass


def log(msg):
    print(f'[setup] {msg}', flush=True)


def ask(question, yes):
    if yes:
        print(f'{question} [Y/n] y (--yes)', flush=True)
        return True
    try:
        return input(f'{question} [Y/n] ').strip().lower() in ('', 'y', 'yes', 'д', 'да')
    except EOFError:
        return False


def run(cmd, **kw):
    log('$ ' + ' '.join(f'"{c}"' if ' ' in str(c) else str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        raise Fail(f'command failed (exit code {r.returncode}): {Path(str(cmd[1])).name if len(cmd) > 1 else cmd[0]} -- see the messages above')
    return r


def dir_stats(d: Path):
    n = total = 0
    if d.is_dir():
        for p in d.rglob('*'):
            if p.is_file():
                n += 1
                total += p.stat().st_size
    return n, total


def link_or_copy(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        if dst.stat().st_size == src.stat().st_size:
            return
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def link_tree(src: Path, dst: Path, skip=frozenset()):
    n = b = 0
    for p in src.rglob('*'):
        if not p.is_file():
            continue
        rel = p.relative_to(src).as_posix()
        if rel in skip:
            continue
        link_or_copy(p, dst / rel)
        n += 1
        b += p.stat().st_size
    return n, b


# ------------------------------------------------------------------------------------------------------------ inputs
def find_inputs(args, keys_arg):
    """dump files (NSP / NSZ / XCI / XCZ) and prod.keys from the arguments, else from put-game-files-here/ and the repository folder."""
    paths = [Path(a) for a in args] or [DROP, REPO]
    dumps, keys = [], [Path(keys_arg)] if keys_arg else []
    for p in paths:
        cands = []
        if p.is_dir():
            cands = [c for c in sorted(p.iterdir()) if c.is_file()]
            for sub in sorted(c for c in p.iterdir() if c.is_dir() and p == DROP):   # one level of sub-folders in the drop folder
                cands += [c for c in sorted(sub.iterdir()) if c.is_file()]
        elif p.is_file():
            cands = [p]
        for c in cands:
            if c.suffix.lower() in DUMP_EXTS:
                dumps.append(c.resolve())
            elif not keys_arg and (c.name.lower() == 'prod.keys' or c.suffix.lower() == '.keys' and 'prod' in c.name.lower()):
                keys.append(c.resolve())
    seen, uniq = set(), []
    for d in dumps:
        if d.name.lower() not in seen:
            seen.add(d.name.lower())
            uniq.append(d)
    return uniq, (keys[0] if keys else None)


# ------------------------------------------------------------------------------------------------------------ toolchain
DOTNET_DIR = RUNTIME / 'dotnet'
DOTNET_EXE = 'dotnet.exe' if WIN else 'dotnet'


def dotnet_env(d, base=None):
    env = dict(base or os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    if d:
        env['DOTNET_ROOT'] = str(Path(d).resolve().parent)
        if not WIN and Path(d).resolve().parent == DOTNET_DIR.resolve() and 'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT' not in env:
            env['DOTNET_SYSTEM_GLOBALIZATION_INVARIANT'] = '1'   # a private SDK must not depend on the distro's libicu
    return env


def sdk_major(d):
    try:
        out = subprocess.run([str(d), '--list-sdks'], capture_output=True, text=True, timeout=60, env=dotnet_env(d)).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    return max([int(l.split('.')[0]) for l in out.splitlines() if l.split('.')[0].isdigit()] + [0])


def find_dotnet():
    """A dotnet with a .NET 10+ SDK (the shader translator and nx_extract target net10.0): DOTNET, tools/.runtime/dotnet, ~/.dotnet, PATH."""
    for c in (os.environ.get('DOTNET'), DOTNET_DIR / DOTNET_EXE, Path.home() / '.dotnet' / DOTNET_EXE, shutil.which('dotnet')):
        if c and Path(c).is_file() and sdk_major(c) >= 10:
            return str(Path(c).resolve())
    return None


def download(url, dst):
    import urllib.request
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(dst, 'wb') as f:
            shutil.copyfileobj(r, f)
        return
    except Exception as e:
        err = e
    if shutil.which('curl') and subprocess.run(['curl', '-fsSL', '-o', str(dst), url]).returncode == 0:
        return
    raise Fail(f'download failed: {url} ({err}); check the internet connection and run setup again')


def install_dotnet(yes):
    print(f'  Setup needs the .NET 10 SDK (to build the dump extractor and the shader translator). It can install a private copy\n'
          f'  from Microsoft (dotnet-install script, ~300 MB download, ~800 MB on disk) into {DOTNET_DIR}.\n'
          f'  Nothing is installed system-wide, no admin rights needed.')
    if not ask('  Download and install the .NET 10 SDK there now?', yes):
        raise Fail('the .NET 10 SDK is needed. Run setup again and answer Y, install it yourself from '
                   'https://dotnet.microsoft.com/download/dotnet/10.0 (SDK), or set DOTNET=<path to dotnet>.')
    script = RUNTIME / ('dotnet-install.ps1' if WIN else 'dotnet-install.sh')
    download('https://dot.net/v1/' + script.name, script)
    if WIN:
        cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), '-Channel', '10.0',
               '-InstallDir', str(DOTNET_DIR), '-NoPath']
    else:
        cmd = ['bash', str(script), '--channel', '10.0', '--install-dir', str(DOTNET_DIR), '--no-path']
    print('  installing (a few minutes)...', flush=True)
    if subprocess.run(cmd).returncode != 0 or sdk_major(DOTNET_DIR / DOTNET_EXE) < 10:
        raise Fail(f'the .NET SDK install into {DOTNET_DIR} failed (see above). Run setup again, or install the .NET 10 SDK '
                   'from https://dotnet.microsoft.com/download/dotnet/10.0')
    return str((DOTNET_DIR / DOTNET_EXE).resolve())


def spirv_val():
    """spirv-val for the optional validation of the rewritten shaders: SPIRV_VAL, PATH, an Android NDK, the Vulkan SDK; None = skipped."""
    env = os.environ.get('SPIRV_VAL')
    if env and Path(env).is_file():
        return env
    found = shutil.which('spirv-val')
    if found:
        return found
    for ndk in (os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')):
        if ndk:
            for c in Path(ndk).glob('shader-tools/*/spirv-val*'):
                return str(c)
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if home:
        for c in sorted(Path(home).glob('ndk/*/shader-tools/*/spirv-val*'), reverse=True):
            return str(c)
    vk = os.environ.get('VULKAN_SDK')
    if vk:
        for c in (Path(vk) / 'Bin' / 'spirv-val.exe', Path(vk) / 'bin' / 'spirv-val'):
            if c.is_file():
                return str(c)
    return None


def check_packages(need_astc):
    mods = ['lz4', 'capstone', 'Crypto', 'numpy'] + (['astc_encoder', 'texture2ddecoder'] if need_astc else [])
    missing = []
    for m in mods:
        try:
            __import__(m)
        except ImportError:
            missing.append(m)
    try:
        from compression import zstd  # noqa: F401  (Python 3.14+)
    except ImportError:
        try:
            from backports import zstd  # noqa: F401
        except ImportError:
            missing.append('backports.zstd')
    if missing:
        raise Fail(f'this Python ({sys.executable}) lacks {", ".join(missing)}: start setup with setup.bat / setup.sh, or '
                   f'python -m pip install -r tools/requirements.txt')


# ------------------------------------------------------------------------------------------------------------ step 1: unpack
class Unpacked:
    def __init__(self, tbuild: Path):
        ex = sorted((tbuild / 'extract').glob('v*'), key=lambda p: int(p.name[1:]) if p.name[1:].isdigit() else -1)
        self.tbuild = tbuild
        self.ver = ex[-1].name if ex else None
        self.data = ex[-1] / 'RomFS' / 'Data' if ex else None
        self.main = ex[-1] / 'ExeFS' / 'main' if ex else None
        self.dmsv = tbuild / 'android' / 'shaders.dmsv'
        self.patched = tbuild / 'android' / f'patched_{self.ver}' if self.ver else None

    def complete(self):
        if not (self.data and self.data.is_dir() and self.main.is_file() and self.dmsv.is_file() and self.patched
                and (self.patched / 'main.patched.bin').is_file()):
            return False
        n, b = dir_stats(self.data)
        return n >= 3000 and b > 10 * GB


def stage_nx2a(work: Path):
    """work/nx2android = third_party/nx2android + profile/<title id> in its dm/games (where porter and patch_nso.py look for a
    game's profile). The .NET builds write their obj/ folders there, never into the repository."""
    dst = work / 'nx2android'
    shutil.copytree(NX2A_SRC, dst, dirs_exist_ok=True)
    games = dst / 'nxnative' / 'dm' / 'games'
    for prof in PROFILE_SRC.iterdir():
        if prof.is_dir():
            shutil.copytree(prof, games / prof.name, dirs_exist_ok=True)
    return dst


def step_unpack(dumps, keys, work: Path, dotnet, jobs, yes):
    dump_dir = work / 'dump'
    tbuild = dump_dir / f'{TITLE_ID}_build'
    u = Unpacked(tbuild)
    if u.complete():
        log(f'unpack: {tbuild} is complete -- skipped (delete it to redo)')
        return u
    if not dumps:
        raise Fail(f'no dump files found: put the base game and the update (NSP / NSZ / XCI / XCZ) into {DROP}')
    if not keys:
        raise Fail(f'no prod.keys found: put your console\'s prod.keys into {DROP}')
    dump_dir.mkdir(parents=True, exist_ok=True)
    for f in dumps:                                   # porter writes beside the files it reads: give it hard links in our work folder
        link_or_copy(f, dump_dir / f.name)
    for old in dump_dir.iterdir():                    # a file from an earlier run that is not given now would confuse the scan
        if old.is_file() and old.suffix.lower() in DUMP_EXTS and old.name not in {f.name for f in dumps}:
            log(f'unpack: removing the link {old.name} from an earlier run (not among this run\'s files)')
            old.unlink()
    nx2a = stage_nx2a(work)
    log('unpack: extracting the dumps, rewriting main and translating the shaders (nx2android porter; 5-15 minutes, more on a first run '
        'that downloads NuGet packages)')
    env = dotnet_env(dotnet, dict(os.environ, PYTHONUTF8='1', PYTHONIOENCODING='utf-8'))
    run([sys.executable, HERE / 'poe_unpack.py', '--nx2a', nx2a, '--folder', dump_dir, '--keys', keys, '--dotnet', dotnet,
         '--home', work / 'nx2a-home', '--jobs', str(jobs)], env=env)
    u = Unpacked(tbuild)
    if not u.complete():
        raise Fail(f'the unpack step finished but {tbuild} is not complete (RomFS/Data, ExeFS/main, shaders.dmsv, patched main)')
    return u


# ------------------------------------------------------------------------------------------------------------ step 2: image
def in_rt(a):
    return any(lo <= a < hi for lo, hi in RT_CHUNKS)


def step_image(u: Unpacked, work: Path):
    """game/libf2game.so: main decompressed (nx2android dm/tools/nso2elf.py) with the x18 GAME sites of the rewrite
    (patched_<ver>/main.patched.bin, main.fixups.tsv, main.sites.tsv) and the rewrite's patch area; each stub's x18 save / restore
    pair is hardened to a pre-/post-indexed stack pair (the app's threads have no Horizon TLS slot)."""
    import re
    import struct
    out = work / 'game' / 'libf2game.so'
    if out.is_file() and out.stat().st_size > 0:
        log(f'image: {out} exists -- skipped (delete it to redo)')
        return out
    sys.path.insert(0, str(work / 'nx2android' / 'nxnative' / 'dm' / 'tools'))
    import nso2elf
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    img, segs, bss, build_id = nso2elf.load(str(u.main))
    bid = build_id.hex() if isinstance(build_id, (bytes, bytearray)) else str(build_id)
    if not bid.lower().startswith(MAIN_BUILD_ID):
        raise Fail(f'image: main build id {bid} is not the supported one ({MAIN_BUILD_ID}, update v393216 / 2.60.00)')
    orig = bytearray(img)
    pat = (u.patched / 'main.patched.bin').read_bytes()
    fx = {}
    for line in (u.patched / 'main.fixups.tsv').read_text().splitlines():
        parts = line.strip('# \n').split('\t')
        if len(parts) == 2:
            fx[parts[0]] = parts[1]
    patch_off = int(fx['PATCH_OFF'], 16)
    sites = []
    for line in (u.patched / 'main.sites.tsv').read_text().splitlines():
        p = line.split('\t')
        if p[0].startswith('0x') and len(p) > 1 and p[1].strip() == 'x18':
            a = int(p[0], 16)
            if a < GEN_END and not in_rt(a):
                sites.append(a)
    patch = bytearray(pat[patch_off:])
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    hard = skipped = nsite = 0
    stubs = set()
    for a in sites:
        w = struct.unpack_from('<I', pat, a)[0]
        if w == struct.unpack_from('<I', orig, a)[0]:
            continue
        if (w & 0xfc000000) != 0x14000000:
            raise Fail(f'image: site {a:#x} is not a branch in the rewrite')
        imm = w & 0x3ffffff
        if imm & (1 << 25):
            imm -= 1 << 26
        stub = a + imm * 4
        if stub < patch_off:
            raise Fail(f'image: site {a:#x} branches outside the patch area')
        struct.pack_into('<I', orig, a, w)
        nsite += 1
        stubs.add(stub)
    for stub in sorted(stubs):
        o = stub - patch_off
        ins = []
        for ii in cs.disasm(bytes(patch[o:o + 4 * 40]), stub):
            ins.append(ii)
            if ii.mnemonic == 'b':
                break
        st = [i for i, ii in enumerate(ins) if ii.mnemonic == 'stp' and ii.op_str.endswith('[sp, #-0x10]')]
        ld = [i for i, ii in enumerate(ins) if ii.mnemonic == 'ldp' and ii.op_str.endswith('[sp, #-0x10]')]
        other_sp = [ii for i, ii in enumerate(ins) if i not in st + ld and re.search(r'\bsp\b', ii.op_str)]
        if len(st) == 1 and len(ld) == 1 and st[0] == 0 and not other_sp and ins[-1].mnemonic == 'b':
            w0 = struct.unpack_from('<I', patch, o + 4 * st[0])[0]
            w1 = struct.unpack_from('<I', patch, o + 4 * ld[0])[0]
            w0n = (w0 & ~(3 << 23)) | (3 << 23)
            w1n = (w1 & ~(3 << 23) & ~(0x7f << 15)) | (1 << 23) | (2 << 15)
            struct.pack_into('<I', patch, o + 4 * st[0], w0n)
            struct.pack_into('<I', patch, o + 4 * ld[0], w1n)
            chk = list(cs.disasm(bytes(patch[o:o + 4 * len(ins)]), stub))
            if not (chk[st[0]].op_str.endswith('[sp, #-0x10]!') and chk[ld[0]].op_str.endswith('[sp], #0x10')):
                raise Fail(f'image: stub {stub:#x} did not harden as expected')
            hard += 1
        else:
            skipped += 1
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.partial')
    with open(tmp, 'wb') as f:
        f.write(orig[:BSS_START])
        f.write(patch)
    os.replace(tmp, out)
    log(f'image: {out} ({out.stat().st_size} bytes): x18 game sites {nsite} (listed {len(sites)}), stubs {len(stubs)} '
        f'(hardened {hard}, left as emitted {skipped})')
    return out


# ------------------------------------------------------------------------------------------------------------ step 3: overlay
def overlay_env(u: Unpacked, work: Path, overlay_dir: Path, val):
    env = dict(os.environ, PYTHONUTF8='1', PYTHONIOENCODING='utf-8', ASTC_SET_DATA=str(u.data.resolve()),
               ASTC_SET_OVERLAY=str(overlay_dir.resolve()), POE_MAIN=str(u.main.resolve()), POE_CACHE=str((work / 'cache').resolve()),
               POE_NXNATIVE=str((work / 'nx2android' / 'nxnative').resolve()), F2_SHADERS_DMSV=str(u.dmsv.resolve()),
               F2_SPIRV_VAL=val or '')
    env.pop('POE_SHADER_TREE', None)
    return env


def step_overlay(u: Unpacked, work: Path):
    overlay_dir = work / 'overlay' / 'Data'
    manifest = overlay_dir.parent / 'MANIFEST.tsv'
    if manifest.is_file() and overlay_dir.is_dir():
        log(f'overlay: {overlay_dir} is complete (MANIFEST.tsv) -- skipped (delete {overlay_dir.parent} to redo)')
        return overlay_dir
    if overlay_dir.parent.exists():
        shutil.rmtree(overlay_dir.parent)
    overlay_dir.mkdir(parents=True)
    rep_dir = work / 'overlay-reports'
    rep_dir.mkdir(parents=True, exist_ok=True)
    val = spirv_val()
    log(f'overlay: spirv-val: {val}' if val else 'overlay: spirv-val not found (optional: Android NDK or Vulkan SDK); the modules are '
        'written and decoded but not validated -- the output is the same')
    env = overlay_env(u, work, overlay_dir, val)
    py = sys.executable
    run([py, OVL / 'ucommon.py'], env=env)            # common strings + the Shader type tree from this dump, once
    for rel in OVERLAY_FILES:
        src = u.data / rel
        if not src.is_file():
            raise Fail(f'overlay: {rel} is not in RomFS/Data -- not the supported game version?')
        dst = overlay_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        run([py, OVL / 'run_convert.py', src, dst, '--report', rep_dir / Path(rel).name] + ([] if val else ['--no-val']), env=env)
    run([py, OVL / 'set_platform.py'], env=env)
    run([py, OVL / 'ggm_patch.py'], env=env)
    boot = overlay_dir.parent / 'apk_assets' / 'bin' / 'Data' / 'boot.config'
    boot.parent.mkdir(parents=True, exist_ok=True)
    boot.write_bytes(b'wait-for-native-debugger=0\n')
    run([py, OVL / 'verify_overlay.py'], env=env)
    run([py, OVL / 'manifest.py'], env=env)           # writes overlay/MANIFEST.tsv last: the step's "done" mark
    return overlay_dir


# ------------------------------------------------------------------------------------------------------------ step 4: astc
def step_astc(u: Unpacked, overlay_dir: Path, work: Path, jobs):
    out_dir = work / 'overlay-astc' / 'Data'
    env = overlay_env(u, work, overlay_dir, None)
    env.update(ASTC_SET_OUT=str(out_dir.resolve()), ASTC_SET_WORK=str((work / 'astc-build').resolve()))
    ok = work / 'astc-build' / 'verified.ok'
    if out_dir.is_dir() and ok.is_file():
        n, b = dir_stats(out_dir)
        log(f'astc: {out_dir} is complete ({n} files, {b / GB:.1f} GB) -- skipped (delete {ok} to rebuild / re-check)')
        return out_dir
    log('astc: re-encoding the BC textures as ASTC (10-20 minutes)')
    run([sys.executable, OVL / 'ucommon.py'], env=env)
    run([sys.executable, OVL / 'astc_set.py', 'build', '-j', str(jobs), '--resume'], env=env)
    run([sys.executable, OVL / 'astc_set.py', 'verify', '-j', str(jobs), '--every', '7'], env=env)
    ok.write_text('verify passed\n')
    return out_dir


# ------------------------------------------------------------------------------------------------------------ step 5: layout
README_TEXT = """PILLARS OF ETERNITY MOBILE - game data folder
============================================

ENGLISH
1. Copy this whole folder "PillarsOfEternityMobile" into Download on the phone
   (the path on the phone must be Download/PillarsOfEternityMobile).
2. Install the APK.
3. Start the game. The first time it shows a page asking for file access
   ("All files access" on Android 11 and newer): allow it and return to the game.
   Without that access the game cannot read this folder and will not start.
4. If something goes wrong, send the folder Download/PillarsOfEternityMobile/logs
   (crash.txt, boot.log, logcat.txt and the "prev" copies).

Textures in this folder: {tex_en}

РУССКИЙ
1. Скопируйте всю папку "PillarsOfEternityMobile" в Download на телефоне
   (путь на телефоне должен быть Download/PillarsOfEternityMobile).
2. Установите APK.
3. Запустите игру. В первый раз она покажет страницу с просьбой о доступе к файлам
   («Доступ ко всем файлам» на Android 11 и новее): разрешите и вернитесь в игру.
   Без этого доступа игра не может прочитать эту папку и не запустится.
4. Если что-то пошло не так, пришлите папку Download/PillarsOfEternityMobile/logs
   (crash.txt, boot.log, logcat.txt и их копии "prev").

Текстуры в этой папке: {tex_ru}
"""
README_TEX = {
    'bc': ('BC - for Adreno (Qualcomm Snapdragon) phones. For Mali, PowerVR or Xclipse run the setup again and pick ASTC.',
           'BC - для телефонов на Adreno (Qualcomm Snapdragon). Для Mali, PowerVR и Xclipse запустите setup ещё раз и выберите ASTC.'),
    'astc': ('ASTC - works on every phone.', 'ASTC - подходит для любого телефона.'),
    'both': ('BC and ASTC - the game picks the right set for the phone by itself.', 'BC и ASTC - игра сама выберет подходящий набор.'),
}


def step_layout(out_root: Path, u: Unpacked, overlay_dir: Path, astc_dir, game_image: Path, texture):
    for sub in ('RomFS', 'overlay', 'overlay-astc', 'game'):    # a layout of an earlier run with another texture mode: start clean
        if (out_root / sub).exists():
            shutil.rmtree(out_root / sub)
    (out_root / 'save').mkdir(parents=True, exist_ok=True)
    skip = frozenset()
    if texture == 'astc':                             # the ASTC copy of a file replaces it whole: leave the BC copy out
        skip = frozenset(p.relative_to(astc_dir).as_posix() for p in astc_dir.rglob('*') if p.is_file())
    n1, b1 = link_tree(u.data, out_root / 'RomFS' / 'Data', skip)
    n2, b2 = link_tree(overlay_dir, out_root / 'overlay' / 'Data', skip)
    n3 = b3 = 0
    if texture in ('astc', 'both'):
        n3, b3 = link_tree(astc_dir, out_root / 'overlay-astc' / 'Data')
    link_or_copy(game_image, out_root / 'game' / 'libf2game.so')
    b4 = game_image.stat().st_size
    en, ru = README_TEX[texture]
    (out_root / 'README.txt').write_text(README_TEXT.format(tex_en=en, tex_ru=ru), encoding='utf-8')
    total = b1 + b2 + b3 + b4
    log(f'layout: {out_root} ({texture}): RomFS/Data {n1} files, overlay/Data {n2} files'
        + (f', overlay-astc/Data {n3} files' if n3 else '') + f', game/libf2game.so -> {total / GB:.2f} GB')
    return total


# ------------------------------------------------------------------------------------------------------------ main
STEPS = ['unpack', 'image', 'overlay', 'astc', 'layout']
GPU_HELP = """Which phone(s) will play this?
  1) Adreno (Qualcomm Snapdragon)                       -> bc    (~16 GB on the phone, quickest setup)
  2) Mali, PowerVR or Samsung Xclipse                   -> astc  (~15 GB, works on every GPU)
  3) not sure / several phones                          -> both  (~28 GB, every GPU)"""


def ask_texture():
    print(GPU_HELP, flush=True)
    while True:
        try:
            v = input('Which one (1/2/3, Enter = 3)? ').strip()
        except EOFError:
            v = ''
        if v in ('', '3'):
            return 'both'
        if v in ('1', '2'):
            return {'1': 'bc', '2': 'astc'}[v]
        print('  please answer 1, 2 or 3')


def check(dumps, keys, work, out_root):
    log(f'Python: {sys.executable} ({sys.version.split()[0]})')
    d = find_dotnet()
    log(f'.NET 10 SDK: {d or "not found (setup offers a private copy in tools/.runtime/dotnet)"}')
    log(f'spirv-val (optional): {spirv_val() or "not found"}')
    log(f'dumps: {", ".join(f.name for f in dumps) or "none found in " + str(DROP)}')
    log(f'prod.keys: {keys or "not found"}')
    u = Unpacked(work / 'dump' / f'{TITLE_ID}_build')
    log(f'unpack: {"done" if u.complete() else "to do"}; image: {"done" if (work / "game/libf2game.so").is_file() else "to do"}; '
        f'overlay: {"done" if (work / "overlay/MANIFEST.tsv").is_file() else "to do"}; '
        f'astc: {"done" if (work / "astc-build/verified.ok").is_file() else "to do / not chosen"}')
    log(f'output folder: {out_root} ({"present" if (out_root / "game/libf2game.so").is_file() else "not made yet"})')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='dump files / folders and prod.keys (default: put-game-files-here/)')
    ap.add_argument('--keys', help='prod.keys (default: found among the files)')
    ap.add_argument('--work', default=str(REPO / 'work' / 'setup'), help='work folder (default: work/setup); needs ~60 GB')
    ap.add_argument('--out', default=str(REPO / 'work' / GAME_NAME), help=f'the folder to copy to the phone (default: work/{GAME_NAME}; '
                    'on the same drive as --work it is made of hard links and takes no extra space)')
    ap.add_argument('--texture', choices=('bc', 'astc', 'both'), help='texture sets (default: asked; with --yes: both)')
    ap.add_argument('--steps', default=','.join(STEPS), help='comma list of: ' + ','.join(STEPS))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--yes', action='store_true', help='answer yes to every question (downloads); --texture defaults to both')
    a = ap.parse_args([x for x in (argv if argv is not None else sys.argv[1:]) if x.lower() != 'check'])
    is_check = any(x.lower() == 'check' for x in (argv if argv is not None else sys.argv[1:]))
    a.yes = a.yes or os.environ.get('POE_SETUP_YES') == '1'
    work = Path(a.work).resolve()
    out_root = Path(a.out).resolve()
    dumps, keys = find_inputs(a.files, a.keys)
    if is_check:
        return check(dumps, keys, work, out_root)
    steps = [s.strip() for s in a.steps.split(',') if s.strip()]
    bad = [s for s in steps if s not in STEPS]
    if bad:
        raise Fail(f'unknown step(s): {", ".join(bad)}')
    texture = a.texture or ('both' if a.yes else ask_texture())
    check_packages(texture != 'bc' and 'astc' in steps)
    work.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(work).free
    log(f'work={work}  out={out_root}  texture={texture}  steps={",".join(steps)}  free={free / GB:.0f} GB')
    t0 = time.time()
    u = Unpacked(work / 'dump' / f'{TITLE_ID}_build')
    if 'unpack' in steps:
        dotnet = find_dotnet() or (None if u.complete() else install_dotnet(a.yes))
        if not u.complete():
            log(f'dumps: {", ".join(f.name for f in dumps)}; keys: {keys.name if keys else "-"}; .NET: {dotnet}')
        u = step_unpack(dumps, keys, work, dotnet, a.jobs, a.yes)
        log(f'[unpack] {time.time() - t0:.0f} s')
    if not u.complete():
        raise Fail(f'{u.tbuild} is not complete: run the unpack step first')
    game = work / 'game' / 'libf2game.so'
    if 'image' in steps:
        if not (work / 'nx2android').is_dir():
            stage_nx2a(work)
        game = step_image(u, work)
        log(f'[image] {time.time() - t0:.0f} s')
    overlay_dir = work / 'overlay' / 'Data'
    if 'overlay' in steps:
        if not (work / 'nx2android').is_dir():
            stage_nx2a(work)
        overlay_dir = step_overlay(u, work)
        log(f'[overlay] {time.time() - t0:.0f} s')
    astc_dir = work / 'overlay-astc' / 'Data'
    if 'astc' in steps and texture != 'bc':
        astc_dir = step_astc(u, overlay_dir, work, a.jobs)
        log(f'[astc] {time.time() - t0:.0f} s')
    if 'layout' in steps:
        if not game.is_file() or not (overlay_dir.parent / 'MANIFEST.tsv').is_file():
            raise Fail('layout: run the image and overlay steps first')
        if texture != 'bc' and not (work / 'astc-build' / 'verified.ok').is_file():
            raise Fail('layout: the ASTC set is not built yet (step astc), or choose --texture bc')
        step_layout(out_root, u, overlay_dir, astc_dir, game, texture)
        log(f'[layout] {time.time() - t0:.0f} s')
    log(f'DONE in {time.time() - t0:.0f} s. Copy the folder {out_root} to the phone as Download/{GAME_NAME} '
        f'(USB file transfer, or: adb push "{out_root}" /sdcard/Download/), then install the APK and start the game.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print(f'[setup] STOP: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('[setup] interrupted; run setup again to continue where it stopped', file=sys.stderr)
        sys.exit(130)
