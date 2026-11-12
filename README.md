# Pillars of Eternity Mobile

**A native Android port of Pillars of Eternity: Complete Edition for Nintendo Switch.** The game is a Unity 2018.1
title: instead of emulating the console's graphics API, the game's own compiled code from your Switch dump runs inside
the stock Android Unity player of the same engine version, with a small layer standing in for the console services. You
bring the game: this repository contains no game content and no keys.

Русский: [ниже](#по-русски).

**Install:** step-by-step guide for players — [docs/INSTALL.md](docs/INSTALL.md) (English) ·
[docs/INSTALL.ru.md](docs/INSTALL.ru.md) (по-русски). In short: dump your own game + update v393216 and `prod.keys`, put
them into `put-game-files-here/`, double-click `setup.bat`, copy `work/PillarsOfEternityMobile` to the phone's
`Download` folder and install the APK from the
[Releases page](https://github.com/Desm0nt/pillars-of-eternity-port/releases/latest) (or build it, see *Building*).

> [!WARNING]
> **Tested so far on one device: Snapdragon 8 Gen 3 / Adreno 750 (Xiaomi 14 Ultra, Android 16) with the phone's own
> Vulkan driver.** **Phones with Mali (Exynos, Dimensity, Tensor), PowerVR and Xclipse GPUs have not been tested at
> all** — setup can prepare a texture set for them (see *Other GPUs*), but expect problems. If something goes wrong on
> your device, please [open an issue](https://github.com/Desm0nt/pillars-of-eternity-port/issues) and **always attach
> the logs**: the whole folder `Download/PillarsOfEternityMobile/logs/` — `crash.txt` (written at the start after a
> crash), `boot.log` / `boot.prev.log` and `logcat.txt` / `logcat.prev.txt` — and name your phone model and Android
> version. Without logs a report usually can't be acted on.

## Screenshots

[SCREENSHOTS: owner takes them on the phone. Suggested set: main menu · character creation (the Watcher) · the first
area · a fight with the party · inventory with the character portrait · the pause menu · the game settings dialog. One
caption line above the table: phone, GPU, picture shape setting, game text language, which controls are shown. When the
pictures are in `docs/images/`, remove the comment markers around the table below and fix the file names.]

<!--
| | |
|---|---|
| ![Main menu](docs/images/main_menu.jpg) | ![Character creation](docs/images/character_creation.jpg) |
| ![First area](docs/images/first_area.jpg) | ![Fight](docs/images/combat.jpg) |
| ![Inventory and portrait](docs/images/inventory.jpg) | ![Pause menu (gear button)](docs/images/pause_menu.jpg) |

![Port settings](docs/images/settings.jpg)
-->

[VIDEO: optional link to a short gameplay clip, if the owner records one.]

## What it is, and what it is not

**It is:**

- The original game code **executing natively** on the phone — no emulated CPU, no JIT, no emulated console GPU.
  Pillars of Eternity is a Unity 2018.1 (IL2CPP) game; its compiled code from your Switch dump is loaded into the
  **stock Android Unity player 2018.1.4f1**, which draws with its own Vulkan renderer on your phone's own driver.
- A small layer standing in for the Switch services the game asks for (user account, save data, controllers, system
  language), plus the fixes the console build needs to run under the Android player.
- A PC setup that turns your dump into a game folder: it unpacks the game with nx2android's extractor, prepares the
  game program, translates the game's shaders for Vulkan and, if you ask for it, converts the textures into a set for
  phones whose GPU cannot read the console's texture format.
- A phone port with **touch controls that play like the PC mouse**, an on-screen gamepad, game controllers, a picture
  that fills the phone's screen and a frame-rate limit up to 120 fps (see *Features*).

**It is not:**

- An emulator, cloud gaming or streaming.
- **A source of game content.** There is no game program, data, shader, text, art or key in this repository, and the
  APK contains none. You dump the game from a Switch you own and convert it on your PC. What the repository knows about
  the game (addresses, instruction words, format descriptions…) is listed in [docs/DERIVED.md](docs/DERIVED.md).
- Affiliated with the makers or publishers of Pillars of Eternity, with Unity Technologies or with Nintendo.

## Features

**The game**

- **Pillars of Eternity: Complete Edition** from your own dump: the base game and **both parts of The White March**
  expansion, which are part of the Complete Edition and are switched on.
- Saves and character portraits work as on the console.
- Game language from the settings (English, Russian, German, French, Spanish, Italian, Polish) or left as the system
  says.

**Controls**

- **Touch as the PC mouse**: tap = left click (select, move, attack, talk, every menu and window), hold = right click,
  two-finger drag = move the camera, pinch = zoom. The hold time for the right click can be set (300–900 ms).
- **On-screen gamepad** with floating sticks and the console's own gamepad controls, for players who prefer them (turn
  *Touch plays as the PC mouse* off): skins, opacity, size, stick dead zone and a layout editor (move, resize, hide)
  from the shared front end of the DOOM / Diablo III / Torchlight II ports.
- **Game controllers** (Bluetooth / USB) work; the on-screen pad hides while a controller is used.

**Picture and speed**

- **The picture fills the phone's screen** in its own shape (e.g. 2000×900 on a 20:9 phone), nothing stretched; or
  16:9 with bars, if you prefer.
- **Frame-rate limit 30 / 60 / 90 / 120**, switched from the settings while playing, no restart needed.
- FPS counter (can be turned off).

**Front end shared with the DOOM / Diablo III / Torchlight II ports**

- Gear button → **pause menu**: *Resume*, *Game settings…*, *On-screen gamepad…* (skin, opacity, size, layout, floating
  stick), *Edit the layout*, show / hide the on-screen gamepad, *Quit game* (asks first). The game itself keeps running
  behind this menu.
- Port settings (*Game settings…*): frame-rate limit, picture shape, touch as the PC mouse, hold time for the right
  click, access to the Download folder (status and a button to the system setting), game language, FPS counter, the
  gear button's look, on-screen gamepad, stick dead zone, menu language (Russian / English).

## Current state

Tested on **Xiaomi 14 Ultra** (Snapdragon 8 Gen 3 / Adreno 750, Android 16) with the phone's own Vulkan driver.

- Main menu at **120 fps**, gameplay at **about 87 fps** with the 120 limit.
- New game, character creation, the first area, saving and loading, touch, the on-screen pad, a physical gamepad, live
  frame-rate switching, portraits.

### Known limitations

- Only update **v393216** (2.60.00) is supported; setup checks the build id of the game program (`main` 68976c70…).
- Only one device has been tested; the texture set for other GPUs (Mali, PowerVR, Xclipse) is prepared by setup but
  has not run on such a phone yet (see *Other GPUs*).
- No vibration yet.
- The PC's drag-to-select box is not available (the console build has no code path for it).
- Saves live in the app's own storage (`Android/data/org.pillars.mobile/files/save`): installing a newer APK over the old
  one keeps them, **uninstalling the app deletes them**.

## Other GPUs (Mali, PowerVR, Xclipse)

The game ships most of its textures in a format (BC) that Adreno reads directly but Mali, PowerVR and the stock Xclipse
driver do not; the Android player would then unpack them on the CPU while loading, which costs memory and loading time.
Setup can therefore build an **ASTC copy** of those textures: it asks which phone the game is for (Adreno → BC;
Mali, PowerVR or Xclipse → ASTC; not sure / several phones → both). At every start the app checks what the GPU can read
and picks the right set by itself; one boot.log line says which (`texture set = ...`).

The ASTC set has been built and checked on the PC, **not yet on a phone without BC**. Reports (device, GPU, Android
version, the `logs` folder) are very welcome.

## Android versions and CPUs

- `PillarsOfEternity-Mobile-v<version>-arm64.apk`: Android 8.0 or newer (minSdk 26), Vulkan required. On Android 11 and
  newer the app asks for *All files access* to read `Download/PillarsOfEternityMobile`.
- 64-bit ARM only.

## Logs and crash reports

Every start writes a fresh `boot.log` and a live `logcat.txt` into `Download/PillarsOfEternityMobile/logs/` (the
previous run is kept as `boot.prev.log` / `logcat.prev.txt`): the device, GPU, texture set and the game's progress.
When the game crashes or is killed while playing, the next start writes **`crash.txt`** there (the previous one is kept
as `crash.prev.txt`). Attach the whole `logs` folder to a bug report.

## Building

Players do not need this. Full guide: [docs/BUILD.md](docs/BUILD.md).

Requirements: **Unity 2018.1.4f1 with Android Build Support (IL2CPP)** (the build takes Unity's Android player and the
il2cpp runtime source from your install; nothing of Unity is stored here), Android NDK r29 (29.0.14206865), Android SDK
build-tools 36.1.0 + platform android-36, JDK 17+, Python 3.12+. Tool locations come from the environment or from a
git-ignored `build.local.env`.

```sh
build.bat                      # Windows: native libraries + signed release APK
build.bat Development          # Unity's development player instead of the release one
# or step by step, on any OS with the tools above:
python tools/build_native.py all
python tools/build_apk.py Release
```

Output: `dist/PillarsOfEternity-Mobile-v<version>-arm64.apk`, app id `org.pillars.mobile` ("Pillars of Eternity
Mobile"), minSdk 26, targetSdk 29. The version comes from `app/VERSION`. Signing: `POE_SIGNING_KEY` (see BUILD.md);
without it a local debug key is made once. `work/` is git-ignored and holds every build output and every game-derived file.

## Installing

Full guide: **[docs/INSTALL.md](docs/INSTALL.md)** (English) / **[docs/INSTALL.ru.md](docs/INSTALL.ru.md)** (Russian).
What setup needs, measured: [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

1. Dump your own copy (base game `0100D6200E130000` + update `0100D6200E130800` **v393216** (2.60.00), as NSP / NSZ /
   XCI / XCZ) and your console's `prod.keys` — e.g. with nxdumptool and Lockpick_RCM on your Switch.
2. Put the files into `put-game-files-here/` (any names) and double-click `setup.bat`. `./setup.sh` is provided for
   Linux / macOS but has not been tested there yet. `setup.bat check` reports what is found, installed and prepared and
   changes nothing. Setup provides Python 3.12 and its packages and the .NET 10 SDK in `tools/.runtime` when they are
   missing (it asks first), needs about **90 GB free** when it starts, asks which phone the game is for, then unpacks
   the game, makes the game image, rewrites the shaders, builds the ASTC set if needed and lays out
   `work/PillarsOfEternityMobile` (16 GB with BC textures, 15 GB with ASTC, 28 GB with both). A full run took 18 minutes
   on the developer's PC; expect 20–40 minutes on slower PCs. Every stage resumes where it stopped.
3. Copy `work/PillarsOfEternityMobile` to the phone's `Download` folder (USB cable, *File transfer* mode, or the
   `adb push` command setup prints) and install the APK.
4. Start *Pillars of Eternity Mobile* and allow *All files access* when asked.

## Repository layout

| Path | What |
|---|---|
| `app/native/` | The native layer linked into `libil2cpp.so`: the game-image loader (`loader`), the boot layer with the Unity player hooks, picture, frame rate, touch, texture-set choice and crash handler (`boot`), and the stand-ins for the Switch SDK calls the game makes (`nn`) |
| `app/java/` | The port's Android activities: start page and file access (`StartActivity`), the game activity over Unity's player (`DmActivity`), touch as the PC mouse (`F2Touch`), the port's settings (`F2Gear`), the data folder (`GameFolder`), logs and crash reports (`CrashLog`) |
| `app/frontend/`, `app/front-assets/` | The shared front end of the author's ports (pause menu, gear dialog, on-screen gamepad, skins) |
| `profile/0100D6200E130000/` | The game profile for nx2android's porter: patches of `main`, memory layout, shader tables |
| `setup.bat`, `setup.sh`, `build.bat` | Game file preparation (`tools/setup_poe.py`) and APK build entry points |
| `tools/` | Setup (`setup_poe.py`, `poe_unpack.py`, `bootstrap.ps1`), the overlay and ASTC tools (`tools/overlay`), the native and APK builds |
| `third_party/nx2android/` | The vendored subset of nx2android (MIT): porter, dump extractor, `main` rewriter, shader split and translator |
| `put-game-files-here/` | Where your dumps and `prod.keys` go (only its README is tracked) |
| `docs/` | Build guide, setup requirements, game-derived information, third-party code |

## Credits

Pillars of Eternity Mobile stands on the work of others — thank you:

- **[nx2android](https://github.com/shirai91/nx2android)** (MIT, seedds) — the Android port of
  [nxnative](https://github.com/seedds/nxnative); a subset of it is vendored in `third_party/nx2android` and used by
  setup: porter, the dump extractor, the `main` rewriter, the shader split and translator, the Unity / NSO readers. Its
  gamepad themes and default layout are the on-screen pad's skins.
- **Ryujinx** shader translator 1.3.2 (Ryujinx Team and Contributors, MIT, [Ryubing](https://github.com/Ryubing)), as
  nx2android vendors it, and the **MacNativeGames** code adapted in nx2android's translator (MIT).
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** by jhaizhou (MIT) — the Android app the shared front end
  descends from, by way of **[DOOM Mobile](https://github.com/Desm0nt/doom-port)**; together with
  **[Diablo III Mobile](https://github.com/Desm0nt/diablo3-port)** and
  **[Torchlight II Mobile](https://github.com/Desm0nt/torchlight2-port)**: the gear dialog, pause menu, on-screen
  gamepad, layout editor, logs and crash reports, the setup and install flow.
- **[SMOL-V](https://github.com/aras-p/smol-v)** by Aras Pranckevicius (public domain) — the opcode table of the SMOL-V
  decoder that rewrites the game's shaders.
- **[AssetRipper](https://github.com/AssetRipper/AssetRipper)** — the Unity 2018.1.4f1 type trees of 16 engine classes
  were read from the type-tree database it embeds.
- **Unity** 2018.1.4f1 Android player and il2cpp runtime (Unity Technologies) — taken from the developer's own install at
  build time, not stored here; Unity's license terms apply to them and to an APK that carries them.
- Installed by setup on your PC, not stored here: **Python** and the PyPI packages pycryptodome, backports.zstd, lz4,
  capstone, numpy, **astc-encoder-py** (MIT; bundles Arm's [astc-encoder](https://github.com/ARM-software/astc-encoder),
  Apache-2.0), texture2ddecoder (MIT) and archspec; the **.NET 10 SDK** and the NuGet packages of the extractor and
  translator (LibHac 0.19.0 and Ryujinx's pinned packages).

Details per file: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

Pillars of Eternity and all related names are trademarks of their respective owners. Nintendo Switch is a trademark of
Nintendo. Unity is a trademark of Unity Technologies. This is an unofficial, non-commercial fan project with no
affiliation to any of them. **You must own the game and dump it yourself**: no game files, no keys and no game-derived
output are included or distributed here; play it with a copy you own.

## License

MIT — see [LICENSE](LICENSE). The d2r-mobile copyright notice is kept, as in the sibling repositories (the shared front
end descends from it); third-party components keep their own licenses ([docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)).

---

## По-русски

> [!WARNING]
> **Проверено только на одном устройстве: Snapdragon 8 Gen 3 / Adreno 750 (Xiaomi 14 Ultra, Android 16) на системном
> драйвере.** Телефоны с GPU **Mali** (Exynos, Dimensity, Tensor), **PowerVR** и **Xclipse** не тестировались вообще —
> набор текстур для них установка готовит, но проблемы вероятны. Если что-то не работает,
> [откройте issue](https://github.com/Desm0nt/pillars-of-eternity-port/issues) и **обязательно приложите логи** — всю
> папку `Download/PillarsOfEternityMobile/logs/`: `crash.txt` (появляется при запуске после падения), `boot.log` /
> `boot.prev.log` и `logcat.txt` / `logcat.prev.txt`, и укажите модель телефона и версию Android.

**Pillars of Eternity Mobile** — нативный порт Pillars of Eternity: Complete Edition для Nintendo Switch на Android.
Игра сделана на Unity 2018.1, поэтому здесь не эмулируется ни процессор, ни графика консоли: собственный скомпилированный
код игры из вашего дампа Switch работает внутри штатного Android-плеера Unity той же версии, а небольшой слой подменяет
системные сервисы консоли (учётная запись, сохранения, геймпад, язык).

- **Игры в репозитории нет.** Нужна своя копия Pillars of Eternity: Complete Edition для Switch (база + обновление
  v393216, оно же 2.60.00) и свои ключи консоли. **Установка по шагам: [docs/INSTALL.ru.md](docs/INSTALL.ru.md).** APK —
  на странице [Releases](https://github.com/Desm0nt/pillars-of-eternity-port/releases/latest). Что репозиторий знает об
  игре — [docs/DERIVED.md](docs/DERIVED.md) (англ.).
- **Вся игра:** основная кампания и обе части дополнения The White March — они входят в Complete Edition и включены.
  Сохранения и портреты персонажей работают как на консоли. Язык игры выбирается в настройках (английский, русский,
  немецкий, французский, испанский, итальянский, польский).
- **Касания как мышь ПК:** тап — левая кнопка (выбор, ходьба, атака, разговор, любые меню и окна), удержание — правая
  кнопка, движение двумя пальцами — сдвиг камеры, щипок — зум. Время удержания для правой кнопки настраивается.
- **Экранный геймпад** с плавающими стиками — консольное управление для тех, кому так привычнее; скины, прозрачность,
  размер, мёртвая зона стиков и редактор раскладки из общего фронтенда портов DOOM / Diablo III / Torchlight II.
  Геймпады (Bluetooth / USB) работают, экранный пад при этом прячется.
- **Картинка на весь экран** телефона в его собственных пропорциях (например, 2000×900 на 20:9), без растягивания;
  или 16:9 с полями. **Лимит кадров 30 / 60 / 90 / 120** переключается в настройках прямо во время игры, без перезапуска.
- **Меню паузы** (шестерёнка): «Продолжить», «Настройки игры…», «Экранный геймпад…», «Изменить раскладку», показать /
  скрыть экранный геймпад, «Выйти из игры» (с подтверждением). Сама игра за этим меню не останавливается.
- Проверено на Xiaomi 14 Ultra (Snapdragon 8 Gen 3 / Adreno 750, Android 16) на системном драйвере: главное меню —
  **120 fps**, в игре — **около 87 fps** при лимите 120.
- **Другие GPU:** игра хранит большую часть текстур в формате BC, который Adreno читает сам, а Mali, PowerVR и Xclipse на
  штатном драйвере — нет. Для них установка может собрать набор текстур ASTC (она спрашивает, для какого телефона игра:
  Adreno — BC, Mali / PowerVR / Xclipse — ASTC, не знаю — оба); приложение при каждом запуске само выбирает подходящий
  набор. ASTC собран и проверен на ПК, на телефоне без BC ещё не запускался — отчёты очень нужны.
- Android 8.0 и новее (minSdk 26), нужен Vulkan, только 64-бит ARM.
- **Ограничения:** только обновление v393216; вибрации пока нет; рамки выделения отряда, как на ПК, нет (в консольной
  сборке её просто нет). Сохранения лежат в памяти приложения (`Android/data/org.pillars.mobile/files/save`): установка
  новой версии APK поверх старой их сохраняет, **удаление приложения — стирает**.
- Подготовка файлов: положите базу, обновление и `prod.keys` в `put-game-files-here/` и дважды щёлкните `setup.bat`
  (Linux / macOS: `./setup.sh`, пока не проверялся); проверка без изменений — `setup.bat check`. Нужно около 90 ГБ
  свободного места, полный проход — 20–40 минут. Готовую папку `work/PillarsOfEternityMobile` (16–28 ГБ) скопируйте в
  `Download` телефона.
- Сборка: Unity 2018.1.4f1 с Android Build Support (IL2CPP), NDK r29, JDK 17+, Python 3.12+, затем `build.bat`
  (см. «Building» и [docs/BUILD.md](docs/BUILD.md)).
