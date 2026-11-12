# Installing Pillars of Eternity Mobile — step by step

Русская версия: [INSTALL.ru.md](INSTALL.ru.md).

Pillars of Eternity Mobile runs **your own copy** of Pillars of Eternity: Complete Edition for Nintendo Switch on an
Android phone. This project contains no game files and never shares game files or console keys: you dump them from a
Switch you own. A setup program on your PC turns your dump into a game folder (15–28 GB, depending on the texture set),
you copy that folder to the phone and install the app.

Setup takes 20–40 minutes (18 minutes on a fast PC); copying the folder to the phone comes on top of that.

## What you need

**From your Switch** (see step 1):

| What | Details |
|---|---|
| Pillars of Eternity: Complete Edition, base game | title id `0100D6200E130000`, from your cartridge or eShop copy |
| Pillars of Eternity update **v393216** (2.60.00) | title id `0100D6200E130800`. Only this exact update works (the port is made for its code; setup checks it). Dump tools usually put it in the file name, e.g. `...[0100D6200E130800][v393216].nsp`. A cartridge dump (XCI) that already contains this update counts as both |
| `prod.keys` | the keys of **your own** console |

Both parts of The White March are part of the Complete Edition: nothing extra to dump.

The game and the update can be **NSP, NSZ, XCI or XCZ**. File names do not matter: setup recognises the files by their
content.

**A PC** with Windows 10/11 (64-bit). `setup.sh` for Linux / macOS (bash, your system's Python 3.12+ with `venv`) is
provided but has not been tested on those systems yet.

- about **90 GB of free disk space** on the drive of the setup folder when setup starts (the dump extractor checks this
  and stops at once with less). The work folder peaks at about 45 GB; about 30 GB stay in `work/` afterwards, and the
  folder for the phone is made of hard links to those files, so it takes no extra space. If your dumps are on another
  drive, setup copies them over first (14 GB more for the NSZ files);
- internet on the first run: setup downloads what is missing — a private Python 3.12 (~15 MB, only when your PC has no
  Python 3.12 or newer), the Python packages (~60 MB from PyPI), the .NET 10 SDK (~300 MB download, ~800 MB on disk,
  only when none is installed) and the NuGet packages of the extractor and shader translator. It asks before each
  download and puts everything into `tools\.runtime` inside its own folder. Later runs work offline. Git for Windows is
  **not** needed.

**A phone:**

- Android 8.0 or newer, 64-bit, with Vulkan;
- **Snapdragon with an Adreno GPU recommended** (tested on Snapdragon 8 Gen 3; Mali / PowerVR / Xclipse phones get
  their own texture set, see step 4, and are untested);
- **16 GB free** in internal storage with BC textures, 15 GB with ASTC, 28 GB with both;
- a USB cable (or another way to copy the folder to the phone).

## Step 1. Get the game files and keys from your Switch

**Dumping them yourself.** This needs a Switch that can run homebrew (how to get there is not covered here).
1. Install the game and the update v393216 on the Switch.
2. Dump them with [nxdumptool](https://github.com/DarkMatterCore/nxdumptool): installed or eShop copy → *NSP*;
   cartridge → *XCI* (an update installed separately is dumped as NSP). For NSP dumps turn on
   **"remove console specific data"** — otherwise the update cannot be read on a PC.
3. Dump the console keys with [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM). You get `prod.keys` on the SD card
   in `switch/`.
4. Copy the files to your PC.

Do not download game files or keys from the internet, and do not share yours.

## Step 2. Get Pillars of Eternity Mobile setup

Download this repository as a ZIP (green **Code** button → **Download ZIP**) or clone it, and extract it to a disk with
enough free space, into a **short path** such as `D:\PoE-setup` (the deepest path inside the repository is 148
characters, and Windows has trouble beyond 260). You get a folder with `setup.bat`, `setup.sh` and a folder
`put-game-files-here`.

## Step 3. Put your files into `put-game-files-here`

```
<setup folder>/
├── setup.bat
├── setup.sh
└── put-game-files-here/
    ├── Pillars of Eternity ... [0100D6200E130000][v0] ... .nsz          (or .nsp / .xci / .xcz — the base game)
    ├── Pillars of Eternity ... [0100D6200E130800][v393216] ... .nsz     (the update)
    └── prod.keys
```

Your files are only read: setup makes hard links to them in its work folder and never changes or uploads them.

## Step 4. Run setup

1. **Double-click `setup.bat`.** If Windows warns about a file from the internet, choose **Run** (or **More info → Run anyway**).
2. Answer the download questions with **Y** and Enter (Enter alone also means yes): the private Python (*Download it
   now?*, only when your PC has no Python 3.12+), the Python packages (*Install them now?*) and the .NET SDK
   (*Download and install the .NET 10 SDK there now?*). Nothing is installed system-wide, no admin rights are needed.
3. Setup asks **which phone(s) will play this**:

   | Answer | Texture set | Folder on the phone |
   |---|---|---|
   | 1) Adreno (Qualcomm Snapdragon) | BC, the game's own textures; quickest setup | ~16 GB |
   | 2) Mali, PowerVR or Samsung Xclipse | ASTC, works on every GPU | ~15 GB |
   | 3) not sure / several phones (Enter) | both; the app picks the right set at every start | ~28 GB |

4. **Wait.** Setup prints a line when each stage is done. Measured on a 12-thread PC with the .NET SDK already there:

   | Stage | Took |
   |---|---|
   | 1. unpack: extract the dumps, prepare the game program, translate the shaders | 4.7 min (of it 2.7 min NSZ decompression) |
   | 2. image: the game image for the app | 1 s |
   | 3. overlay: the game files with shaders, rewritten for Android | 0.5 min |
   | 4. astc: the ASTC texture set (only for answers 2 and 3) | 12.5 min |
   | 5. layout: the folder for the phone | 2 s |

   A first run that downloads its tools and a slower PC take longer (20–40 minutes). If you close the window or the PC
   turns off, just run setup again: it continues where it stopped.
5. At the end it shows `DONE`, where the finished folder is, the `adb push` command to copy it, and what to do next.

Instead of using `put-game-files-here` you can also drag the files onto `setup.bat`.

### Not sure what is missing?

Run `setup.bat check` (in a terminal in the setup folder). It lists Python, the .NET SDK, the dumps and `prod.keys` it
finds, which stages are done and whether the folder for the phone exists, and changes nothing.

## Step 5. Copy the game folder to the phone

The finished folder is **`work/PillarsOfEternityMobile`** inside the setup folder.

- **USB file transfer:** connect the phone, choose **File transfer**, open the phone's **internal storage** →
  **Download** and copy the whole **`PillarsOfEternityMobile`** folder there. It is big, so give it time. It must end
  up exactly like this:

  ```
  Internal storage/Download/PillarsOfEternityMobile/RomFS/Data/...
  Internal storage/Download/PillarsOfEternityMobile/overlay/Data/...
  Internal storage/Download/PillarsOfEternityMobile/overlay-astc/Data/...   (only with the ASTC set)
  Internal storage/Download/PillarsOfEternityMobile/game/libf2game.so
  ```

- **adb (USB debugging):** setup prints the `adb push` command at the end.

When the copy is on the phone, you can delete `work/setup` on the PC to get the space back (setup then has to start over
if you need the folder again).

## Step 6. Install the app

Install `PillarsOfEternity-Mobile-v<version>-arm64.apk` from the
[latest release](https://github.com/Desm0nt/pillars-of-eternity-port/releases/latest), or build it yourself (see
*Building* in the [README](../README.md)). Android asks you to allow installing apps from your browser or file manager the
first time.

## Step 7. First launch

1. Start **Pillars of Eternity Mobile**.
2. The first time it shows a page asking for **All files access** (it needs it to read `Download/PillarsOfEternityMobile`).
   Turn it on and go back: the game starts.
3. The gear button opens the pause menu; *Game settings…* holds the port's options: frame-rate limit (30 / 60 / 90 /
   120, changes at once), picture shape (whole screen / 16:9 with bars), touch as the PC mouse, hold time for the right
   click, game language, FPS counter, on-screen gamepad.

**Playing by touch:** tap = left click (select, move, attack, talk, every menu and window), hold = right click, drag
with two fingers = move the camera, pinch = zoom, two-finger tap = pause, two fingers held = select the whole party, three-finger tap = close the window. If you prefer the console's gamepad controls, turn off *Touch plays as
the PC mouse* in the settings and use the on-screen gamepad (*On-screen gamepad…* in the pause menu: skin, opacity, size,
layout). A Bluetooth / USB controller works too; the on-screen pad hides while you use it.

Saves and settings stay in the app's own storage (`Android/data/org.pillars.mobile/files`). Installing a newer APK over
the old one keeps them; **uninstalling the app deletes your saves.**

## Troubleshooting / FAQ

**"image: main build id … is not the supported one"** — your update is not v393216 (2.60.00): dump that update.

**"no dump files found" / "no prod.keys found"** — check that the files are in `put-game-files-here` (setup also looks
in the setup folder itself), or drag them onto `setup.bat`.

**The extractor stops on the keys or on the update** — use the keys of the console you dumped from, made with an
up-to-date Lockpick_RCM (`title.keys` is a different file), and dump NSP files with nxdumptool's *remove console
specific data* option turned on.

**Setup stops at the start because of free space** — it needs about 90 GB free on the drive of the setup folder: free
some space, or move the whole setup folder (with your dumps) to a bigger drive.

**The app shows a page saying what is missing instead of starting** — the folder is not where the app looks, or file
access was not given. Check that the path on the phone is exactly `Download/PillarsOfEternityMobile/RomFS/Data`, allow
*All files access* on that page and check again. The settings also show the access status, with a button to the system
setting.

**Where is the language setting?** — gear button → *Game settings…* → *Game language (text and voice)*; it applies from
the next start of the game. The picture shape and the touch mode also apply from the next start; the frame-rate limit
changes at once.

**The game is slow / the phone gets hot** — lower the frame-rate limit in *Game settings…* (60 or 30); it changes at
once.

**Black screen or the app closes** — open an issue on this repository with the phone model, Android version and what
happened, and attach the whole `Download/PillarsOfEternityMobile/logs/` folder: after a crash the next start writes
`crash.txt` there; `boot.log` and `logcat.txt` of the current run and `boot.prev.log` / `logcat.prev.txt` of the one
before are written while the game runs. For setup problems, copy the setup window's text into the issue.

What setup needs and how long it takes, in detail: [DEPENDENCIES.md](DEPENDENCIES.md). What it does step by step:
`setup.bat --help`.
