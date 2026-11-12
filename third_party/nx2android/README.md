# third_party/nx2android: the part of nx2android the setup uses

[nx2android](https://github.com/shirai91/nx2android) (MIT, Copyright (c) 2026 seedds, license in `nxnative/LICENSE`) ports Nintendo
Switch games to Android with its own host. Pillars of Eternity Mobile runs the game in the stock Unity player instead, but its
setup reuses three steps of nx2android's `porter` (through `tools/poe_unpack.py`): dump extraction, the rewrite of the game's
`main` (`nxnative/dm/tools/patch_nso.py`) and the shader translation to SPIR-V (`nxnative/dm/gfx/tools/shader_split.py`, the
Ryujinx-based translator `nxnative/dm/gfx/tools/dmsh_xlate` over `nxnative/dm/third_party/ryujinx`). The overlay tools also
import its Unity file reader (`nxnative/dm/gfx/tools/unityfs.py`) and NSO readers (`nxnative/dm/tools/nso2elf.py`, `nso_syms.py`).

## What is here

Files copied unchanged from the `poe` branch of the port author's nx2android clone, commit
`54d661bd647de83e62b863e9c5648e9a17179caf`, which is upstream `main` at `5e4ea3bb` ("fix(shaders): validate cached translator and
export products (#212)") plus the author's Pillars of Eternity commits. Only these paths were copied (the host, the iOS and
Android apps, the docs, the tests and the other games' profiles are left out):

- `porter/` (all), `nxnative/scripts/build_ipa_from_nsp.py`
- `nxnative/dm/tools/` (without `tests/`), `nxnative/dm/android/tools/`, `nxnative/dm/android/overlay_default.tsv`
- `nxnative/dm/gfx/tools/` (without the prebuilt macOS translator `dmsh_xlate_bin/`)
- `nxnative/dm/games/{game.py, profile_guess.py, settings.py, settings.tsv, README.md}`, `nxnative/dm/ios/tools/ios_metal.py`
  (read by porter's step fingerprints)
- `nxnative/dm/third_party/ryujinx/` (Ryujinx 1.3.2 shader translator, MIT, `ryujinx/LICENSE.txt`),
  `nxnative/dm/third_party/README.md`, `nxnative/dm/third_party/macnativegames/LICENSE` (code adapted in `dmsh_xlate/Program.cs`)

Of the author's commits on top of upstream `main`, only one touches a copied file the setup runs: `023a661` "fix(shader): read
Unity 2018.1 compute shaders (Pillars of Eternity skinning)" (`unityfs.py`: the Unity 2018.1 ComputeShader type tree;
`shader_split.py`: a comment). The others change `porter/cli.py`, `porter/datapack.py`, `porter/pipeline.py` (an `--astc-overlay`
option the setup does not use) and the host, which is not copied.

The game's profile is not part of this folder: it is `profile/0100D6200E130000` at the repository root. The setup copies this
folder and the profile to `work/setup/nx2android` before it runs anything, so the .NET builds (`obj/`, `bin/`) never write
into the repository.
