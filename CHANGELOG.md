# Changelog

All notable changes to Vivify Quest are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

"Converted" maps are PC ("Windows") bundle maps that the mod converts on the
headset. When a release changes how bundles are converted, it bumps the
conversion cache version, and cached conversions are redone automatically.

## [Unreleased]

## [0.14.11] - 2026-09-29

### Fixed
- **Converted maps freezing the game (0.14.10).** The blit companion, the
  single-screen build of a map's shaders, was loaded on the main thread in
  one call. That call loaded every material in it, and with them their
  textures, whose data files the companion renames away. It now loads
  asynchronously, a step per frame, and never blocks.

### Changed
- The blit companion is now opt-in. The new setting **Blit Fix For Screen
  Textures (experimental)** is off by default. With it off, converted maps
  load exactly as they did in 0.14.9 plus the geometry-stage fallback.

## [0.14.10] - 2026-09-29

### Fixed
- **Grey stand-ins and missing models on full Windows-bundle maps.** Shaders
  with a geometry stage (743Aether's Wireframe Note, Triangle Explosion and
  the like) were left untranslated. On the headset that meant a grey stand-in,
  or nothing where only some variants failed.
  - Multiview forbids a geometry stage, so these variants are now linked
    without it. Where the geometry stage did the projection, the
    object-to-clip transform is added to the vertex program.
  - Effects built on the geometry stage (exploding triangles, wireframes) come
    out as the plain mesh with the material's own shading, in its real
    colours.
  - Checked on 743Aether: all 35 of its shaders translate, up from 34, and
    none of its variants are left on DirectX, down from 102. All 173
    distinct programs compile and link under glslang, and every one writes a
    position.
- **Blit effects missing on converted maps.** A blit into a screen texture
  or a temporary texture draws single-screen, where the two-eye programs of
  a converted map's shaders draw nothing.
  - Maps that use blits now also get a single-screen build of their
    shaders, converted in the background and cached next to the main one.
  - Any blit into a single texture uses a copy of its material on that
    build, kept in step with the original every frame. Blits into the eye
    texture keep the two-eye program.

### Changed
- **Broken Quest shaders are always replaced by their PC build.** The
  **PC Shaders For Broken Quest Shaders** setting is gone, and Convert PC
  Bundles On Device no longer stops it; only turning shader translation off
  does. Hold My Hand's merge now uses the unsplit 2021 PC build (see 0.14.9).
- Conversion cache version 14: converted maps reconvert once.

## [0.14.9] - 2026-09-28

### Fixed
- **Converted maps broken by 0.14.8, and Hold My Hand's kaleidoscope drawing
  nothing again.** Both had the same cause: the headset never picks a shader
  variant whose stereo keyword was renamed to `STEREO_MULTIVIEW_ON`. That
  held for all three places the rename was tried:
  - the 2021 keyword table (0.14.0 onward);
  - the 2019 programs (0.14.0–0.14.4);
  - the 2019 pass table (0.14.8).

  A split shader showed the headset only its single-screen program, which
  draws nothing in two-eye rendering. The kaleidoscope worked in 0.14.7
  because it came from the 2019 PC bundle, which wasn't split. In 0.14.8 it
  came from the split 2021 one.
  - Every converted map, 2019 and 2021, now gets the two-eye program in its
    plain variants. That layout has worked on the headset since 0.13.3.
  - Conversion cache version 13: maps (and Hold My Hand's merged bundle)
    reconvert once.

### Added
- **Replay Render Mode (converted maps)** setting (off by default). It
  replaces 0.14.8's Recordable 2019 Converted Maps. Turn it on while
  rendering replays or recordings, and off to play.
  - Converted maps are then built with single-screen programs only, which
    replay and recording renderers can draw. The headset draws none of them.
  - Each mode has its own cache, so switching converts each map at most once
    per mode.

### Removed
- The **Recordable 2019 Converted Maps** setting.

### Known issues
- With no single-screen programs in play mode, offscreen blits and render
  textures on converted maps can flicker again, as before 0.14.0.

## [0.14.8] - 2026-09-27

### Fixed
- **Converted Unity 2019 maps missing 3D assets, or showing grey stand-ins,
  in replay and recording renderers.** Those renderers draw through a
  single-screen camera. Since 0.14.5, a 2019 map's converted shaders only
  had two-eye (multiview) programs, which a single-screen camera can't run.
  - 2019 shaders now get separate single-screen and two-eye programs, as
    2021 ones already did.
  - What makes this work: a 2019 bundle picks its keyword variants through
    each pass's `m_NameIndices`. The stereo keyword `STEREO_INSTANCING_ON` is
    now renamed to `STEREO_MULTIVIEW_ON` there, in place. 0.14.0–0.14.4
    renamed it inside the programs, which Unity doesn't read.
  - Checked on 743Aether's real 2019 bundle:
    - 40 of its 41 keyword entries renamed (the 41st is in the one shader
      that can't be translated);
    - 387 stereo variants split;
    - all 313 distinct programs compile and link under glslang (157
      single-screen, 156 two-eye).

### Added
- **Recordable 2019 Converted Maps** setting (on by default). If converted
  2019 maps lose their notes or visuals in the headset, turn it off: that
  would mean the headset doesn't pick up the renamed keyword.

### Changed
- The cache marker records this setting, so every converted map reconverts
  once, and again whenever the setting changes.

## [0.14.7] - 2026-09-27

### Fixed
- **Hold My Hand's kaleidoscope still not drawn.** Two causes, both seen in
  the 0.14.6 log:
  - **The load-time swap never happened.** It only replaced shaders that
    Unity reported as unsupported. A shader shipped with no programs reports
    itself as supported and draws nothing, so no material was ever changed.
    The swap now goes by the shader's name, and the log names each material
    it changes.
  - **The merge was refused.** The song folder only had the Unity 2019 PC
    bundle, while the Quest bundle is Unity 2021, and a shader can only be
    merged between bundles of the same version. The mod now picks the PC
    build that matches the Quest bundle's Unity version, and downloads it by
    its `Info.dat` checksum when the song folder doesn't have it. A PC bundle
    of the other version still stands in at load if that download fails.

## [0.14.6] - 2026-09-27

### Added
- **Broken Quest shaders are merged into the Quest bundle itself.** Hold My
  Hand's Quest bundle ships its raymarched kaleidoscope with no programs,
  because the mapper's Unity failed to compile it for Android.
  - When a Quest bundle has shaders like that, the mod downloads the map's PC
    bundle if the song folder doesn't have one, and converts it.
  - It then writes a copy of the Quest bundle with the PC build of each
    empty shader put in its place. Every material that uses the shader loads
    with a working one, with no swapping at run time.
  - The copy is cached in `Mods/Vivify/ConvertedBundles/` and loaded on every
    later play. The song folder's own bundle is not changed.
  - The Quest shader keeps its own references (default textures, fallback)
    when the PC build has the same number of them; otherwise they are
    cleared, and the log says so.
  - A shader is only moved between bundles built by the same Unity version.
    If the merge can't be done, the 0.14.5 run-time stand-in is used.
  - If the merged bundle crashes the game, the next selection loads the Quest
    bundle as it shipped.
- `conv --merge` in the host tool, with tests. The merge was also checked on
  Hold My Hand's real Quest bundle: the other 25 shaders read back unchanged.

## [0.14.5] - 2026-09-27

### Fixed
- **Notes and most visuals invisible on converted maps built with Unity 2019.**
  This was a regression in 0.14.0–0.14.4. It affected most maps whose PC
  bundle was downloaded or converted, such as Yoi Okashi to Warui Okashi and
  Through The Screen.
  - What went wrong: the stereo split from 0.14.0 renamed the stereo keyword
    inside each program, but Unity picks a 2019 shader's variants through the
    pass's `m_NameIndices`, which the rename never reached. The eye cameras
    drew the single-view programs, which draw nothing in the headset's
    two-eye (multiview) pass.
  - The fix: 2019 bundles now convert the way they did in 0.13.3. Only 2021
    bundles, whose keyword table is patched where Unity reads it, get
    separate single-view and multiview programs.
  - A new test fails without the fix.
- **Hold My Hand's kaleidoscope: the PC build of its shader is now found.**
  The step added in 0.14.2 asked the PC bundle only for Shader assets. A map's
  shader is almost always pulled in by the material that uses it, so it found
  nothing ("0 found in the PC bundle"). It now collects shaders from every
  material and prefab in the bundle. If a shader is still missing, the log
  lists the ones the bundle does contain.

### Changed
- Conversion cache version 12, so maps converted by 0.14.0–0.14.4 reconvert
  by themselves.
- The README is shorter. Its per-version history moved to this file, and the
  credits are fuller.

## [0.14.4] - 2026-09-27

### Added
- Vivify maps with no Quest assets now download their PC bundle, using the
  `windows2021`/`windows2019` checksum in `Info.dat`, and convert it. The same
  fallback runs when a Quest bundle download fails or times out.

### Changed
- Every bundle download is logged, including the resolved URL, the bytes
  written, and the HTTP/curl status on failure.
- The PC-shader download for broken Quest shaders gives up after three
  minutes instead of holding the play button.

## [0.14.3] - 2026-09-27

### Fixed
- Custom notes grey on Burning Sands. Small named cbuffers, such as the
  instancing `Props` buffer that holds `_Color`, are loose uniforms again.
  Only cbuffers over 2 KB become uniform blocks, each with an explicit
  `layout(binding = N)`.
- The PC-shader step mistook the map's own `bundleAndroid2021.vivify` for its
  PC bundle. It now only takes `bundleWindows2021.vivify` or
  `bundleWindows2019.vivify`, and otherwise downloads one.

### Known issues
- 743Aether's wireframe note shader uses a geometry stage, which multiview
  rendering forbids. Its stand-in does not take the note colour yet.

## [0.14.2] - 2026-09-27

### Added
- A PC-build stand-in for shaders that a Quest bundle ships empty. Hold My
  Hand's raymarched AudioLink kaleidoscope failed to compile for Android when
  the map was built. The mod now converts the map's PC bundle and uses the
  PC build of that shader instead. New setting: **PC Shaders For Broken
  Quest Shaders** (on by default).

### Changed
- Shader names are read from `m_ParsedForm.m_Name`, so logs and scans name
  every shader.

## [0.14.1] - 2026-09-27

### Changed
- Map realtime shadows from `SetRenderingSettings` are now opt-in (new
  setting **Map Realtime Shadows**, off by default). With shadows on, the
  shadow map comes out fully shadowed under multiview, which is the suspected
  cause of RSIH's missing note bodies.

## [0.14.0] - 2026-09-27

### Added
- **AudioLink for Beat Saber 1.40.8 on Quest**, with an **AudioLink** setting
  (on by default). It is a port of Aeroluna's BSAudioLink, following
  RedBrumbler's BSAudioLink-Quest where the Quest differs. It uses the
  AudioLink 3.1.2 analysis shader, run through this mod's own converter.
  - It feeds in the song's audio, timing, media state and theme colours.
  - It publishes `_AudioTexture` globally.
  - It registers the SongCore `AudioLink` capability.

### Fixed
- Flicker and garbage on converted maps. Converted shaders get separate
  single-view and multiview programs (the multiview ones under
  `STEREO_MULTIVIEW_ON`), so blits, render textures and secondary cameras
  draw again. This only happens when the game actually uses
  `STEREO_MULTIVIEW_ON`.

## [0.13.3] - 2026-09-27

### Fixed
- Everything a translated shader drew landed in the wrong place. The per-eye
  camera matrices are now read from Unity's `UnityStereoGlobals` uniform
  block, instead of loose uniforms that Unity never updates.

## [0.13.2] - 2026-09-27

### Changed
- GPU instancing is turned off on materials from converted bundles, so each
  note in a chord or chain is drawn with its own transform.

## [0.13.1] - 2026-09-27

### Fixed
- Per-instance arrays in translated shaders are sized at load time
  (`UNITY_RUNTIME_INSTANCING_ARRAY_SIZE`) instead of Unity's placeholder of 2.

## [0.13.0] - 2026-09-26

### Added
- A crash guard. A converted bundle that took the game down is reconverted
  without shader translation the next time its level is selected.

### Fixed
- Crash on selecting a converted Unity 2019 level. Translated programs now
  keep the 4-byte alignment Unity expects before the data that follows the
  code.
- Dialtone translated 0 of 3 shaders; now 3 of 3. The inline layout of
  parameter blobs (Unity 2021.3.10+) is now read.

## [0.12.0] - 2026-09-26

### Added
- **Scene depth for map shaders** setting (on by default). The camera renders
  `_CameraDepthTexture` for raymarchers and distortion effects.

### Fixed
- The converter translates real PC shaders. Unity strips DXBC reflection from
  built bundles, so reflection is now rebuilt from `m_ParsedForm` and the
  parameter blobs. On 743Aether, 34 of 35 shaders translate, up from 0.
- GPU-instanced variants, 2019 double-wide stereo variants, `float3` arrays
  in constant buffers, and shaders with subroutines now translate.
- The note colour aliases (`_BaseColor`, `_TintColor`, `_MainColor`) are only
  written to stand-in shaders, never to a map's own shader.
- `VRCenterAdjust.Update skipped` was logged every frame.

## [0.11.0] - 2026-09-26

### Fixed
- Converted shaders now actually run on the Quest:
  - variants in `m_ParsedForm` are relabelled as GLES;
  - stages are linked into one GLSL program;
  - programs are emitted for multiview, with each eye's projection taken
    from the single-pass-instanced twin.
- The program store is read with Unity's real layout: one entry table, with
  segments and parameter entries.

## [0.10.0] - 2026-09-24

### Added
- Features merged from other Quest forks:
  - Blit material and order aliases, clearing Blits, and persistent Blits
    (`"duration": -1`);
  - named enum values in `SetRenderingSettings`;
  - the rest of `RenderTextureFormat`.

### Fixed
- Notes drawn pure white.
- Levels coming out white or black:
  - HDR colours were clamped to white;
  - black emission was taken as the colour;
  - rim and outline colours tinted whole meshes;
  - textures that cannot be sampled were used anyway.

## [0.9.14]

### Fixed
- The CPU texture decoder is given readable pixels. Block-compressed textures
  in converted bundles are marked readable, with their pixels kept in the
  companion stream.

## [0.9.13]

### Fixed
- Every converted level went black. The stand-in shading carried the wrong
  colour and texture properties.

## [0.9.1]

### Changed
- Both diagnostic files are `.txt`, in one folder. The session log is
  buffered and capped at 8 MB.

## [0.9.0]

### Added
- A per-level report file, with one block when the level starts and one when
  it ends.
- **Force Reconvert All (ignore cache)** in settings.

## [0.8.9]

### Added
- A watchdog: Vivify stands down instead of hanging the game when per-frame
  work runs long. Level-load phases are timed.

### Fixed
- Fallback-shader search results were not cached.
- Texture decoding was unbounded on the main thread.

## [0.8.8]

### Fixed
- The mod would not load at all.

## [0.5.0]

### Added
- An on-device PC → Quest bundle converter, and **Convert All PC Bundles
  Now**.

### Fixed
- Custom sabers did nothing.
- Notes going invisible partway through a song.
- Arcs and other transparent geometry disappearing.
- Converted bundles rendering nothing: broken shaders now fall back to a
  stand-in, and textures are decoded on the CPU.
- Raymarching and depth-driven effects:
  - the depth texture format;
  - depth-only cameras;
  - `targetTexture` breaking stereo.

