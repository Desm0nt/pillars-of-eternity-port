#!/usr/bin/env python3
"""Pillars of Eternity Mobile: build the native libraries into work/build/native/.

  libil2cpp.so  = the il2cpp runtime of YOUR Unity 2018.1.4f1 install (Editor/Data/il2cpp: libil2cpp + external/boehmgc),
                  copied to work/build/il2cpp-src with three small F2 hooks (IL2CPP_HOOKS below), compiled with the NDK
                  + our app/native/{loader,boot,nn}        (unstripped copy: libil2cpp.sym.so, link map libil2cpp.map)
  libmain.so    = app/native/main/f2_main.c (replaces Unity's libmain.so)
The game's own code is NOT built here: it is the flat image game/libf2game.so that setup makes from the player's dump.
Nothing from Unity is stored in this repository; docs/BUILD.md says what to install.

usage: python tools/build_native.py [all|so|main] [-j N]
Environment: F2_SKIP="a.c,b.c" leaves own modules out (default: none); tool locations: tools/buildcfg.py.
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buildcfg as cfg  # noqa: E402

REPO = cfg.REPO
NATIVE = REPO / 'app' / 'native'
WORK = REPO / 'work' / 'build'
SRC = WORK / 'il2cpp-src'          # patched copy of the Unity install's il2cpp runtime
OBJ_RT = WORK / 'il2cpp-obj'
OUT = WORK / 'native'
TARGET_RT = 'aarch64-linux-android21'   # the runtime as Unity's own Android toolchain builds it
TARGET = 'aarch64-linux-android26'      # our code (minSdk 26)

# Unity's AndroidToolChain defines + IL2CPPOutputBuildDescription defines; F2: the Switch game code was compiled with stack-trace
# sentries (IL2CPP_TARGET_SWITCH), the runtime must push / pop them the same way.
DEFS_COMMON = ['LINUX', 'ANDROID', 'PLATFORM_ANDROID', '__linux__', '__STDC_FORMAT_MACROS', 'GC_NOT_DLL', 'RUNTIME_IL2CPP',
               'IL2CPP_ENABLE_NATIVE_STACKTRACES=0', 'F2_STACKTRACE_SENTRIES=1']
DEFS_BOEHM = ['HAVE_BDWGC_GC', 'HAVE_BOEHM_GC', 'DEFAULT_GC_NAME="BDWGC"', 'ALL_INTERIOR_POINTERS=1', 'GC_GCJ_SUPPORT=1',
              'JAVA_FINALIZATION=1', 'NO_EXECUTE_PERMISSION=1', 'GC_NO_THREADS_DISCOVERY=1', 'IGNORE_DYNAMIC_LOADING=1',
              'GC_DONT_REGISTER_MAIN_STATIC_DATA=1', 'GC_VERSION_MAJOR=7', 'GC_VERSION_MINOR=4', 'GC_VERSION_MICRO=0', 'GC_THREADS=1',
              'USE_MMAP=1', 'USE_MUNMAP=1']

# The three hooks F2 adds to the il2cpp runtime (the only changes to Unity's source): (file, anchor line in Unity's source,
# what is inserted, before / after the anchor). Applied to the copy in work/build/il2cpp-src; the anchors must match once.
IL2CPP_HOOKS = [
    ('libil2cpp/il2cpp-config.h',
     '#define IL2CPP_ENABLE_STACKTRACE_SENTRIES (IL2CPP_TARGET_JAVASCRIPT || IL2CPP_TARGET_N3DS || IL2CPP_TARGET_SWITCH)',
     '/* F2: Switch-compiled game code pushes/pops stack-trace sentries; match it. */\n'
     '#define IL2CPP_ENABLE_STACKTRACE_SENTRIES (IL2CPP_TARGET_JAVASCRIPT || IL2CPP_TARGET_N3DS || IL2CPP_TARGET_SWITCH || defined(F2_STACKTRACE_SENTRIES))',
     'replace'),
    ('libil2cpp/vm/Runtime.cpp', '#include "il2cpp-config.h"',
     'extern "C" { void f2_runtime_pre_init(void); void f2_runtime_gc_ready(void); }', 'first-line-before'),
    ('libil2cpp/vm/Runtime.cpp', '        il2cpp::utils::RegisterRuntimeInitializeAndCleanup::ExecuteInitializations();',
     '        f2_runtime_pre_init(); /* F2: map the Switch game image and register its Il2CppCodeRegistration */', 'before'),
    ('libil2cpp/vm/Runtime.cpp', '        gc::GarbageCollector::Initialize();',
     '        f2_runtime_gc_ready(); /* F2: game image data/bss as GC roots */', 'after'),
    ('libil2cpp/vm/Thread.cpp', '#include "il2cpp-config.h"', 'extern "C" { void f2_thread_enter(void); }', 'first-line-before'),
    ('libil2cpp/vm/Thread.cpp', '    void Thread::Initialize(Il2CppThread* thread, Il2CppDomain* domain)\n    {',
     '        f2_thread_enter(); /* F2: per-thread slot block for the x18 stubs of the game image */', 'after'),
]


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'FAILED: {" ".join(str(c) for c in cmd)[:400]}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}')
    return r


def prepare_il2cpp():
    """work/build/il2cpp-src = <Unity>/Data/il2cpp/{libil2cpp, external/boehmgc} + IL2CPP_HOOKS (redone when the stamp differs)"""
    data = cfg.unity_editor()
    il = data / 'il2cpp'
    stamp = SRC / '.f2-stamp'
    want = f'{il}\n{len(IL2CPP_HOOKS)} hooks v1\n'
    if stamp.is_file() and stamp.read_text(encoding='utf-8') == want:
        return
    if SRC.exists():
        shutil.rmtree(SRC)
    shutil.copytree(il / 'libil2cpp', SRC / 'libil2cpp')
    shutil.copytree(il / 'external' / 'boehmgc', SRC / 'external' / 'boehmgc')
    for rel, anchor, text, how in IL2CPP_HOOKS:
        p = SRC / rel
        s = p.read_text(encoding='utf-8', errors='surrogateescape')
        nl = '\r\n' if '\r\n' in s else '\n'
        a = anchor.replace('\n', nl)
        t = text.replace('\n', nl)
        if s.count(a) != 1:
            sys.exit(f'il2cpp hook: {rel}: the anchor occurs {s.count(a)} times (expected once) -- not Unity 2018.1.4f1?\n  {anchor}')
        if how == 'replace':
            s = s.replace(a, t)
        elif how == 'first-line-before':
            if not s.startswith(a):
                sys.exit(f'il2cpp hook: {rel} does not start with {anchor!r}')
            s = t + nl + s
        elif how == 'before':
            s = s.replace(a, t + nl + a)
        else:
            s = s.replace(a, a + nl + t)
        p.write_text(s, encoding='utf-8', errors='surrogateescape', newline='')
    stamp.write_text(want, encoding='utf-8')
    print(f'il2cpp runtime: copied from {il} and hooked ({len(IL2CPP_HOOKS)} insertions) -> {SRC}')


def prefix_maps():
    # no build-machine path in the libraries: __FILE__ of the runtime's asserts becomes il2cpp/..., ours app/native/...
    return [f'-ffile-prefix-map={SRC}=il2cpp', f'-ffile-prefix-map={SRC.as_posix()}=il2cpp',
            f'-ffile-prefix-map={NATIVE}=app/native', f'-ffile-prefix-map={NATIVE.as_posix()}=app/native']


def runtime_sources():
    out = sorted(p for p in (SRC / 'libil2cpp').rglob('*') if p.suffix in ('.c', '.cpp') and p.is_file())
    out.append(SRC / 'external' / 'boehmgc' / 'extra' / 'gc.c')
    return out


def rt_cmd(bin_, src, obj):
    boehm = 'boehmgc' in src.parts
    cpp = src.suffix == '.cpp'
    cmd = [bin_ / f'clang++{cfg.EXE}' if cpp else bin_ / f'clang{cfg.EXE}', '-c', '-g', '-funwind-tables', '-fPIC', '-fdata-sections',
           '-ffunction-sections', '-fno-strict-aliasing', '-fvisibility=hidden', '-fno-strict-overflow', '--target=' + TARGET_RT, '-w',
           '-O2'] + prefix_maps()
    cmd += ['-D' + d for d in DEFS_COMMON + (DEFS_BOEHM if boehm else [])]
    inc = [SRC / 'external/boehmgc/include', SRC / 'external/boehmgc/libatomic_ops/src'] if boehm else \
          [SRC / 'libil2cpp', SRC / 'external/boehmgc/include']
    cmd += ['-I' + str(i) for i in inc]
    if cpp:
        cmd += ['-fno-rtti', '-std=gnu++11', '-fvisibility-inlines-hidden', '-fexceptions', '-Wno-reserved-user-defined-literal',
                '-Wno-c++11-narrowing']
    return cmd + ['-o', obj, src]


def compile_cached(job):
    cmd, obj, deps = job
    stamp = Path(str(obj) + '.cmd')
    key = ' '.join(str(c) for c in cmd)
    if obj.exists() and stamp.exists() and stamp.read_text(encoding='utf-8') == key and obj.stat().st_mtime >= max(d.stat().st_mtime for d in deps):
        return obj, ''
    obj.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        return None, f'{deps[0]}\n{r.stderr[-3000:]}'
    stamp.write_text(key, encoding='utf-8')
    return obj, r.stderr.strip()


def compile_all(jobs_list, jobs):
    objs, fails = [], []
    with cf.ThreadPoolExecutor(jobs) as ex:
        for obj, err in ex.map(compile_cached, jobs_list):
            if obj is None:
                fails.append(err)
            else:
                objs.append(obj)
    if fails:
        sys.exit(f'{len(fails)} compile failures; first:\n{fails[0]}')
    return objs


def runtime_objects(bin_, jobs):
    srcs = runtime_sources()
    hdr = max(p.stat().st_mtime for p in SRC.rglob('*.h'))
    job = []
    for s in srcs:
        rel = s.relative_to(SRC)
        obj = OBJ_RT / rel.with_suffix(rel.suffix + '.o')
        job.append((rt_cmd(bin_, s, obj), obj, [s, SRC / '.f2-stamp']))
    objs = compile_all(job, jobs)
    print(f'il2cpp runtime: {len(objs)} objects')
    return objs


def own_objects(bin_, jobs):
    skip = tuple(x for x in os.environ.get('F2_SKIP', '').split(',') if x)
    flags = ['-c', '-g', '-O2', '-funwind-tables', '-fPIC', '-fdata-sections', '-ffunction-sections', '-fno-strict-aliasing',
             '-fvisibility=hidden', '-fno-omit-frame-pointer', '--target=' + TARGET, '-Wall', '-Wno-unused-function',
             '-Wno-unused-variable', '-DLINUX', '-DANDROID', '-DPLATFORM_ANDROID', '-D__linux__', '-D__STDC_FORMAT_MACROS', '-DGC_NOT_DLL',
             '-DRUNTIME_IL2CPP', '-DIL2CPP_ENABLE_NATIVE_STACKTRACES=0', '-DF2_STACKTRACE_SENTRIES=1'] + prefix_maps()
    inc = ['-I' + str(p) for p in (NATIVE, NATIVE / 'loader', NATIVE / 'boot', NATIVE / 'nn', SRC / 'libil2cpp', SRC / 'external/boehmgc/include')]
    headers = list(NATIVE.rglob('*.h'))
    job = []
    for sub in ('loader', 'boot', 'nn'):
        for s in sorted((NATIVE / sub).glob('*.c')) + sorted((NATIVE / sub).glob('*.cpp')):
            if s.name in skip:
                continue
            cpp = s.suffix == '.cpp'
            cmd = [bin_ / (f'clang++{cfg.EXE}' if cpp else f'clang{cfg.EXE}')] + flags + inc
            if cpp:
                cmd += ['-fno-rtti', '-std=gnu++11', '-fvisibility-inlines-hidden', '-fexceptions', '-w']
            obj = OUT / 'obj' / f'{sub}_{s.name}.o'
            job.append((cmd + ['-o', obj, s], obj, [s] + headers))
    return compile_all(job, jobs)


def build_so(jobs):
    bin_ = cfg.ndk_bin()
    prepare_il2cpp()
    objs = runtime_objects(bin_, jobs)
    mine = own_objects(bin_, jobs)
    rsp = OUT / 'objs.rsp'
    rsp.write_text('\n'.join('"' + Path(o).as_posix() + '"' for o in objs + mine), encoding='utf-8')
    so = OUT / 'libil2cpp.sym.so'
    run([bin_ / f'clang++{cfg.EXE}', '--target=' + TARGET, '-shared', '-Wl,-soname,libil2cpp.so', '-Wl,--no-undefined', '-Wl,-z,noexecstack',
         '-Wl,--gc-sections', '-Wl,--build-id', '-Wl,-z,max-page-size=16384', '-static-libstdc++', '-llog', '-ldl', '-lm',
         '-Wl,--wrap=dl_iterate_phdr', '-Wl,-Map=' + str(OUT / 'libil2cpp.map'),
         '-Wl,--version-script=' + str(NATIVE / 'loader' / 'exports.txt'), '-o', so, '@' + str(rsp)])
    stripped = OUT / 'libil2cpp.so'
    run([bin_ / f'llvm-strip{cfg.EXE}', '--strip-unneeded', '-o', stripped, so])
    nm = run([bin_ / f'llvm-nm{cfg.EXE}', '-D', '--defined-only', stripped]).stdout
    names = [l.split()[-1] for l in nm.splitlines() if l.strip()]
    for must in ('f2_boot', 'f2_unity_loaded', 'f2_unity_onload_done', 'il2cpp_init', 'il2cpp_runtime_invoke'):
        if must not in names:
            sys.exit(f'missing export {must}')
    print(f'libil2cpp.so {stripped.stat().st_size} bytes ({len(objs)} runtime objects + {len(mine)} own); exports il2cpp_* '
          f'{sum(1 for n in names if n.startswith("il2cpp_"))}, f2_* {sum(1 for n in names if n.startswith("f2_"))}')


def build_main():
    bin_ = cfg.ndk_bin()
    obj = OUT / 'obj' / 'main_f2_main.c.o'
    obj.parent.mkdir(parents=True, exist_ok=True)
    run([bin_ / f'clang{cfg.EXE}', '-c', '-g', '-O2', '-funwind-tables', '-fPIC', '-fdata-sections', '-ffunction-sections', '-fno-strict-aliasing',
         '-fvisibility=hidden', '-fno-omit-frame-pointer', '--target=' + TARGET, '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
         '-DLINUX', '-DANDROID', '-DPLATFORM_ANDROID', '-D__linux__', '-D__STDC_FORMAT_MACROS', '-DGC_NOT_DLL', '-DRUNTIME_IL2CPP',
         '-DIL2CPP_ENABLE_NATIVE_STACKTRACES=0', '-DF2_STACKTRACE_SENTRIES=1'] + prefix_maps() +
        ['-I' + str(NATIVE), '-I' + str(NATIVE / 'loader'), '-I' + str(NATIVE / 'boot'), '-I' + str(NATIVE / 'nn'),
         '-o', obj, NATIVE / 'main' / 'f2_main.c'])
    so = OUT / 'libmain.so'
    run([bin_ / f'clang{cfg.EXE}', '--target=' + TARGET, '-shared', '-Wl,-soname,libmain.so', '-Wl,--no-undefined', '-Wl,-z,noexecstack',
         '-Wl,--build-id', '-Wl,-z,max-page-size=16384', '-llog', '-ldl', '-o', so, obj])
    run([bin_ / f'llvm-strip{cfg.EXE}', '--strip-unneeded', so])
    print(f'libmain.so {so.stat().st_size} bytes')


if __name__ == '__main__':
    args = sys.argv[1:]
    what = args[0] if args and not args[0].startswith('-') else 'all'
    jobs = int(args[args.index('-j') + 1]) if '-j' in args else (os.cpu_count() or 8)
    OUT.mkdir(parents=True, exist_ok=True)
    if what in ('so', 'all'):
        build_so(jobs)
    if what in ('main', 'all'):
        build_main()
