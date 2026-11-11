# Changelog

All notable changes to Vivify Quest are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

"Converted" maps are PC ("Windows") bundle maps that the mod converts on the
headset. When a release changes how bundles are converted, it bumps the
conversion cache version, and cached conversions are redone automatically.

## [Unreleased]

## [0.15.9] - 2026-11-11

From the logs of 0.15.8 build 2 (`logs_4`). Converted maps are reconverted
once. Nothing here is tried on a headset.

### Fixed
- **Map effects and trails that silently went missing (Fall Away's HUD).**
  The mod keeps a table of a map's assets, which Unity does not count as a
  use. The clean-up it runs after loading a map's single-view shaders (since
  0.15.3), and in crash recovery mode, destroyed every asset used by nothing
  in the scene: a material only a screen effect or a saber trail uses. The
  log shows Fall Away's HUD overlay material loaded, then "missing" when its
  effect started. Assets in the table are now kept until the bundle unloads.
- **Saber lights from another mod, on converted maps.** The logs show two
  point lights of intensity 1000 with soft shadows in every level, one per
  saber. The game's own shaders ignore them; a converted PC map's shaders
  are lit by them, their shadows flicker on every surface, and each one costs
  six extra passes over the scene every frame. They are now switched off
  while a Vivify map plays. New setting **Saber Lights On Map Objects** (off)
  keeps them.
- **Map sabers and trails lost after pausing.** 0.15.6's check that the
  map's saber is on screen ran while the pause menu had the sabers switched
  off, decided the map's saber could not be shown, and gave your own saber
  and trails back for the rest of the level. It no longer judges a saber the
  game has switched off.
- **Invisible map trails.** The map's trail took its colour from the game's
  own trail, which a custom saber mod makes transparent to hide it. It now
  takes the saber colour from the game's colour manager. The map trail's own
  renderer is also never hidden along with other mods' saber parts.
- **Grass and other geometry-shader effects in one eye.** 0.15.8 build 2's
  declaration changed nothing on the headset: its driver cannot run a
  geometry stage for both eyes. New setting **Geometry Effects In Both Eyes**
  (advanced, on): those effects are drawn without the geometry stage, flat
  but in both eyes. Off gives the full effect in the left eye only.
- **Heartbeat's black tunnel (an attempt).** Its Quest shader declares its
  step count as an `int`; the material holds it as a float, which OpenGL ES
  does not load into an int uniform, so it would raymarch zero steps. Such
  values are now also set as integers. The log names each one.
- **Auto Effect Resolution** only steps while a screen effect is on. In
  Heartbeat it dropped to 40% with no effect running.
- **Settings list** moved slightly right; it sat off the left edge.

## [0.15.8 build 2] - 2026-11-11

Same version, rebuilt. The menu and logs say "0.15.8 build 2". Converted maps
are reconverted once (conversion version 23). Nothing here is tried on a
headset.

### Fixed
- **Grass and other geometry-shader effects in one eye (Yoi Okashi to Warui
  Okashi).** A two-eye program with a geometry stage is only valid if the
  geometry shader declares the view count too
  (`EXT_multiview_tessellation_geometry_shader`). Ours did not, so it drew in
  the left eye or not at all. It declares it now, where the headset's driver
  has the extension; where it does not, nothing changes.
- **743 Aether's clouds and fog in one eye.** They are mesh particle systems
  with GPU instancing on, which Unity draws through shader variants the
  converter leaves out. Particle systems of a converted map now draw without
  GPU instancing, through the variant that is converted.
- **Map saber trails overridden by Custom Models' trail.** Custom Models puts
  its trails under its own saber model, beside the game's, and a trail turns
  its renderer back on a few frames after it starts. When a map replaces the
  trail, other trails under the whole saber are now hidden, and kept hidden.
- **Water reflections (Get Your Wish).** A reflection probe draws one view
  at a time, where a converted map's two-eye shaders draw nothing, so the
  water reflected only the notes. A map with realtime probes is now prepared
  like one with screen cameras, and its single-view shaders are used while a
  probe renders. As with those cameras, this takes effect from the second
  time the map is selected.
- **Fall Away's HUD.** 0.15.5 showed the map's head-following text directly;
  that text is only meant to be filmed by the map's own camera, and shown
  directly it sat in the wrong place and hid note bodies behind it. That is
  undone. The filmed HUD is laid over the screen by an effect whose two-eye
  program leaves the HUD texture out; such an effect now runs its one-eye
  program once per eye, so the overlay shows in the headset.

### Performance
- **Reflection probes refresh about once a second**, not continuously. One
  face a frame was still an extra pass over the scene every frame for each
  probe, since 0.15.3. "Full Reflection Probes" keeps the map's own rate.

### Not fixed
- **743 Aether lagging when you look down at the platform.** The platform's
  gemstone shader is heavy per pixel and fills the view from there; nothing
  in this build changes that.
- **Heartbeat's second tunnel is black.** It is drawn by the map's own
  Quest-built shader (`PoofShaders/AudioPortal/World_PC`), which Vivify does
  not convert or alter; why it comes out black on the headset is not known.
  Turning on This Map > Use Windows Bundle for it makes the mod use its own
  conversion of the PC shader instead, which may behave differently.
- **Luminescent's water** was not looked at separately; if it uses a
  realtime reflection probe, the probe fix above applies.

## [0.15.8] - 2026-11-11

### Fixed
- **Screen effects now get the right screen size (`_ScreenParams`).** A
  likely cause of Breezer going one colour when its backrooms start: its VHS
  effect snaps its sampling to a grid of screen pixels, using a built-in
  value that only a camera's own setup writes. When the effects run, that
  value is whatever the last camera to render left behind; if that was a
  camera drawing into a small texture, the whole view samples a few points,
  or one. The mod now sets it to the size of the picture the effects work
  on. The "Vivify Blit check" log line records what the value was before.
  - Not confirmed on a headset. Why's This Dealer's effect does not use this
    value, so its yellow screen has a different cause, still unknown; the
    Flat-Screen Effect Guard (0.15.7) and its log line are for that.

### Notes
- **Breezer's bombs that should look like notes.** The map builds each one
  from a bomb and two fake notes on tracks: one shows only its body, the
  other only its arrow (its body dissolved, turned 180 degrees), and the
  bomb itself is dissolved. Seeing two whole notes stacked means the
  per-track `dissolve` / `dissolveArrow` are not being applied to notes that
  sit on two tracks at once. That is done by Noodle Extensions and Tracks,
  not by Vivify, and nothing in Vivify touches it. No change here.

## [0.15.7] - 2026-11-11

### Added
- **Flat-Screen Effect Guard** (Performance section, on by default).
  Reported: a map's screen effect turns the whole view one colour, and the
  map is only playable with Disable All Blits on (Why's This Dealer goes all
  yellow).
  - While a screen effect has been on for a second or more, the mod samples
    the picture going into the effects and the picture coming out, at 1 s,
    2 s and 3 s.
  - If all three show a normal picture going in and one flat colour coming
    out, that effect is switched off for the rest of the level, so the map
    can be seen and played. A flat black or white is left alone: that is a
    fade.
  - The first sample of each effect is written to the log ("Vivify Blit
    check"), with the colours in and out and the screen texture's size.
- This treats the symptom. In Why's This Dealer the effect itself is
  translated correctly: it paints everything brighter than a very low
  threshold flat yellow, so the cause is in the picture it is given on the
  Quest, which is what the log line measures. Not tried on a headset.

## [0.15.6] - 2026-11-10

### Fixed
- **Use Map Sabers made sabers invisible.** 0.15.4 hid everything else
  under the saber whenever a map's saber was applied. With a custom saber
  mod the map's model was not being drawn, so nothing was left. Now the
  mod checks that the map's model is really on screen before hiding
  anything:
  - renderers of the map's model that another saber mod switched off are
    switched back on;
  - if the game's model object is switched off or scaled to nothing, the
    map's model moves onto the saber itself;
  - if the map's model still cannot be shown, your own saber stays visible.
  The log says which of these happened ("Vivify saber: ..."). Not tried on
  a headset.

## [0.15.5] - 2026-11-10

### Fixed
- **Fall Away: the on-screen text (HUD) was missing.** The map's headset HUD
  is text that follows your head, on layer 17. The PC headset camera draws
  that layer; the Quest game camera leaves it out, so the text was never
  drawn. The main camera now draws the headset-only layers a map's prefabs
  use (17, and the layers PC mods use for headset-only objects: 7, 21, 24,
  26), for that level only.
  - The map also films a second copy of the text for an overlay effect
    (`Aeroluna/OverlayUITex`). In a headset that effect passes the picture
    through unchanged, on PC as well; it is for the desktop view. Read from
    the map's PC and Quest shaders.
- **RSIH: the "TV switching off" effect at the end.** Its shader
  (`Custom/RetroTVShutdown`) has no two-eye program in the map's Quest
  bundle, and a one-eye program draws nothing into the two-eye screen
  texture. A screen effect whose Quest shader is one-eye only now runs once
  per eye. The bundle scan at selection finds these shaders.
- Neither is tried on a headset. The log names the layers added and the
  effects run per eye.

## [0.15.4] - 2026-11-10

Converted maps are reconverted once (conversion version 22).

### Fixed
- **Shaders with a geometry stage gave both eyes the left eye's view.**
  Reported as a shader's base layer showing in one eye and the lines drawn
  over it in the other. In a single-pass stereo shader with a geometry stage
  (wireframes, grass, extruded lines), the vertex stage reads the eye from the
  instance ID and the geometry stage routes it. 0.15.1 taught the shader
  translator to rebuild that instance ID per eye, but the converter never
  told the translator which programs needed it, so the fix did not run in any
  converted bundle. It does now. A new converter test covers it.
  - Not tried on a headset. If a map still shows one layer per eye after it
    reconverts, the map's name and a log with Debug Logging on will pin it.

### Added
- **Hover hints in the settings menu.** Point at a toggle, a number setting
  or a button and a sentence or two says what it does.
- **Use Map Sabers** (Gameplay, on by default).
  - On: sabers a map brings replace yours, including a custom saber mod's
    model, which is now hidden wherever it sits under the saber and kept
    hidden if the mod builds it late. The map's model is attached to the
    saber itself, so a trick mod that throws or spins the saber moves the
    map's model with it.
  - Off: you keep your own sabers and trails on every map.
  - Sabers that another saber mod builds without the game's own setup are
    now found when a map assigns a saber. Before, the map's sabers were
    ignored on them.
  - Read from the sources of the Quest trick mod (it moves the saber's own
    transform). Not tried on a headset.

## [0.15.3 build 2] - 2026-11-05

Same version, rebuilt. The menu and logs say "0.15.3 build 2".

### Fixed
- **Settings texts that change showed nothing**: the PC Maps conversion
  progress, the This Map row and the logs line. Since 0.15.1 these texts had
  word wrap and an ellipsis on a box of zero height. BSML's settings list does
  not size its children's heights, so TMP cut every line away. The box now
  gets its height, and text that does not fit is drawn instead of hidden.
- **Conversion progress bar**: Convert All PC Bundles shows a bar, a
  percentage, the count and the map being worked on. The last status is kept,
  so it shows again when you reopen the menu.
- **Use Windows Bundle looked like it did nothing**: with no Vivify map
  selected it could not do anything, and the hint saying so was invisible
  (above). The row now says plainly that no Vivify map is selected (pick one,
  with a difficulty that needs Vivify). For a selected map it shows the map
  and which bundle it uses, and the button reads on, off, or "-".

### Removed
- **The test map**: Settings > Test Vivify, `testmap/`, the Build Test Map
  workflow, and the QMOD's test map file (and the `fileCopies` packing added
  for it). The 0.15.2 entry below describes what was removed.

## [0.15.3] - 2026-11-05

Fixes from the newer reports on
[#102](https://github.com/trail-b1az3r/vivify-quest/issues/102).

### Fixed
- **Map objects that never appeared (Get Your Wish's lyrics and more).**
  Offscreen Culling froze an animator while everything it moved was out of
  view. Maps often animate things *into* view, from scale 0 or from a
  renderer that starts disabled; such an object was never seen, so it never
  moved. The tester found the lyrics and other elements only with culling off.
  Offscreen Culling is now off by default, under a new setting key so
  everyone's saved "on" resets once, and labelled "(can hide objects)".
- **Get Your Wish's water reflected only the notes.** Its water reflects the
  map through a realtime reflection probe, and 0.15.1 had turned map probes
  off to stop the start-up crash. Probes are allowed again by default, in a
  reduced form: at most 128 px, no HDR, one cube face per frame instead of all
  six (about 1/70 of the cost of that map's 512 px HDR probe). **Full
  Reflection Probes** turns the reduction off. Crash recovery mode still turns
  probes off for a map that went down.
- **Through The Screen: no sabers on the TV, a flat white floor, a black
  finale screen.** All three are drawn by the map's own cameras, which render
  into textures one view at a time. A converted map's shaders, on 2019-built
  bundles like this one, only hold two-eye (multiview) programs, which draw
  nothing into a single texture. The floor's lighting comes from such a camera
  (`LightMeshCam`). Now:
  - such a camera draws the map with single-view twins of its shaders, taken
    from the map's single-view companion build and swapped in only while that
    camera renders;
  - the first time a map's prefabs bring texture cameras, Vivify leaves a
    marker beside the converted map; from the next selection on, the
    companion is prepared with the prefabs' shaders included, in the menu, and
    the prefabs are let go straight after;
  - new advanced setting: **Single-View Map Cameras** (on by default). Not in
    Lite.

- **Auto Effect Resolution** (Performance, on by default). Final Slaughter
  ([#100](https://github.com/trail-b1az3r/vivify-quest/issues/100)) runs a
  three-pass full-screen effect at full resolution on top of a depth pass for
  most of the song. The Quest falls behind, its frame synthesis fills the
  gaps, and notes look like they stop and slide back and forth. When frames
  stay well above the best this level has managed, map effects step down 10%
  at a time (to 40% at least), and back up when there is room again.
- Fixed the 0.15.3 build failing to link (reflection probe enum constants).

### Not fixed yet
- Get Your Wish's mist blocking the doorway at 0:55, and a map ("scaredTHIN"?)
  that blinds until paused: these need the map name or a log.

## [0.15.2] - 2026-11-04

### Added
- **Crash recovery mode.** While a Vivify map plays, a small marker file
  records the map, a heartbeat every 5 s, and each screen effect (Blit) as it
  starts. On the next launch, a map counts as having gone down if:
  - the game crashed or was closed while the map played, or
  - you left the map from the pause menu within a minute of an effect
    starting, and the game was restarted within 90 seconds of that. This is
    what happened with 743 Aether's intro bokeh in 0.15.1.

  Such a map goes on a recovery list (`RecoveryMaps.txt` beside the logs).
  The next time it is played, it runs in recovery mode:
  - unused assets are unloaded and memory collected before it starts;
  - textures load at half resolution (a quarter of the memory), put back
    when the map ends;
  - map effects render at half resolution or less;
  - the effect that was starting when it went down is skipped, and the log
    says so.

  A Quest app cannot be given more memory than the system allows; this is how
  a map gets more room instead. New advanced settings: **Crash Recovery Mode**
  (on by default) and **Forget Crash Recovery** (clears the list). Not in
  Lite. Host tests cover the cases (`tools/recovery`, run in CI).

- **Test Vivify: a test map made for this port** (`testmap/`; removed in
  0.15.3 build 2). It is a PC
  Vivify map to play and to check a dev build against, set to Mud Flow's *The
  Sense of Me / Chemicals*. You stand in a car driving through rain at night;
  at the switch to *Chemicals* the drive gives way to a shore at dusk, with an
  original character sitting by the water. It covers:
  - prefabs and their removal, and render settings (fog, ambient, skybox);
  - a particle system, plus material and global property animation;
  - a geometry-shader glow along the edges of the view;
  - custom notes, debris and sabers with a trail;
  - two screen effects (Blit).

  Every shader is written for single-pass instanced stereo, as PC maps are.
  The Unity project builds every mesh, material and prefab from code, and a
  new workflow (`Build Test Map`) exports the bundle with Unity 2021.3 once
  the Unity license secrets are set (`testmap/README.md`). `build.yml` packs
  the latest built map into the full QMOD. **Settings > Diagnostics > Test
  Vivify** installs it as a custom level. The song is a commercial track and
  never ships: the map plays your own .ogg copy
  (`Mods/Vivify/TestMapSong.ogg`).
- QMODs can now carry `fileCopies` (`scripts/createqmod.ps1`; removed in
  0.15.3 build 2).

### Notes on the 0.15.1 logs
- Burning Sands and Get Your Wish no longer crash at the start: both played
  to the end with realtime reflection probes off.
- 743 Aether froze as its intro bokeh effect started (2 s in), and Why's This
  Dealer froze as its post-process effect started (28.7 s in). Both shaders
  convert, compile and link cleanly, and other maps' effects ran in the same
  sessions, so the cause is not known yet. Recovery mode skips those effects
  the next time after a freeze.

## [0.15.1] - 2026-11-04

### Fixed
- **Grass and other geometry-shader effects drew in one eye only.** In a
  single-pass stereo shader with a geometry stage (grass, fur, screen
  particles), the vertex stage reads the eye from the instance ID and passes
  it on, and the geometry stage writes the layer. The translator only rebuilt
  the instance ID for vertex stages that write the layer themselves. So both
  views got eye 0, and the geometry stage's `gl_Layer = 0` sent every
  primitive to the left eye. That is the one-eye grass, and objects that sit
  doubled and offset between the eyes. Now the instance ID carries the view
  in these vertex stages too, and the geometry stage's layer write goes to a
  dummy under multiview.
- **Geometry shaders that read the eye back now translate.** They were
  refused before, which left the shader on DirectX: nothing drew. This covers
  [Through The Screen](https://beatsaver.com/maps/43a5d)'s ParticleScreen and
  Visualizer, and 743 Aether's effects. Every program of Through The Screen,
  Get Your Wish, Lifelike and YOU now compiles and links (vertex, geometry and
  fragment stages together) under glslang.
- **Through The Screen was "a tiny square".** Its prefabs bring five cameras
  that render into small textures for its in-world screens (256×224, 128×128,
  32×32 and others). The map leaves them set to both eyes. On the Quest's XR
  rendering, a camera with an eye set is an eye camera. These drew into the
  headset view instead of their textures, at the texture's size, so the map
  showed as a small square in the corner and its screens stayed empty
  ([#102](https://github.com/trail-b1az3r/vivify-quest/issues/102)). A camera
  in a map prefab that renders into a texture is now set to no eye, so it
  draws into its texture as it does on PC. It is logged as `Vivify prefab
  camera ...`. One limit: this map's PC bundle is a Unity 2019 build, whose
  converted shaders have only two-eye (multiview) programs. Converted objects
  seen through such a camera still draw nothing in its texture; the game's
  own objects do draw.
- **Get Your Wish and Burning Sands crashed at the start of the level.** Both
  turn on realtime reflection probes at time 0 and spawn probes. A realtime
  probe renders the scene six more times into a cubemap. Get Your Wish's
  probe is 512 px HDR and re-renders every frame. Realtime reflection probes
  are now off on Vivify maps by default (new advanced setting **Map
  Reflection Probes**). Probes keep the skybox or a baked cubemap instead,
  which should also remove Get Your Wish's oversized water reflections. This
  is the strongest lead from the logs, not a confirmed cause. See Added for
  what the next log will show if it still crashes.
- **Vivify Lite was the full mod under another name.** `qpm s qmod` runs the
  build again, and that second build configured CMake without `VIVIFY_LITE`,
  so the full library replaced the lite one before packing. The lite setting
  is now cached in the build directory and set on the packing step. CI also
  fails if the lite QMOD's library lacks the lite marker.
- **Settings text overlapping** ([#101](https://github.com/trail-b1az3r/vivify-quest/issues/101)).
  Every text in the Vivify tab now reserves its own height. The status line
  and the This Map row have a fixed height, wrap, and end in an ellipsis
  instead of drawing over the controls below. Setting labels shrink to fit
  beside their control. The empty rows at the bottom now reserve real height,
  so the last settings are no longer cut off by the scroll view.

### Added
- **Level start and level end are logged line by line.** For 20 seconds after
  a Vivify level starts, and after it ends, every log line is flushed to disk
  as it is written. A native crash in those windows (the Get Your Wish and
  Burning Sands crashes, the crash on the way back to the menu) no longer
  loses the last lines.
- **A Beat Saber 1.45.3_30054 build, ready for when its dependencies
  ship.** A new workflow, `Build Quest Mod (Beat Saber 1.45.3)`, builds the
  same source for 1.45.3. `scripts/use-game-target.py` with
  `targets/1.45.3.json` points `qpm.json` and `mod.json` at that game's
  libraries. Vivify needs CustomJSONData and Tracks, and neither has a 1.45.3
  release yet (Tracks, Noodle Extensions and Chroma only have unreleased
  `v1.45.3` branches). Until they do, the workflow names what is missing in
  its summary and stops without building; this is not a failure. Once they
  are released it builds full and Lite QMODs named `...-bs1.45.3.qmod`, and
  any game changes Vivify has to follow show up as compile errors.
  Code specific to 1.45.3 can test `VIVIFY_GAME_1_45_3`.
- The per-level settings line also shows `mapRealtimeShadows` and
  `mapReflectionProbes`.
- **Mirror diagnostics** for reflections that turn with the head instead of
  staying in place ([#81](https://github.com/trail-b1az3r/vivify-quest/issues/81)).
  Each visual check (5, 30 and 90 s) logs the game's active mirrors and what
  each one renders with. It also lists every enabled camera, since each camera
  that sees a mirror renders its own reflection. Lifelike's floor mirror is
  up from 73 s to 92 s, so the 90 s check catches it. Vivify does not touch the
  game's mirrors, and the comment on #81 says the same problem was in 0.14.7.
  This is for finding the cause, not a fix.

### Changed
- **Converted maps are converted again** (conversion version 21) to pick up
  the stereo fixes above.
- Realtime reflection probes are off on Vivify maps unless **Map Reflection
  Probes** is on. That also saves GPU time on every map that uses them.

## [0.15.0] - 2026-10-28

### Fixed
- **Shaders that write only depth now draw on converted maps.** A pixel
  program that writes depth (or coverage) and no colour declared that output
  as a colour output at `layout(location = 4294967295)`. No GLSL ES compiler
  accepts that, so every variant of the shader drew nothing. In
  [Through The Screen](https://beatsaver.com/maps/43a5d) that was all 18
  programs of its `AreaLit/LightMeshClear` shader. Every one of its programs
  and every one of [Yoi Okashi](https://beatsaver.com/maps/43a4b)'s now
  compiles and links under glslang, and a translator test covers the case.
- **Converted maps are converted again** (conversion version 20), so they
  pick up this fix and 0.14.27's geometry pass-through fix, which went out
  without a version bump. Crash-guard strikes start fresh too.

### Added
- **Breadcrumbs through a level's end.** "The game crashes after a Vivify
  level" came with no log. Each step of a level's teardown now writes a
  `Vivify level end:` line and flushes it to disk, as does selecting a level
  again on the way back to the menu. After a crash, the last such line in
  `vivify_global.txt` names the step it happened in. That file keeps earlier
  sessions; `VivifySession.txt` is overwritten when the game starts again.
- **The visual check summarises what is drawing.** At 5 and 30 seconds it
  logs how many renderers are active, how many have no shader this GPU can
  run, the shaders drawing the most of the scene, and how many screen effects
  are running. A mostly-unrunnable scene is listed under "Likely visual
  problems". This is for maps that are just black.

### Known, not fixed
- The crash after a Vivify level is not found yet. Nothing in 0.14.26 to
  0.14.29 that runs at a level's end showed a fault when checked offline. The
  breadcrumbs above are there to find it.
- Through The Screen and Yoi Okashi were black, and it is not known whether
  the fix above is all of it. Every shader of Yoi Okashi already compiled
  before, so its black screen has another cause. Both maps' bundles load
  cleanly and every asset they use is found. One shader in Through The
  Screen, a particle visualizer, still cannot be translated: its geometry
  stage reads the eye index.

## [0.14.29] - 2026-10-28

### Fixed
- **The mod builds again.** 0.14.27's saber-trail fallback for Custom Models
  called a function whose definition was never included, so the qmod
  compiled but failed to link. 0.14.27 and 0.14.28 never produced a build.
- **`vivify_global.txt` and `VivifyReport.txt` are written again, or say why
  not.** Of the three log files only the session log reliably appeared. The
  other two were opened in append mode, and any failure was swallowed without
  a trace. Both now open the way the session log does: created when missing,
  otherwise opened and written at the end. Each file's path, or the exact
  reason it could not be opened, is now among the first lines of the session
  log. A failure opening one file no longer stops the other two from being
  tried, and the report gets a `SESSION STARTED` line every launch.
- **Replay renders of converted maps, including BeatLeader replays played
  through the Replay mod.** Since 0.14.28, Replay Render Mode applied only
  while Replay renders, but Replay decides to render only when Render is
  pressed, after Vivify had already chosen and loaded the map's normal build.
  So a render got the build the headset draws, which the render camera does
  not. With the setting on, the single-view build is now prepared in the
  background when the map is selected. When the render starts, Vivify plays
  it instead, and logs which one it played.
- **Note bodies black on maps that bring shadow-casting lights (RSIH).**
  0.14.1 ignored a map's own shadow settings, but RSIH's prefab also adds two
  directional lights with soft shadows. Those still cast shadows whenever the
  game's quality level allows them, and under multiview the shadow map comes
  out fully dark, so notes showed only their arrows. With "Map Realtime
  Shadows" off (the default), realtime shadows are now off for the whole
  Vivify level and restored after.
  - Read from RSIH's bundle and the 0.14.1 notes; not tried on a headset.
    The new visual check (below) logs a live note's parts, so if bodies are
    still missing the log shows why.

### Added
- **Use Windows Bundle for a map** (This Map, at the top of Vivify's
  settings). The selected map is played from its converted Windows bundle
  even though it ships a Quest one. The choice is kept per song folder, takes
  effect at once, and falls back to the Quest bundle when the map has no
  Windows build. For maps like YOU, whose Quest build ships every shader but
  draws less on the headset.
- **Visual checks and a "Likely visual problems" list.** At 5, 30 and 90
  seconds into each Vivify level, Vivify records what is on screen:
  - how many live notes have a body or prefab that cannot draw;
  - a full dump of one live note's renderers (shader, supported, enabled,
    queue, and its dissolve `_Cutout` and colour);
  - the lights and shadows the scene has, and the ambient and sun settings;
  - whether the main camera draws the notes' layer.
  Each level's VivifyReport block now opens with a plain list of what is most
  likely to look wrong: shaders that are DirectX-only or refused by this GPU,
  materials on stand-in shading or with no usable shader, undecodable
  textures, the visual check's findings, and a pointer to Use Windows Bundle
  when it might help.

### Changed
- **Settings are less cluttered.** Everyday settings (This Map,
  Performance, Gameplay, PC Maps, Diagnostics) come first, and the rest sits
  behind **Show Advanced Settings**. Labels are shorter so they fit beside
  their controls, and the log files are listed on one line.

### Known, not fixed
- Expedition 33 ("[Gamified Vivify] Lumière") without its Chroma look with
  GraphicsTweaks installed. That map uses only Chroma for its environment
  and note colours, plus one Vivify prefab and no Blits, so none of Vivify's
  GraphicsTweaks-related code runs on it. A session log from that map with
  GraphicsTweaks on would show the visual check's light and camera lines.
- YOU's notes that sometimes disappear: its glass notes read the screen
  through a GrabPass. That is the first suspect, but it is not confirmed.
  The visual check counts prefab notes with nothing that can draw.

## [0.14.28] - 2026-10-27

### Fixed
- **Black Windows maps with Replay Render Mode left on (#92, likely #93).**
  The setting builds converted maps with single-screen programs only. Replay
  renderers can draw those, but the headset draws none of them, so a player
  who turned it on saw every converted map black until they turned it off
  (as the reporter of #92 found). It now applies only while the Replay mod
  is actually rendering a replay, checked through Replay's own API. Leaving
  it on no longer affects normal play, and without Replay installed it does
  nothing. The menu label says so, and the per-level settings line in the
  log now includes `replayRenderMode`. #92's log shows the cause: the
  converted caches carry the `_replay` suffix, but the settings line had no
  field for the setting.
  - Read from Replay 0.5.2's source: its render flag is set when Render is
    pressed, before the level loads, which is when Vivify picks the bundle.
    Not tried on a headset.
- **Videos follow the song speed (#91).** At Slower Song, Faster Song, a
  Practice speed or a mod such as BSNightcoreQuest, a map's videos kept
  playing at 1x. A video attached to a note or saber prefab was also yanked
  back to the song every time it drifted 0.15 s, so it stuttered. Videos
  now play at the song's speed, both those on note or saber prefabs and
  those in `InstantiatePrefab` objects, which were not tracked at all.
  - Animators and particle systems already followed the song speed, and
    property animations already run on song time; neither changed. Textures
    that animate in their shader (`_Time`) run on the game clock, as on PC,
    and still do not follow the song speed.

### Known, not fixed
- Mirror head-tilt in Lifelike (#81). The map's reflection is the game's
  own environment mirror: the map turns on its `Mirror` object and asks
  for mirror quality High. On Quest that renders only when GraphicsTweaks
  enables mirrors, which matches the report that the problem needs
  GraphicsTweaks. Vivify does not touch how the game renders mirrors.
  Whether the tilt also shows on a non-Vivify map with GraphicsTweaks
  mirrors on would tell if Vivify is involved at all.

## [0.14.27] - 2026-10-16

### Fixed
- **Geometry shaders that pass a varying straight through no longer leave
  their whole variant grey.** A geometry program that passed a semantic
  through (for example `TEXCOORD0`) would need one name as both its input
  and its output, so the converter refused it, and the variant stayed on
  DirectX: grey or missing models on the headset. The geometry output now
  takes a `gs_` name and the fragment stage's input is renamed to match when
  they link.
  - Reported in #60 (42 Flux: grey or missing models; Extra Sensory II) and
    the geometry-shader variants in 743 Aether (#64, #81).
  - Checked: a raw 743 Aether PC bundle now translates 34 of 35 shaders with
    all 180 programs compiling and linking under glslang, where 2 of its
    shaders were refused before. A new converter test links a pass-through
    geometry stage with its fragment stage.
- **Map saber trails work with Custom Models (#89).** Custom Models builds
  its sabers through Lapiz, which skips the game's saber setup, so the saber
  has no trail of its own and Vivify's trail, which followed that trail, was
  never made. Only Custom Models' trail was left. Vivify's trail now follows
  the saber itself when the game's trail is missing. When the map replaces
  the trail (not additive), Custom Models' trails are hidden for the level
  and come back afterwards.
  - Read from the Custom Models and Lapiz sources. Not tried on a headset.

### Compatibility
- **GraphicsTweaks bloom keeps a map's effects in the PC order.** On Quest
  the game's bloom (main effect) is off, so Vivify never placed it among a
  map's Blit effects. GraphicsTweaks (the "PC graphics settings" mod) can
  turn bloom on. Bloom then ran first, in the game's own pass, and every
  effect, including "before main effect" ones, was drawn over the bloomed
  image. When bloom is on, Vivify now renders it between the map's pre- and
  post-effects as on PC, and skips the game's pass so it is not drawn twice.
  Secondary cameras that ask for `mainEffect` get bloom as on PC. With bloom
  off, as in an unmodded game, nothing changes. The log notes when Vivify
  takes over rendering bloom.
  - GraphicsTweaks 0.1.13 (the 1.40.8 build) was read for other overlaps:
    MSAA (Vivify's effect textures already drop multisampling), resolution
    scale (effects follow the frame's real size each frame), and depth and
    smoke (Vivify still turns on scene depth for maps that read it). None
    needed a change. Its higher refresh rates and resolution do make heavy
    maps slower; Effect Resolution and Offscreen Culling help there.

### Known, not fixed
- Two geometry shaders in 743 Aether (Beat Saber Wireframe 3D-Noise and
  TriangleExplosion) read `SV_RenderTargetArrayIndex`, the eye index in
  single-pass stereo, as a geometry input. Those variants still stay on
  DirectX. Turning on Draw Geometry-Shader Effects draws them without the
  geometry stage. Passing the eye index through the geometry stage needs a
  headset test.
- Solid or white screen-effect maps (#67, #69, #87) are not reproduced here:
  their bundles are not in the reports' logs. The experimental Blit Fix
  setting is the current workaround.

## [0.14.26] - 2026-10-16

### Added
- **Two downloads per release.**
  - `Vivify-v<version>.qmod`: the full build, every feature.
  - `Vivify-Lite-v<version>.qmod`: the feature set of 0.14.7 (plus the
    geometry-shader setting, which the issue asked for). The lite build
    hides and switches off everything added after 0.14.7: effect resolution,
    culling, the PC-bundle-only option, crash guard toggle, prepare shaders,
    replay render mode, the blit fix, the Reset and Redownload buttons, and
    the scene-depth scan (depth stays on, as in 0.14.7). Its converter also
    translates every shader variant, as 0.14.7 did, and it keeps the
    freeze fix from 0.14.22 (integers in integer variables).
  - Both keep mod id `vivify`, so they share one settings file. Install one.

### Changed
- **Settings are grouped**: Performance, Shaders and Conversion, Features,
  Conversion actions, Diagnostics.
- **Finer slider steps**: Effect Resolution moves in 5% steps, Far Culling
  in 10 m steps.
- The bottom of the settings menu is no longer clipped: spacer lines follow
  the diagnostic paths.

### Not changed yet
- Mirror and reflection head-tilt (issue #81): reflections still move with
  the headset. This build does not fix it; it needs a headset test to be
  sure of the cause.

## [0.14.25] - 2026-10-13

Requested as "0.14.24.1"; qpm and QuestPatcher only accept three-part
versions, so it ships as 0.14.25. Every feature and setting is unchanged.

### Changed
- **Fewer frame hitches from garbage collection.** Every frame with a map
  effect running, Vivify created a new command buffer for each blit (and one
  per mid-render effect group, plus one per depth copy for map-created
  cameras), then threw it away. That steady managed garbage is what the
  garbage collector pauses the game for. The buffers are now created once
  and reused.
- An animated global shader keyword is switched only when its state
  changes, not on every frame of the animation.

### Fixed
- **Compatibility with other mods.**
  - Vivify's part of every hook it shares with the game and other mods
    (sabers, notes, bombs, chains, debris, cut effects, pause, image
    effects) is now isolated: if it throws, the error is logged once and
    the game and the other mods carry on. Until now such an error unwound
    through the other mods' hooks and usually crashed the game.
  - The game's room recentering (VRCenterAdjust) runs untouched outside
    Vivify maps. Vivify was skipping it in the menu and in ordinary maps,
    so it differed from the base game everywhere, not just in Vivify maps.
  - A failure in Vivify's start-up, or an unreadable config file, no longer
    terminates the game at launch: Vivify disables itself or starts from
    its defaults, and every other mod keeps working.
- The session log now lists every loaded mod and its version at startup,
  so a problem caused by another mod can be spotted from the log.

## [0.14.24] - 2026-10-10

### Added
- **Use Only PC Bundle For Missing Shaders** (settings, off by default).
  When a map's Quest bundle shipped shaders empty that its PC build has,
  the whole map is played from the converted PC build instead of the Quest
  bundle with those shaders patched in.
- **Crash Guard** (settings, on by default). Off, a converted map is never
  put into the grey untranslated fallback, however its loads ended.

### Fixed
- **Converted maps stuck grey with invisible notes (Yoi Okashi to Warui
  Okashi).** Two loads of a converted map that never finish put it into the
  crash guard's fallback: converted again without shader translation, so
  everything draws as a grey stand-in and replaced notes can disappear. It
  stayed that way until the converter changed. The freezes caused by other
  mods (Adaptive Audio Latency and others) counted as strikes, so maps went
  grey that have nothing wrong with them: Yoi Okashi's bundle converts with
  all 24 shaders and 163 variants linked, and all 216 programs compile and
  link under glslang.
  - Conversion cache version 19 clears every strike, so each converted map
    gets its translated shaders back once.
  - A map loaded in the fallback is now named in the session log, with how
    to get it out (Force Reconvert All, or Crash Guard off).

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

