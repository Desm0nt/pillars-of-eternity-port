"""Where the developer's tools are: environment variables, else build.local.env (KEY=VALUE lines, untracked) in the repository root,
else the defaults below. Used by tools/build_native.py and tools/build_apk.py; docs/BUILD.md lists every key.

  ANDROID_NDK       Android NDK r29 (29.0.14206865 was used); default: ANDROID_NDK_HOME / ANDROID_NDK_ROOT, else the newest
                    <ANDROID_SDK>/ndk/29.*
  ANDROID_SDK       Android SDK with build-tools and a platform; default: ANDROID_HOME / ANDROID_SDK_ROOT
  BUILD_TOOLS       build-tools version (default 36.1.0)       ANDROID_PLATFORM   platform for android.jar (default android-36)
  JAVA_HOME         a JDK 17 or newer (javac, jar, keytool; d8 / apksigner run with it)
  UNITY_EDITOR      the Editor folder of Unity 2018.1.4f1 with Android Build Support (IL2CPP); default: the standard Unity Hub
                    location of this platform
  POE_SIGNING_KEY   signing key: nx2android's signing-key.json (RSA n/e/d + certificate), or a .jks / .keystore
                    (POE_KEYSTORE_PASS, POE_KEY_ALIAS); default: a debug keystore made once in work/signing with the JDK's keytool
"""
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WIN = os.name == 'nt'
EXE = '.exe' if WIN else ''
BAT = '.bat' if WIN else ''


def _local():
    f = REPO / 'build.local.env'
    out = {}
    if f.is_file():
        for line in f.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                out[k.strip()] = v.strip().strip('"')
    return out


_L = _local()


def get(key, *fallbacks, default=None):
    for k in (key,) + fallbacks:
        v = os.environ.get(k) or _L.get(k)
        if v:
            return v
    return default


def fail(msg):
    sys.exit(f'build: {msg}\n(see docs/BUILD.md: set it in the environment or in build.local.env at the repository root)')


def sdk():
    s = get('ANDROID_SDK', 'ANDROID_HOME', 'ANDROID_SDK_ROOT')
    if not s or not Path(s).is_dir():
        fail(f'ANDROID_SDK is not set or not a folder ({s!r})')
    return Path(s)


def ndk():
    n = get('ANDROID_NDK', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')
    if not n:
        s = get('ANDROID_SDK', 'ANDROID_HOME', 'ANDROID_SDK_ROOT')
        cands = sorted(Path(s).glob('ndk/29.*'), reverse=True) if s else []
        n = str(cands[0]) if cands else None
    if not n or not (Path(n) / 'toolchains' / 'llvm').is_dir():
        fail(f'ANDROID_NDK (an NDK r29 folder) is not set or not an NDK ({n!r})')
    return Path(n)


def ndk_bin():
    pre = ndk() / 'toolchains' / 'llvm' / 'prebuilt'
    hosts = [p for p in pre.iterdir() if p.is_dir()] if pre.is_dir() else []
    if not hosts:
        fail(f'no prebuilt toolchain in {pre}')
    return hosts[0] / 'bin'


def build_tools():
    bt = sdk() / 'build-tools' / get('BUILD_TOOLS', default='36.1.0')
    if not bt.is_dir():
        fail(f'no build-tools {bt.name} in the SDK (BUILD_TOOLS=<version> picks another)')
    return bt


def android_jar():
    j = sdk() / 'platforms' / get('ANDROID_PLATFORM', default='android-36') / 'android.jar'
    if not j.is_file():
        fail(f'no {j} (ANDROID_PLATFORM=android-<N> picks another)')
    return j


def java_home():
    j = get('JAVA_HOME')
    if not j or not (Path(j) / 'bin' / f'javac{EXE}').is_file():
        fail(f'JAVA_HOME (a JDK 17+) is not set or has no javac ({j!r})')
    return Path(j)


def unity_editor():
    u = get('UNITY_EDITOR')
    cands = [u] if u else ([r'C:\Program Files\Unity\Hub\Editor\2018.1.4f1\Editor', r'C:\Program Files\Unity\2018.1.4f1\Editor']
                           if WIN else ['/Applications/Unity/Hub/Editor/2018.1.4f1/Unity.app/Contents',
                                        '/Applications/Unity/Unity.app/Contents'])
    for c in cands:
        p = Path(c)
        data = p / 'Data' if (p / 'Data').is_dir() else p
        if (data / 'il2cpp' / 'libil2cpp').is_dir() and (data / 'PlaybackEngines' / 'AndroidPlayer').is_dir():
            return data
    fail('UNITY_EDITOR: no Unity 2018.1.4f1 Editor with Android Build Support (IL2CPP) found '
         f'(tried {", ".join(cands)}); it must contain Data/il2cpp/libil2cpp and Data/PlaybackEngines/AndroidPlayer')


def version():
    """app/VERSION 'X.Y.Z' -> (name, versionCode = 100000 + X*10000 + Y*100 + Z), the sibling ports' formula"""
    v = (REPO / 'app' / 'VERSION').read_text(encoding='utf-8').strip()
    x, y, z = (int(p) for p in v.split('.'))
    if not (0 <= y < 100 and 0 <= z < 100):
        fail(f'app/VERSION {v}: minor and patch must be 0..99')
    return v, 100000 + x * 10000 + y * 100 + z
