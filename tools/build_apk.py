#!/usr/bin/env python3
"""Pillars of Eternity Mobile: build and sign the APK by hand (no Unity editor build, no Gradle).

  package org.pillars.mobile, label "Pillars of Eternity Mobile", minSdk 26, targetSdk 29, version from app/VERSION
  classes.dex            Unity's own, from <Unity>/PlaybackEngines/AndroidPlayer/Variations/il2cpp/<variant>/Classes, unchanged
  classes2.dex           our front end (app/frontend/java, from frontend-common) + the F2 activity (app/java)
  lib/arm64-v8a/         libmain.so, libil2cpp.so (ours: tools/build_native.py), libunity.so (Unity's, unchanged)
  assets/                the on-screen pad's themes (app/front-assets), boot.config, Unity's "unity default resources"
                         (assets/f2/unity_default_resources, from the same Unity install as libunity.so), a placeholder
                         Managed/ folder (the engine insists on one; the game's metadata is read from the data folder)
  res/                   Unity's styles and app icon from the install, our launcher icon (app/frontend/res)
Nothing from the game is inside: the game image, its il2cpp metadata and data come from the folder setup prepares.

usage: python tools/build_apk.py [Release|Development]     -> dist/PillarsOfEternity-Mobile-v<version>-arm64.apk
       (Development: dist/PillarsOfEternity-Mobile-v<version>-arm64-development.apk, Unity's development player)
Tool locations and the signing key: tools/buildcfg.py / docs/BUILD.md. Run tools/build_native.py first (build.bat does both).
POE_APK_FULL=1: a developer APK that also carries the game image and metadata from work/setup (never for distribution).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buildcfg as cfg  # noqa: E402

REPO = cfg.REPO
APP = REPO / 'app'
FRONT_JAVA = APP / 'frontend' / 'java'
FRONT_RES = APP / 'frontend' / 'res'
FRONT_ASSETS = APP / 'front-assets'
F2_JAVA = APP / 'java'
NATIVE = REPO / 'work' / 'build' / 'native'
W = REPO / 'work' / 'build' / 'apk'
DIST = REPO / 'dist'
PACKAGE = 'org.pillars.mobile'   # the application id (the Java classes stay in package org.pillars.port)
LABEL = 'Pillars of Eternity Mobile'
BUILD_ID = 'f2f2f2f2-0001-4f32-9000-706f65663261'     # unity.build-id (any GUID; names the OBB the engine would look for)
DEV_FULL = os.environ.get('POE_APK_FULL') == '1'

MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}"
    android:versionCode="{code}" android:versionName="{name}" android:installLocation="internalOnly">
    <uses-sdk android:minSdkVersion="26" android:targetSdkVersion="29" />
    <supports-screens android:smallScreens="true" android:normalScreens="true" android:largeScreens="true" android:xlargeScreens="true" android:anyDensity="true" />
    <uses-feature android:glEsVersion="0x00030000" />
    <uses-feature android:name="android.hardware.vulkan.version" android:required="false" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.READ_EXTERNAL_STORAGE" />
    <uses-permission android:name="android.permission.WRITE_EXTERNAL_STORAGE" />
    <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />
    <application android:label="@string/app_name" android:icon="@drawable/icon" android:theme="@style/UnityThemeSelector"
        android:debuggable="true" android:extractNativeLibs="true" android:requestLegacyExternalStorage="true"
        android:isGame="true" android:allowBackup="false" android:hasCode="true" android:allowNativeHeapPointerTagging="false">
        <!-- the icon's entry: checks the data folder / asks for file access before any engine code runs, then starts DmActivity -->
        <activity android:name="org.pillars.port.StartActivity" android:label="@string/app_name" android:exported="true"
            android:screenOrientation="sensorLandscape" android:noHistory="true" android:theme="@android:style/Theme.DeviceDefault.NoActionBar"
            android:configChanges="orientation|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|uiMode|density">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
        <activity android:name="org.pillars.port.DmActivity" android:label="@string/app_name"
            android:screenOrientation="sensorLandscape" android:launchMode="singleTask" android:exported="true"
            android:configChanges="mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density"
            android:resizeableActivity="false" android:hardwareAccelerated="true">
            <meta-data android:name="unityplayer.UnityActivity" android:value="true" />
        </activity>
        <meta-data android:name="unity.build-id" android:value="{build_id}" />
        <meta-data android:name="unity.splash-mode" android:value="0" />
        <meta-data android:name="unity.splash-enable" android:value="false" />
        <meta-data android:name="unityplayer.SkipPermissionsDialog" android:value="true" />
        <meta-data android:name="android.max_aspect" android:value="2.4" />
    </application>
</manifest>
'''


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'FAILED: {" ".join(str(c) for c in cmd)[:300]}\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}')
    return r.stdout + r.stderr


def java_env():
    jh = cfg.java_home()
    return dict(os.environ, JAVA_HOME=str(jh), PATH=os.pathsep.join([str(jh / 'bin'), os.environ.get('PATH', '')])), jh / 'bin'


def make_res(player):
    res = W / 'res'
    if res.exists():
        shutil.rmtree(res)
    for sub in ('values', 'values-v21', 'drawable-mdpi'):
        (res / sub).mkdir(parents=True)
    (res / 'values/strings.xml').write_text(f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <string name="app_name">{LABEL}</string>\n</resources>\n', encoding='utf-8')
    shutil.copyfile(player / 'Apk/res/values/styles.xml', res / 'values/styles.xml')
    shutil.copyfile(player / 'Apk/res/values-v21/styles.xml', res / 'values-v21/styles.xml')
    shutil.copyfile(player / 'Apk/res/drawable-mdpi/app_icon.png', res / 'drawable-mdpi/app_icon.png')
    for sub in ('drawable-nodpi', 'drawable-anydpi-v26'):          # our launcher icon (icon.png + the adaptive pair)
        (res / sub).mkdir()
        for f in (FRONT_RES / sub).iterdir():
            shutil.copyfile(f, res / sub / f.name)
    return res


def front_dex(player, variant):
    """our front end + the F2 activity -> classes2.dex (Unity's classes.dex stays as it is)"""
    out = W / 'java'
    if out.exists():
        shutil.rmtree(out)
    (out / 'classes').mkdir(parents=True)
    srcs = sorted(FRONT_JAVA.rglob('*.java')) + sorted(F2_JAVA.rglob('*.java'))
    (out / 'sources.txt').write_text('\n'.join('"' + p.as_posix() + '"' for p in srcs), encoding='utf-8')
    unity_jar = player / 'Variations/il2cpp' / variant / 'Classes/classes.jar'
    env, jbin = java_env()
    jar = cfg.android_jar()
    r = subprocess.run([str(jbin / f'javac{cfg.EXE}'), '--release', '17', '-encoding', 'UTF-8', '-nowarn', '-Xlint:-options', '-d', str(out / 'classes'),
                        '-cp', os.pathsep.join([str(jar), str(unity_jar)]), '@' + str(out / 'sources.txt')], capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit('javac FAILED\n' + (r.stdout + r.stderr)[-6000:])
    run([jbin / f'jar{cfg.EXE}', 'cf', out / 'front.jar', '-C', out / 'classes', '.'], env=env)
    run([cfg.build_tools() / f'd8{cfg.BAT}', '--release', '--min-api', '26', '--lib', jar, '--classpath', unity_jar, '--output', out, out / 'front.jar'], env=env)
    n = sum(1 for _ in (out / 'classes').rglob('*.class'))
    print(f'   front end: {len(srcs)} sources, {n} classes -> classes2.dex {(out / "classes.dex").stat().st_size} bytes')
    return out / 'classes.dex'


def signer_args(tmp):
    """apksigner arguments for POE_SIGNING_KEY (nx2android signing-key.json, or a keystore), else a debug keystore made once in
    work/signing. A key converted from signing-key.json lives only in a temporary folder outside the repository while signing."""
    key = cfg.get('POE_SIGNING_KEY')
    if key:
        k = Path(key)
        if not k.is_file():
            cfg.fail(f'POE_SIGNING_KEY is not a file')
        if k.suffix.lower() == '.json':
            import json
            from Crypto.PublicKey import RSA
            j = json.loads(k.read_text(encoding='utf-8'))
            rsa = RSA.construct((int(j['n']), int(j['e']), int(j['d'])))
            (tmp / 'key.pk8').write_bytes(rsa.export_key(format='DER', pkcs=8))
            (tmp / 'cert.der').write_bytes(bytes.fromhex(j['cert']))
            print('   signing: nx2android key (POE_SIGNING_KEY, signing-key.json)')
            return ['--key', tmp / 'key.pk8', '--cert', tmp / 'cert.der']
        pw = cfg.get('POE_KEYSTORE_PASS')
        if not pw:
            cfg.fail('POE_KEYSTORE_PASS is needed for a keystore given in POE_SIGNING_KEY')
        print('   signing: keystore (POE_SIGNING_KEY)')
        args = ['--ks', k, '--ks-pass', 'env:POE_KEYSTORE_PASS']
        alias = cfg.get('POE_KEY_ALIAS')
        return args + (['--ks-key-alias', alias] if alias else [])
    ks = REPO / 'work' / 'signing' / 'debug.keystore'
    if not ks.is_file():
        ks.parent.mkdir(parents=True, exist_ok=True)
        env, jbin = java_env()
        run([jbin / f'keytool{cfg.EXE}', '-genkeypair', '-keystore', ks, '-storepass', 'android', '-keypass', 'android', '-alias', 'poe',
             '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=Pillars of Eternity Mobile (local build)'], env=env)
    print(f'   signing: local debug key {ks.relative_to(REPO)} (set POE_SIGNING_KEY to install over a build signed with another key)')
    return ['--ks', ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--ks-key-alias', 'poe']


def build(variant):
    data = cfg.unity_editor()
    player = data / 'PlaybackEngines' / 'AndroidPlayer'
    var = player / 'Variations/il2cpp' / variant
    if not (var / 'Libs/arm64-v8a/libunity.so').is_file():
        cfg.fail(f'no {var / "Libs/arm64-v8a/libunity.so"}: install Android Build Support (IL2CPP) for Unity 2018.1.4f1')
    for lib in ('libil2cpp.so', 'libmain.so'):
        if not (NATIVE / lib).is_file():
            sys.exit(f'build: no {NATIVE / lib}: run tools/build_native.py first')
    ver, code = cfg.version()
    name = ver if variant == 'Release' else f'{ver}-development'
    bt, jar = cfg.build_tools(), cfg.android_jar()
    if W.exists():
        shutil.rmtree(W)
    W.mkdir(parents=True)
    DIST.mkdir(parents=True, exist_ok=True)
    res = make_res(player)
    (W / 'AndroidManifest.xml').write_text(MANIFEST.format(package=PACKAGE, code=code, name=name, build_id=BUILD_ID), encoding='utf-8')
    run([bt / f'aapt2{cfg.EXE}', 'compile', '--dir', res, '-o', W / 'res.zip'])
    base = W / 'base.apk'
    run([bt / f'aapt2{cfg.EXE}', 'link', '-o', base, '-I', jar, '--manifest', W / 'AndroidManifest.xml', W / 'res.zip',
         '--min-sdk-version', '26', '--target-sdk-version', '29', '--version-code', str(code), '--version-name', name])
    files = [('classes.dex', var / 'Classes/classes.dex', False), ('classes2.dex', front_dex(player, variant), False)]
    for p in sorted(FRONT_ASSETS.rglob('*')):
        if p.is_file():
            files.append(('assets/' + p.relative_to(FRONT_ASSETS).as_posix(), p, True))
    files += [('lib/arm64-v8a/libmain.so', NATIVE / 'libmain.so', False),
              ('lib/arm64-v8a/libunity.so', var / 'Libs/arm64-v8a/libunity.so', False),
              ('lib/arm64-v8a/libil2cpp.so', NATIVE / 'libil2cpp.so', False)]
    boot = W / 'boot.config'
    boot.write_bytes(b'wait-for-native-debugger=0\n')
    files.append(('assets/bin/Data/boot.config', boot, True))
    # Unity's own Android-player asset (same origin as libunity.so): served by native/boot/f2_paths.c when the data folder has none
    files.append(('assets/f2/unity_default_resources', player / 'Data/unity default resources', True))
    if not DEV_FULL:
        # The stock player copies <apk>/assets/bin/Data/Managed to <files>/il2cpp and quits with "Failed to extract resources needed by
        # Il2CPP" when the apk has no such folder: one file of our own; the game's metadata is read from the data folder (f2_diag.cpp).
        ph = W / 'managed_placeholder.txt'
        ph.write_bytes(b'The il2cpp metadata of the game is read from the game data folder (RomFS/Data/Managed), not from this apk.\r\n')
        files.append(('assets/bin/Data/Managed/f2_placeholder.txt', ph, True))
    else:
        setup = REPO / 'work' / 'setup'
        gd = next((setup / 'dump' / '0100D6200E130000_build' / 'extract').glob('v*/RomFS/Data'), None)
        if not gd or not (setup / 'game' / 'libf2game.so').is_file():
            sys.exit('POE_APK_FULL=1 needs a finished setup in work/setup')
        files.append(('lib/arm64-v8a/libf2game.so', setup / 'game' / 'libf2game.so', False))
        for p in sorted((gd / 'Managed').rglob('*')):
            if p.is_file():
                files.append(('assets/bin/Data/Managed/' + p.relative_to(gd / 'Managed').as_posix(), p, True))
    with zipfile.ZipFile(base, 'a') as z:
        for nm, src, stored in files:
            zi = zipfile.ZipInfo(nm, date_time=(2026, 11, 11, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with open(src, 'rb') as f:
                z.writestr(zi, f.read(), compresslevel=None if stored else 6)
    aligned = W / 'aligned.apk'
    run([bt / f'zipalign{cfg.EXE}', '-f', '-p', '4', base, aligned])
    suffix = ('' if variant == 'Release' else '-development') + ('-devfull' if DEV_FULL else '')
    apk = DIST / f'PillarsOfEternity-Mobile-v{ver}-arm64{suffix}.apk'
    env, _ = java_env()
    with tempfile.TemporaryDirectory(prefix='poe-sign-') as td:
        run([bt / f'apksigner{cfg.BAT}', 'sign'] + signer_args(Path(td)) + ['--min-sdk-version', '26', '--out', apk, aligned], env=env)
    verify = run([bt / f'apksigner{cfg.BAT}', 'verify', '--verbose', '--print-certs', apk], env=env)
    badging = run([bt / f'aapt2{cfg.EXE}', 'dump', 'badging', apk])
    idsig = Path(str(apk) + '.idsig')
    if idsig.exists():
        idsig.unlink()
    sha = hashlib.sha256(apk.read_bytes()).hexdigest()
    print(f'== {apk.relative_to(REPO)}  {apk.stat().st_size} bytes  sha256 {sha}')
    for l in badging.splitlines():
        if l.startswith(('package:', 'sdkVersion', 'targetSdkVersion', 'application-label:', 'native-code', 'launchable-activity')):
            print('  ', l[:200])
    for l in verify.splitlines():
        if 'Verified using' in l or 'certificate SHA-256' in l:
            print('  ', l.strip()[:140])


if __name__ == '__main__':
    v = sys.argv[1] if len(sys.argv) > 1 else 'Release'
    if v not in ('Release', 'Development'):
        sys.exit('usage: build_apk.py [Release|Development]')
    build(v)
