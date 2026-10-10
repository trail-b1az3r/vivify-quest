# Changelog

All notable changes to Vivify Quest are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

"Converted" maps are PC ("Windows") bundle maps that the mod converts on the
headset. When a release changes how bundles are converted, it bumps the
conversion cache version, and cached conversions are redone automatically.

## [Unreleased]

## [0.14.23] - 2026-10-10

### Added
- **Offscreen Culling** (settings, on by default). Animators in a map's
  prefabs and replacement notes stop updating while none of their renderers
  is on screen, and resume at the right time once one is. An animator with
  nothing to draw (one moving a light or a camera) keeps animating.
- **Far Culling Distance** (settings, 0-1000 m, off by default). Nothing
  farther than this from the player is drawn. It is off by default because
  far scenery is part of many maps' look; try 150-300 m on heavy maps.
- **`vivify_global.txt`**, a log that is never cleared: every session is
  appended, with a timestamp on each line. Past 9 MB it continues in
  `vivify_global-p2.txt`, `-p3` and so on. When all parts together pass
  1 GB, the older half of them is deleted.
- The per-level settings line in the session log now includes Effect
  Resolution, scene depth, Prepare Shaders, geometry effects and both
  culling settings.

### Notes
- A 0.14.22 log of 743 Aether runs smoothly until the map starts its
  `IntroBokeh` full-screen blur at about 2 seconds; the stalls follow it.
  That blur is GPU work that culling does not remove. **Effect Resolution
  50%** is the setting that reduces it.

## [0.14.22] - 2026-10-07

### Fixed
- **Converted maps freezing on start.** The shader translator stored
  integers (loop counters, true/false flags, indices) in float variables as
  bit patterns. As floats, the integers 1, 2, 3 are denormals, and GLSL ES
  lets the GPU flush those to zero, which Adreno does. A loop counter
  therefore never got past zero, the shader looped forever, and the game
  hung on the first frame that drew it. A 0.14.21 log of 743 Aether shows
  exactly that: the level loads, the first notes spawn, then nothing more.
  - Temp registers are now integer variables that hold the raw bits. Float
    instructions reinterpret them; integer ones use them as they are. No
    integer passes through a float anywhere: in the 194 programs of a
    converted 743 Aether, every one of its 304 loop counters now counts as an
    integer, and all 194 compile and link under glslang.
  - Conversion cache version 18: converted maps reconvert once, and a map
    the crash guard had marked gets its translated shaders back.
- **Wrong integer constants in converted shaders.** DXBC's `mov` has no
  type, so integer literals were printed as floats. -1 (DXBC's "true") is a
  NaN as a float and was written `0.0`, and 1 became `1.4e-45`. Conditions
  came out false, and loops started at the wrong value (743 Aether's voronoi
  loops ran 0..1 instead of -1..1). Typeless copies (`mov`, `movc`, `swapc`)
  now move bits.
- The stereo eye index and other integer built-ins (`SV_InstanceID`,
  `SV_RenderTargetArrayIndex`, ...) are integer variables too, so eye 1 can
  no longer be flushed to eye 0.

## [0.14.21] - 2026-10-07

### Changed
- **Less lag: scene depth only for maps that read it.** Every Vivify map had
  the main camera render a depth texture, a second pass over the whole
  scene every frame. Vivify now checks the map's shaders in the background
  when it loads the bundle, and skips that pass when none of them samples
  `_CameraDepthTexture`.
  - Of the bundles checked, Hold My Hand (its Quest build and its converted
    PC build), Dialtone, HALO CE, geekd and Yoi Okashi read no depth. 743
    Aether (3 shaders) and Burning Sands (2) do, and keep it.
  - A map that asks for depth itself (`SetCameraProperty`) still gets it.
    Until the check has finished, depth stays on, as before.
  - In a 0.14.20 logcat of 42 Flux, the GPU ran at 96-97% at its top clock
    and the game dropped to 53-80 FPS once the map's effects started.

### Added
- **Frame stalls in the session log.** A gameplay frame that takes half a
  second or more is now logged with the song time and the bundle, up to 10
  per song. A 0.14.7 log of a converted 743 Aether showed the game not
  drawing for about 86 seconds at song start, with nothing in the log to
  mark it. The next log of a freeze will show how long it lasts and where.

## [0.14.20] - 2026-10-01

### Added
- **Effect Resolution % (lower = less lag)** setting, 25-100%, default 100.
  It renders a map's post-process effects (blits on the screen and the
  screen textures maps create) at that share of the eye resolution and
  scales the result up. Full-screen raymarchers and distortion effects cost
  per pixel, so 50% does a quarter of the work. At 100% nothing changes.
  Cameras a map creates still render at full resolution.

## [0.14.19] - 2026-09-30

### Added
- **Redownload All Vivify Maps** (settings, press twice). It lists every
  installed Vivify map in
  `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyMaps.txt`
  (BeatSaver key, song hash, folder, song name). It then deletes the
  converted bundle cache and replaces each map's folder with a fresh
  download from BeatSaver: the exact installed version when BeatSaver still
  has it, otherwise the map's current version. Use it when maps stay broken
  even on an older release: it returns every Vivify map to what its mapper
  uploaded.
  - A map's old folder is deleted only after its new copy has downloaded and
    unpacked, so a failed download never loses a map. The list file records
    the result for each map.
  - Songs refresh when it finishes.

### Fixed
- **Broken bundles left in song folders.** A PC bundle Vivify downloaded
  into a song folder was written straight to its final name. A download cut
  short by closing the game during a freeze, or an error page served as a
  success, stayed behind as the map's bundle, and every version, older ones
  included, kept loading it. Downloads now go through a `.part` file and are
  kept only if they are an asset bundle.

## [0.14.18] - 2026-09-30

### Fixed
- **Converted maps freezing.** A PC bundle carries shader variants a Quest
  can never use: GPU-instanced copies (Vivify turns instancing off on
  converted materials) and, in Unity 2019 maps, an extra stereo copy. The
  converter translated all of them, leaving that much more for the game to
  load and the GPU to compile.
  - In 743Aether, 573 of 774 shader program references were unusable.
  - Those are now left out of the conversion: 743Aether goes from 336
    linked variants to 90, and from 156 distinct programs to 86. Every
    shader keeps its usable variants, and all 86 programs compile under
    glslang.
  - Unity 2021 maps with the stereo split (Dialtone) are unchanged.
  - Conversion cache version 17: converted maps reconvert once, smaller.

### Changed
- **Prepare Shaders Before Playing** is now off for everyone. Its saved
  setting was reset (new config key), because it may itself have been what
  froze converted maps in the menu.

## [0.14.17] - 2026-09-30

### Added
- **Draw Geometry-Shader Effects (experimental)** setting, off by default.
  - Shaders with a geometry stage, such as wireframe notes, exploding
    triangles and similar effects in 743Aether, Through The Screen and other
    PC-only maps, are drawn without that stage instead of as grey stand-ins.
    The object-to-clip transform is added where the geometry stage did the
    projection.
  - This is the 0.14.10 fallback, off since 0.14.13. It's a setting now so it
    can be tried per player.
  - Turning it on or off reconverts affected maps when next selected, and
    first song starts may take longer while the extra shaders compile.
  - Aimed at issues #70, #60 and #64. Not verified on a headset.

### Notes on open issues
- #67 and #69 (blits turning a map one solid colour, or white until paused):
  try **Blit Fix For Screen Textures (experimental)**.
- #64 (743Aether freezing): 0.14.15 compiles shaders in the menu, and a
  single freeze no longer turns a map grey. **Reset Vivify (clean slate)**
  (0.14.16) clears maps that are already stuck grey.

## [0.14.16] - 2026-09-30

### Added
- **Reset Vivify (clean slate)** button in Mod Settings → Vivify. Press it
  twice within 5 seconds. It does, in one go, what otherwise needs a file
  manager:
  - deletes the whole `Mods/Vivify/ConvertedBundles` folder: every converted
    map, merged and donor bundle, and crash-guard note, so maps the guard
    turned grey are translated again;
  - deletes the PC bundles Vivify downloaded into **Quest** maps' song
    folders (such as Hold My Hand's `bundleWindows2021.vivify`). Vivify
    downloads them again when needed. A PC-only map's own bundle is never
    touched.
  - resets every Vivify setting to its default.

  It unloads whatever Vivify has loaded first, and it won't run while
  Convert All is going. Each map converts again the next time you select
  it. Reopen the menu to see the reset toggles.

## [0.14.15] - 2026-09-30

### Added
- **Prepare Shaders Before Playing** (setting, on by default). A 0.14.7 log
  shows the game stalled for about 88 seconds at the start of a converted
  map's song, while the headset's GPU driver compiled its translated shaders
  for the first time.
  - That compiling now happens in the menu instead, right after you select a
    converted map. Each of its materials is used once, two per frame, for
    both the single-screen and the two-eye variant.
  - The play button shows "Preparing shaders N/M..." until it's done.
  - The log says how long it took.

### Changed
- **Crash guard needs two failed loads in a row.** The first time a
  converted map's load never finishes, the translated shaders get one more
  try. Only a second failure in a row reconverts the map without
  translation (grey stand-ins). Before, one crash, or closing the game during
  a long first compile, turned a map grey until Force Reconvert. A load that
  finishes clears the first strike.

## [0.14.14] - 2026-09-30

### Fixed
- **The merge of PC shaders into Quest bundles is back, fixed.** It fills
  shaders a Quest bundle shipped empty, such as Hold My Hand's kaleidoscope.
  - Why the 0.14.8 merge drew nothing: it merged the PC bundle's normal
    conversion, which splits 2021 shaders. The merged shader's plain
    variants were then single-screen programs, and the headset's two-eye
    pass draws nothing with those.
  - The load-time stand-in that worked in 0.14.7 had used an unsplit build.
    Merges and the stand-in now both use their own unsplit conversion of the
    PC bundle, cached as `…_donor.vivify`.
  - The converter now refuses a split build as a merge donor, and a new test
    checks every merged vertex variant is a two-eye program.
  - A merge still needs the PC and Quest bundles to come from the same Unity
    version. Otherwise the shader is stood in at load, as in 0.14.7, and the
    log says which one happened.

## [0.14.13] - 2026-09-30

### Changed
- **Converted maps are built the way 0.14.7 built them.** You found that
  version looked best, and 0.14.8–0.14.12 looked worse and froze longer at
  song start.
  - The stereo split is back for Unity 2021 bundles, when the game uses
    `STEREO_MULTIVIEW_ON`. My 0.14.9 claim that it never worked was wrong.
    Unity 2019 bundles are still not split.
  - The geometry-stage fallback (0.14.10) is off. Its extra programs are
    compiled when a song starts. The converter keeps it as an option,
    unused by the mod.
  - The shader depth mapping (0.14.12) is reverted.
  - Hold My Hand: the kaleidoscope is swapped in at load, as in 0.14.7. The
    merge into the Quest bundle is switched off; it drew nothing in 0.14.8.
  - Conversion cache version 16. Converted maps reconvert once, when you
    next select them.
- Kept from 0.14.8–0.14.12, since none of them change how a map is drawn:
  - Replay Render Mode (off by default);
  - the blit fix (off by default);
  - the background-pass throttling;
  - one conversion per file at a time.

## [0.14.12] - 2026-09-29

### Fixed
- **Every map stuttering after an update or a Force Reconvert All.** The
  background pass reconverted every converted map one after another, at full
  speed, even while a song played. Your 0.14.11 log shows it redoing all 58.
  - It now runs at low priority and waits between maps while a song is
    playing.
- **Two conversions of the same map at once.** If you selected a map while
  the background pass was converting it, both wrote the same file, which
  could leave a corrupt bundle. The second conversion now waits for the
  first and uses its result.
- **Raymarchers that write their own depth drew over everything on
  converted maps.** Their depth was written in OpenGL's −1..1 clip range
  where a 0..1 depth is expected, so anything nearer than halfway sat in
  front of the whole scene. It is now mapped. (Hold My Hand's kaleidoscope
  doesn't write depth, so this does not change it.)

### Changed
- Conversion cache version 15. Converted maps reconvert once, in the
  background and throttled.

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

