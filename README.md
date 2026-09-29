# Vivify Quest

A Quest port of [Vivify](https://github.com/Aeroluna/Vivify) for **Beat Saber
1.40.8**. It plays Vivify maps on the headset, including maps that were only
ever built for PC.

It is built from several community Quest ports merged into one (see
[Credits](#credits)), and adds three things of its own:
- an on-device PC → Quest converter, with its own DirectX → GLSL shader
  translator;
- AudioLink;
- diagnostics for maps that don't work.

What changed in each release is in [CHANGELOG.md](CHANGELOG.md).

## Features

- FULLY WORKING RAYMARCHING SHADERES IN HOLD MY HAND (only in 0.14.7+)

- **Vivify events on Quest:** custom prefabs, materials, blits, cameras,
  screen textures and rendering settings. Blits run at the right point in the
  frame (before or after the skybox, opaque or transparent geometry), as on
  PC, so effects don't draw over the notes.
- **PC maps playable on Quest.**
  - A map with no Quest bundle has its PC bundle converted on the headset.
    If the song folder has no PC bundle, the mod downloads one.
  - Shaders are translated from DirectX to GLES for the Quest's two-eye
    (multiview) rendering.
  - Anything that can't be translated gets a stand-in shader, so it is still
    drawn.
- **Broken Quest shaders replaced.** A shader that a map's Quest bundle
  shipped empty (the mapper's Unity failed to compile it) is replaced by the
  PC build of that shader. The mod downloads the map's PC bundle if needed,
  and merges the translated shader into a cached copy of the Quest bundle.
- **AudioLink:** `_AudioTexture` for any AudioLink shader in a map, and the
  SongCore `AudioLink` capability.
- **Crash guard:** a converted bundle that crashes the game is automatically
  reconverted without shader translation, so the level stays playable.
- **Diagnostics:** a session log and a per-level report on the headset.

## Install

1. Install the `.qmod` from the
   [releases](https://github.com/trail-b1az3r/vivify-quest/releases) with
   QuestPatcher or ModsBeforeFriday.
2. Dependencies (SongCore, CustomJSONData, Tracks, BSML and the rest) are
   installed by your mod manager.

The newest untested build of `main` is the rolling
[nightly](https://github.com/trail-b1az3r/vivify-quest/releases/tag/nightly)
pre-release. It is rebuilt daily when something has changed.

## Settings

All settings are in **Mod Settings → Vivify**.

| Setting | Default | What it does |
|---|---|---|
| Convert PC Bundles On Device | on | Converts PC-only maps on the headset. |
| Translate Shaders On Conversion | on | Translates PC shaders to GLES. Off: everything uses stand-in shading. |
| Stand-In Shading For Unsupported Shaders | on | Draws shaders that can't run with a simple stand-in instead of nothing. |
| Scene Depth For Map Shaders | on | Renders `_CameraDepthTexture` for raymarchers and distortion effects. |
| AudioLink | on | Runs AudioLink for maps that use it. |
| Map Realtime Shadows | off | Lets maps turn on realtime shadows. Expensive; can blacken note bodies. |
| Replay Render Mode (converted maps) | off | Converts maps with single-screen programs so replay and recording renderers can draw them. Turn on to render, off to play. |
| Blit Fix For Screen Textures (experimental) | off | Builds converted maps' shaders a second time for blits into screen textures, where they otherwise draw nothing. |
| Disable Custom Note Visuals | off | Keeps the game's own notes, sabers and debris. |
| Disable All Blits | off | Turns off post-processing effects. |
| Disable Beat 0 Filmgrain Blit | off | Skips film-grain blits that start at beat 0. |
| Disable CreateCamera Depth | off | Stops map-created cameras from rendering depth. |
| Disable Vivify Visuals In Multiplayer | on | Plays Vivify maps without their visuals in multiplayer. |
| Disable VR Center Adjust Handling | off | Lets room recentering move Vivify's cameras and effects. |
| Submit Scores On Vivify Maps | on | |
| Debug logging | off | More detail in the session log. |

**Convert All PC Bundles Now** converts every PC map up front. **Force
Reconvert All (ignore cache)** redoes every conversion.

## When a map doesn't work

Send these two files from the headset:

```
/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt
/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt
```

For a crash, also send `/sdcard/ModData/com.beatgames.beatsaber/logs2/`.
The map's bundle helps too:
- **Quest maps:** `bundleAndroid2021.vivify` from the song folder.
- **Converted maps:** the converted copy from `Mods/Vivify/ConvertedBundles/`.

### Known limits

- **Geometry-shader and tessellation effects** can't run on the Quest under
  multiview. They are drawn with a stand-in.
- **A map that needs a feature the Quest's GPU lacks** can look different
  from PC.

## Building

You need [QPM.CLI](https://github.com/QuestPackageManager/QPM.CLI), the
Android NDK, CMake and Ninja (see the
[BSMG Quest modding guide](https://bsmg.wiki/quest/quest-modding-intro.html)).

```sh
python3 scripts/restore-deps.py   # dependencies from GitHub (no qpackages.com needed)
qpm s build
qpm s qmod
```

CI (`.github/workflows/build.yml`) does the same on every push. See
[`scripts/README-deps.md`](scripts/README-deps.md) for dependency sources.

### Tests

The converter, shader translator and parsers are plain C++20 with no Unity
dependency, so they build and test on a desktop under ASan/UBSan:

```sh
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include -o /tmp/conv \
    tools/bundleconvert/main.cpp src/VivifyBundleConvert.cpp \
    src/VivifySerializedFile.cpp src/VivifyDxbc.cpp
VIVIFY_CONV=/tmp/conv python3 tools/bundleconvert/run_tests.py
VIVIFY_CONV=/tmp/conv python3 tools/bundleconvert/fuzz.py
```

`tools/shaderscan`, `tools/dxbc`, `tools/texturedecode` and `tools/report`
have their own suites. `.github/workflows/validate.yml` runs them all,
including a glslang compile of every translated program.

## Credits

**Vivify and AudioLink**
- [Aeroluna](https://github.com/Aeroluna) wrote
  [Vivify](https://github.com/Aeroluna/Vivify) for PC Beat Saber, which this
  port follows, and [BSAudioLink](https://github.com/Aeroluna/BSAudioLink),
  whose AudioLink integration and shader bundle this port uses (MIT).
- [llealloo](https://github.com/llealloo) and contributors wrote
  [AudioLink](https://github.com/llealloo/vrc-udon-audio-link) (MIT).
- [RedBrumbler](https://github.com/RedBrumbler) wrote
  [BSAudioLink-Quest](https://github.com/RedBrumbler/BSAudioLink-Quest), the
  earlier Quest port of AudioLink that this one follows where the Quest
  differs.

**Quest ports merged into this one**
- **axo-lotl** ([Gay-Axolotl](https://github.com/Gay-Axolotl)):
  [Vivifhy-Quest](https://github.com/Gay-Axolotl/Vivifhy-Quest), the base of
  this port and its per-`CameraEvent` render ordering.
- **Braxed** ([rbatteries1-design](https://github.com/rbatteries1-design)):
  [Vivify-Quest-Port](https://github.com/rbatteries1-design/Vivify-Quest-Port),
  which contributed:
  - the legacy Blit aliases;
  - the `VRCenterAdjust` hooks;
  - the note-visual and multiplayer settings;
  - settings-menu fixes.
- [Lars27110](https://github.com/Lars27110):
  [Vivify-Quest](https://github.com/Lars27110/Vivify-Quest), with the same
  fixes.
- [webbs7524-wq](https://github.com/webbs7524-wq):
  [Vivify-Quest-2](https://github.com/webbs7524-wq/Vivify-Quest-2), and
  [PATTT160](https://github.com/PATTT160):
  [Vivify-Quest3.0fork](https://github.com/PATTT160/Vivify-Quest3.0fork).
  Their Blit extensions are merged: material aliases, order spellings,
  clearing, and persistent Blits.
- [gamesbeash-art](https://github.com/gamesbeash-art):
  [Vivify-Quest_enabled-Play](https://github.com/gamesbeash-art/Vivify-Quest_enabled-Play),
  which contributed:
  - named enum values in `SetRenderingSettings`;
  - the full `RenderTextureFormat` list.

**Tools and references**
- Unity's [HLSLcc](https://github.com/Unity-Technologies/HLSLcc) was the
  reference for what the DirectX → GLSL translation has to produce.
- The Quest modding community's libraries: SongCore, CustomJSONData,
  BSML, beatsaber-hook, bs-cordl and QPM.
- [Beat Saber Modding Group](https://bsmg.wiki) documentation.

**Testers**
- ME
- people who made issues 
