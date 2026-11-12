# Building the APK (developers)

Players do not need this: they install a released APK and only run the setup (`put-game-files-here/README.txt`).

## What you need

| Tool | Version used | How the build finds it |
|---|---|---|
| Unity 2018.1.4f1 **with Android Build Support (IL2CPP)** | 2018.1.4f1 | `UNITY_EDITOR` = its `Editor` folder (default: `C:\Program Files\Unity\Hub\Editor\2018.1.4f1\Editor`, or `...\Unity\2018.1.4f1\Editor`) |
| Android NDK | r29 (29.0.14206865) | `ANDROID_NDK` (or `ANDROID_NDK_HOME` / `ANDROID_NDK_ROOT`, else the newest `ndk/29.*` of the SDK) |
| Android SDK: build-tools and a platform | build-tools 36.1.0, platform android-36 | `ANDROID_SDK` (or `ANDROID_HOME` / `ANDROID_SDK_ROOT`); `BUILD_TOOLS`, `ANDROID_PLATFORM` pick other versions |
| JDK | 17 or newer (21 used) | `JAVA_HOME` |
| Python | 3.12+ with pycryptodome (only to read an nx2android `signing-key.json`) | `build.bat` uses `tools\.runtime` (made by `setup.bat`) when it exists, else `python` |

Each key can be an environment variable or a line `KEY=VALUE` in `build.local.env` at the repository root (git-ignored), e.g.

    UNITY_EDITOR=D:/Unity/2018.1.4f1/Editor
    ANDROID_SDK=D:/Android/Sdk
    ANDROID_NDK=D:/Android/Sdk/ndk/29.0.14206865
    JAVA_HOME=D:/jdk-21
    POE_SIGNING_KEY=D:/keys/signing-key.json

Nothing of Unity is stored in this repository. The build takes `libunity.so`, Unity's `classes.dex` / `classes.jar`, its
`styles.xml` and default icon, `unity default resources` and the il2cpp runtime source from your install.

## Build

    build.bat                      (Windows: native libraries + signed release APK)
    build.bat Development          (Unity's development player instead of the release one)

or step by step (any OS with the tools above):

    python tools/build_native.py all     -> work/build/native/{libil2cpp.so, libmain.so}
    python tools/build_apk.py Release    -> dist/PillarsOfEternity-Mobile-v<version>-arm64.apk

`tools/build_native.py` copies the il2cpp runtime of your Unity install to `work/build/il2cpp-src`, adds the six F2 hooks
(`IL2CPP_HOOKS`: stack-trace sentries like the Switch build, the game image is mapped before the runtime initialises, its data
and bss become GC roots, each thread gets the x18 slot block), compiles it the way Unity's Android toolchain does (clang,
`aarch64-linux-android21`, `-O2`, asserts on) and links it with our code (`app/native/{loader,boot,nn}`, `aarch64-linux-android26`)
into `libil2cpp.so`. Source paths are mapped to `il2cpp/...` and `app/native/...`, so the libraries carry no path of the build PC.
`F2_SKIP=a.c,b.c` leaves modules out.

`tools/build_apk.py` builds the APK by hand (aapt2, javac, d8, zipalign, apksigner; no Gradle, no Unity editor build): package
`org.pillars.mobile`, label "Pillars of Eternity Mobile", minSdk 26, targetSdk 29.

## Version

`app/VERSION` holds `X.Y.Z`; the APK gets versionName `X.Y.Z` and versionCode `100000 + X*10000 + Y*100 + Z` (0.1.0 -> 100100),
the formula of the sibling ports. Release files are named `PillarsOfEternity-Mobile-vX.Y.Z-arm64.apk`.

## Signing

- `POE_SIGNING_KEY=<signing-key.json>`: nx2android's key format (RSA n / e / d and the self-signed certificate, made by nx2android's
  porter in its user folder). An APK signed with the same key installs over earlier builds of the same package. The key is
  converted in a temporary folder outside the repository for apksigner and removed after signing; it is never printed.
- `POE_SIGNING_KEY=<file>.jks` / `.keystore` with `POE_KEYSTORE_PASS` (and `POE_KEY_ALIAS`): a Java keystore.
- Not set: a local debug key is made once in `work/signing/debug.keystore` (an APK signed with it does not install over one
  signed with another key: uninstall first, after backing up the save).

## Developer options

- `POE_APK_FULL=1 python tools/build_apk.py`: an APK that also carries `libf2game.so` and the game's `Managed/` metadata from a
  finished `work/setup`, for native work without the data folder's game image. It contains game code: never distribute it.
- The setup's steps run on their own: `python tools/setup_poe.py --steps image,overlay --texture bc --yes`.
- `python tools/poe_unpack.py --help`: the nx2android part of the setup alone.
