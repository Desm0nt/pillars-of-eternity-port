# Game-derived information in this repository

Pillars of Eternity Mobile contains **no game code and no game data**: no executables or parts of them, no RomFS files (the
Unity `.assets` / `.resS` / `.resource` files, asset bundles, `globalgamemanagers`, `global-metadata.dat`, `Managed/`), no shaders,
textures, audio, video, text or fonts, and no keys. The player supplies the game (`put-game-files-here/README.txt`), and every
game-derived output is produced on the player's PC into the git-ignored `work/` folder by `setup.bat` / `setup.sh`:

- the extracted ExeFS / RomFS, the rewritten `main` (`main.patched.bin`, `main.sites.tsv`, `main.fixups.tsv`) and the shaders
  translated to SPIR-V (`shaders.dmsv`): `work/setup/dump/0100D6200E130000_build/`
- the game image the app maps, `game/libf2game.so` (the game's own compiled code, decompressed and rewritten)
- the overlay (`overlay/Data`: nine shader-carrying game files with their shaders rewritten for Vulkan, and `globalgamemanagers`
  set to Vulkan / landscape) and the ASTC texture set (`overlay-astc/Data`)
- caches read out of the player's files: Unity's common-string table (out of `main`) and the Shader / texture type trees (out of the
  game's own asset bundles)

The APK carries no game file either: the game image and the game's il2cpp metadata are read from the setup folder on the phone.

To make the original code run on Android, the repository does contain *facts about* the game, found by analysing the author's own
copy: addresses, instruction words, identifiers and format descriptions. This page lists them so their extent is clear. They apply
only to Pillars of Eternity: Complete Edition for Switch, update v393216 (2.60.00), `main` build id
`68976c702865e548ad33367b1ed393db`; the setup checks that build id, and porter checks the build ids of all three modules
(`profile/0100D6200E130000/game.tsv`).

| What | Where | Extent |
|---|---|---|
| **Game image layout**: section offsets of `main` (text end, rodata, data, bss, dynamic, eh_frame_hdr, the il2cpp code and metadata registrations, codegen options) and the patch-area layout of the rewrite | `app/native/loader/f2_gen_gameinfo.h`, `f2_gen_image.h`; `tools/setup_poe.py` (`GEN_END`, `BSS_START`, `RT_CHUNKS`, `MAIN_BUILD_ID`) | ~25 constants; 10 address ranges |
| **Runtime redirects**: where `main` keeps its own copy of il2cpp runtime functions, each sent to the function of the same name in our `libil2cpp.so`; runtime code that must never run (poisoned); the typeinfo slots to fill | `app/native/loader/f2_gen_tables.c` | 397 (offset, runtime function name) pairs, 152 offsets, 124 offsets |
| **NintendoSDK plugin wrappers**: offsets of the game's P/Invoke wrappers into the Switch SDK and their managed names (`nn.hid.ControllerSupport$$Show` ...) with our replacements | `app/native/loader/f2_gen_tables.c` (`f2_nn_entries`), `app/native/nn/f2_nn_table.c` | 192 entries; ~110 offsets |
| **Switch-only internal calls** the game uses (19 `UnityEngine.Switch.*` and one `System.Diagnostics.Process` icall) and three `UnityEngine.Application` getters answered differently | `app/native/nn/f2_switch_icalls.c` | names and signatures |
| **Unity player hooks**: offsets and original instruction words inside the stock Android `libunity.so` 2018.1.4f1 (Development and Release) that the boot layer checks before patching (vendor check, texture formats, screen, touch, MSAA) | `app/native/boot/f2_hooks.c`, `f2_screen.c`, `f2_touch.c`, `f2_msaa.c`, `f2_texdiag.c`, `f2_crash.c`, `f2_vkwrap.c`, `f2_gates.c` | offsets into Unity's library, each word checked before it is replaced |
| **Rewriter patches of `main`** (porter's `patches.py`): 8 instruction rewrites (the NVN deferred sampler slot, touch-as-mouse, the 16:9 aspect tolerance) and 12 observation gates, each as offset + original word (+ new word) | `profile/0100D6200E130000/patches.py` | 20 sites |
| **Game profile for porter**: memory layout keys, module build ids, the shader groups | `profile/0100D6200E130000/game.tsv`, `android.tsv` | a few dozen keys |
| **Shader tables made by nx2android's shader split** (no shader code): the key of every shader variant and the game file / object it came from; compute-shader thread-group facts; vertex / pixel pairs | `profile/0100D6200E130000/shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `shader_map.tsv.conflicts.tsv`, `pso_manifest.tsv`, `bf_want.tsv` | 1,190 keys with object names (138 KB); 12 compute keys |
| **Overlay recipe**: the names of the nine game files that carry Shader objects, the `globalgamemanagers` fields changed (graphics API, shader-define platform, orientation), the texture classes / formats re-encoded, the subsets of files the menu opens, a few game class names in comments (why readable textures stay BC) | `tools/overlay/*.py`, `tools/setup_poe.py` (`OVERLAY_FILES`) | file and field names, enum values |
| **SMOL-V variant**: the old SMOL-V encoding (version 0) the Unity 2018.1.4f1 player decodes, re-implemented from its disassembly | `tools/overlay/smolv.py` | format description; the opcode table is smol-v's own (docs/THIRD_PARTY.md) |

Function and type names come from the symbols and il2cpp metadata the Switch modules carry, or are descriptive names given by the
author. The il2cpp runtime hooks in `tools/build_native.py` quote six single lines of Unity's il2cpp source as anchors.

What the repository does **not** contain, and the tooling never writes into the source tree: `*.nsp / *.nsz / *.xci / *.nca`,
ExeFS (`main`, `sdk`, `subsdk0`, `main.npdm`), `main.patched.bin`, `libf2game.so`, RomFS files, `shaders.dmsv`, overlay files,
the ASTC set, `common_strings.pkl`, saves, console keys, the signing key, APKs, Unity binaries.
