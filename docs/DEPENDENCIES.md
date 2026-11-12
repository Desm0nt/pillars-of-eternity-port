# What you need

## Players: setup (preparing the game folder on a PC)

| Need | Details |
|---|---|
| Your game files | Pillars of Eternity: Complete Edition for Switch, base game `0100D6200E130000` v0 and update `0100D6200E130800` v393216 (2.60.00), as NSP, NSZ, XCI or XCZ, and `prod.keys` of your console. See `put-game-files-here/README.txt`. |
| Windows 10 / 11 (x64), or Linux / macOS | Windows: `setup.bat`. Linux / macOS: `./setup.sh` (bash). **Git for Windows is not needed**: setup on Windows runs in PowerShell and Python only. |
| Python 3.12 or newer | Windows: found automatically (py launcher, PATH); when there is none, `setup.bat` offers a private Python 3.12 (python.org's official NuGet package) in `tools\.runtime\python`. Linux / macOS: your system's `python3` 3.12+ with `venv`. The packages (`tools/requirements.txt`, ~60 MB from PyPI) go into a private virtual environment in `tools/.runtime`; nothing is installed system-wide. |
| .NET 10 SDK | Builds the dump extractor and the shader translator. Used when installed (`dotnet` on PATH, `~/.dotnet`, or `DOTNET=<path to dotnet>`); otherwise setup offers a private copy (Microsoft's dotnet-install script, ~300 MB download, ~800 MB on disk) in `tools/.runtime/dotnet`. |
| Internet | On the first run only: Python packages, the .NET SDK if missing, and the NuGet packages of the extractor and translator. A second run needs no network. |
| Disk space | Free space on the drive of the repository folder: about **90 GB** when the run starts (nx2android's extractor checks for twice the decompressed game plus the decompressed NSZ copies, 86 GB for this game, and stops before it starts with less); the work folder itself peaks at about 45 GB while the decompressed NSZ copies (28 GB) exist. Kept afterwards in `work/`: about 30 GB (extracted game 15.6 GB, ASTC set 12.7 GB, overlay 0.7 GB). The folder to copy, `work/PillarsOfEternityMobile`, is made of hard links to those files, so it takes no extra space on the same drive: 16 GB with BC textures, 15 GB with ASTC, 28 GB with both. Delete `work/setup` after copying the folder to the phone if you need the space back (setup then has to start over). |
| Time | Measured: 18 minutes (1066 s) for `--texture both` on a 12-thread PC with the .NET SDK already installed: unpack 4.7 min (NSZ decompression 2.7 min), game image 1 s, overlay 0.5 min, ASTC textures 12.5 min (only with `astc` or `both`), layout 2 s. Expect 20-40 minutes on slower PCs and on a first run that downloads Python packages, the .NET SDK and NuGet packages. |
| Optional | `spirv-val` (Android NDK `shader-tools`, Vulkan SDK, or on PATH): setup then validates the rewritten shaders; the output is the same without it. |

Nothing is uploaded; your dumps are only read (setup makes hard links to them in its work folder; when they are on another drive
than the repository it copies them there instead, which takes their size once more: 14 GB for the NSZ files).

## Players: the phone

An arm64 Android phone with Android 8.0 or newer, Vulkan, and room for the game folder (16 to 28 GB). The APK asks for "All files
access" on Android 11 and newer, to read `Download/PillarsOfEternityMobile`.

## Developers: building the APK

Unity 2018.1.4f1 with Android Build Support (IL2CPP), Android NDK r29, Android SDK build-tools 36.1.0 + platform android-36, a
JDK 17+ and Python 3.12+: see [BUILD.md](BUILD.md). Third-party code and where it comes from: [THIRD_PARTY.md](THIRD_PARTY.md).
