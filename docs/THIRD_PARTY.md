# Third-party code and data in Pillars of Eternity Mobile

The repository's own code is MIT (root `LICENSE`). Its Java front end (`app/frontend/java`) is the shared front end of the author's
Switch ports (DOOM Mobile, Diablo III Mobile, Torchlight II Mobile), which descends from d2r-mobile's Android app; that is why the
root `LICENSE` keeps d2r-mobile's copyright line as the sibling repositories do. Everything below keeps its own license.

## In this repository

| What | Where | License / copyright | Source |
|---|---|---|---|
| nx2android (subset: porter, the dump extractor, the `main` rewriter, the shader split and translator, Unity / NSO readers) | `third_party/nx2android` (unchanged copies; what was copied and from which commit: its `README.md`) | MIT, Copyright (c) 2026 seedds (`third_party/nx2android/nxnative/LICENSE`) | https://github.com/shirai91/nx2android |
| Ryujinx shader translator 1.3.2 (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator`), as nx2android vendors it | `third_party/nx2android/nxnative/dm/third_party/ryujinx` | MIT, Ryujinx Team and Contributors (`LICENSE.txt` there) | https://github.com/Ryubing |
| MacNativeGames code adapted in nx2android's translator (`dmsh_xlate/Program.cs`, `VertexCounterLowering`) | `third_party/nx2android/nxnative/dm/gfx/tools/dmsh_xlate` | MIT (`nxnative/dm/third_party/macnativegames/LICENSE`) | named in nx2android's `dm/third_party/README.md` |
| nx2android gamepad themes and default layout (the on-screen pad's skins) | `app/front-assets/themes`, `app/front-assets/overlay_default.tsv` (unchanged) | MIT, Copyright (c) 2026 seedds (`app/front-assets/LICENSE-nx2android-themes.txt`) | https://github.com/shirai91/nx2android |
| Shared front end (gear, pause menu, pad, settings pages) | `app/frontend/java/org/pillars/port` | MIT (this repository); descends from d2r-mobile (MIT, Copyright (c) 2026 jhaizhou) by way of DOOM Mobile | https://github.com/jhaizhou-ops/d2r-mobile, https://github.com/Desm0nt/doom-port |
| SMOL-V opcode table (331 rows) | `tools/overlay/smolv_opdata.py` | public domain (smol-v v0.1; later releases MIT or public domain), Aras Pranckevicius | https://github.com/aras-p/smol-v, `source/smolv.cpp` at `ce9a68221f28592578a948af56f1fee03c3bacc7` |
| Unity 2018.1.4f1 release type trees of 16 engine classes (field names, types, sizes, flags) | `tools/overlay/unity_types_2018.1.4.json` | facts about Unity's serialization format; taken from the type-tree database that AssetRipper embeds (GPL-3.0 tool; the database is generated from Unity editor builds) | https://github.com/AssetRipper/AssetRipper; see "Open questions" below |

The SMOL-V decoder / encoder in `tools/overlay/smolv.py` is our own code (written from the format the Unity 2018.1.4f1 player
decodes); only the opcode table is smol-v's.

## Taken from the developer's own installs at build time (not stored here)

- **Unity 2018.1.4f1 with Android Build Support (IL2CPP)**: `libunity.so`, Unity's `classes.dex` / `classes.jar`, its `styles.xml`
  and default app icon, `unity default resources`, and the il2cpp runtime source (`Editor/Data/il2cpp/libil2cpp`,
  `external/boehmgc`), which `tools/build_native.py` copies to `work/build` and changes in six places (the F2 hooks, listed in
  `IL2CPP_HOOKS` there; the anchors are single lines of Unity's source). Unity's license terms apply to these and to an APK that
  carries them.
- **Android NDK r29, SDK build-tools, a JDK**: compilers and packaging tools.

## Installed by setup on the player's PC (not stored here)

- Python 3.12 (python.org's NuGet package, only when no Python 3.12+ is installed) and the packages of `tools/requirements.txt`
  from PyPI: pycryptodome (BSD / public domain), backports.zstd (BSD-3-Clause), lz4 (BSD-3-Clause), capstone (BSD-3-Clause),
  numpy (BSD-3-Clause), astc-encoder-py (MIT; bundles ARM's astc-encoder, Apache-2.0), texture2ddecoder (MIT), archspec
  (Apache-2.0 / MIT).
- The .NET 10 SDK (Microsoft's dotnet-install script, only when none is installed) and the NuGet packages the extractor and
  translator reference (LibHac 0.19.0 for `nx_extract`, the packages pinned in Ryujinx's `Directory.Packages.props`).

## Open questions (for the repository owner)

- `tools/overlay/unity_types_2018.1.4.json`: the type layouts are Unity's; they were read out of AssetRipper's embedded type-tree
  database. They are needed only to rewrite `globalgamemanagers` (`ggm_patch.py`), whose 16 classes the Switch file stores without
  type trees. Alternatives: regenerate them from a Unity 2018.1.4f1 editor build (an asset bundle with type trees for those
  classes) and cite that instead.
