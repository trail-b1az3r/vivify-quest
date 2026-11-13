# Experimental separate-eye geometry backend

Current offline candidate: Test 23 extends [texture ownership cleanup](TEXTURE_OWNERSHIP.md#camera-capture-resizing-test-23)
to camera capture resizing and allocation failures. It retains Test 22's
[native identity revalidation and expanded native-map checks](NATIVE_IDENTITY.md),
Test 21's [upstream 0.15.11 implementations](UPSTREAM_0_15_11.md) in preference
to overlapping custom code, and the missing [live material refresh](MATERIAL_REFRESH.md)
and [texture sampling corrections](../dxbc/TEXTURE_SAMPLING.md).

Test 23 has not been installed or tested on Quest. Test 17 remains the last
installed candidate. General tessellation, full Windows visual parity and native
multiview speed remain incomplete. The older prototype records below are
historical evidence, not validation of the current candidate.

## Earlier prototype 8 record

Status: prototype 8 fixes confirmed asset-lifetime and stereo-copy faults found
in prototype 7 playback. Full Windows visual parity and native multiview speed
are **not achieved or verified**. Through the Screen completed full native Unity
practice runs with its three timed effects and native/separate/native scene-route
transitions. Both eye images are populated after the explicit all-slices blit
fix. Yoi Okashi to Warui Okashi also completed its full 303.77-second practice
run with six donors ready, generated geometry visible in both eyes, timed blits
active and no renderer shutdown. These static captures do not verify moving-head
stereo or matched Windows pixels. Through the Screen's geometry-heavy section
remains slow. Tessellation is unimplemented;
the guarded unsupported dependencies below still apply.

All 18 installed maps passed Unity preparation again: 3,124 source-material and
897 donor pass attempts, zero unexpected failures, shader-repair failures or
fallback materials. An explicit unused-resource collection plus managed GC lost
none of 1,695 unique selected asset references (sum across maps). These are
preparation and resource-lifetime checks, not whole-map visual comparisons.

Base: upstream nightly commit `5b10900d8f498dcd461b3ffbe6737b8c98fda357`.

## Prototype 8 playback fixes

- AssetBundle-loaded objects referenced only from the native runtime cache now
  retain their original hide flags plus `DontUnloadUnusedAsset` while selected.
  Reset/unload restores just that retention bit. A managed SafePtr alone did not
  protect native Unity objects from `Resources.UnloadUnusedAssets`: Through the
  Screen's MildAcid material was destroyed before its 73.125-second Blit event.
  Asset lookups also reject a managed wrapper whose native object has died.
- Mono texture-camera material registration depends on loaded mono shaders,
  rather than a camera-prefab cache-generation marker. The marker can be absent
  on an otherwise valid warm-cache play. TV camera readbacks now contain both
  saber traces with the controllers resting; moving-controller alignment is not
  established by that check.
- CommandBuffer blits into arrays and a stereo camera target explicitly retain
  all destination slices. Unity 2022.3's convenience overload supplies destination
  slice zero, even when the RenderTargetIdentifier was constructed for all
  slices. A first tracked test had a black right eye when geometry activated;
  subsequent full-song runs populated both eyes after this change. Ordinary 2D
  camera targets keep their original blit path.

Prototype 7's apparent disappearance of the lower corruption was inconclusive:
its saved user run was in crash-recovery mode, which bypassed the experimental
renderer. Later tests intentionally cleared only that known deliberately stopped
Through the Screen recovery entry, after saving it. Recovery protection remains
enabled. No song files or original bundle selections were rewritten.

The existing opt-in menu harness has a second, default-off
`VIVIFY_GAMEPLAY_DIAGNOSTICS` gate for explicitly authorized local regression
runs. It uses the game's practice transition, a fresh No Fail modifier object,
disabled score submission, a native stop deadline, and app-private commands.
Texture readback resolves HDR/MSAA into ARGB32 before CPU encoding; early raw HDR
readbacks were invalid and are not visual evidence. An ordinary mono reference
camera does not override HMD tracking and is not a stereo parity proof. An
abandoned XR view override was removed. Both diagnostic flags must be OFF for a
QMOD; diagnostic commands and practice controls are absent from that binary.

Unity unscaled frame-period samples in a full tracked Through the Screen run
had medians around 19 ms in the opening and 34 ms in its separate-eye geometry
section, excluding readback intervals. These are frame periods, not measured GPU
times or a controlled benchmark. OVR returned no usable frame stats and the XR
GPU query returned zero; zero is treated as unavailable. Actual performance and
comfortable moving-head stereo alignment remain unresolved.

Two diagnostic-only native-layout tests read Unity 2022.3's XR display properties.
The provider advertised layouts 1 | 4 (arrays and separate 2D textures). A bounded
six-second request changed the reported textureLayout from 1 to 4, but the actual
legacy stereo mode stayed 3 (multiview) and render-pass count stayed one across
ten samples. The original layout was observed restored. This did not establish
native multipass support in Beat Saber's built-in renderer. The temporary setter
and probe command were removed; no product rendering-mode switch is shipped.
The retained diagnostic properties are read-only. This is not a performance fix.

## Prototype 7 PC camera integration (retained in prototype 8)

The reference is [Aeroluna/Vivify at f83aa3c](https://github.com/Aeroluna/Vivify/tree/f83aa3c51290e288edae872dc2dac6b66112d3d6).
Its secondary-camera, main-effect, depth-copy, bloom and mirror behavior informed
these changes. They are implementation changes, not claims of matched images.

- Replacement eye transforms, non-jittered projections and culling matrices now
  come from the real XR eye views. Mono texture cameras retain their authored or
  reflected camera matrices rather than borrowing an enclosing eye's matrices.
- Each replacement eye and Vivify secondary camera gets a native BloomPrePass
  with the same four serialized configuration fields copied by PC Vivify. Private
  render data and textures are never shared. Quest 1.40.8 initializes that native
  component lazily; its Awake is empty. After both eye images finish, the redundant
  main camera's bloom is suppressed along with its geometry and depth draw.
- Vivify secondary-camera main effects run inside that camera's image callback,
  while its own bloom and depth bindings are current. Their default is enabled,
  matching PC Vivify. The old later self-filtering pass has been removed.
- Mirror renderer caches are isolated by actual source camera identity. Each
  private native renderer retains its own reflected camera and render textures;
  authored quality settings remain live. A scoped identity stack preserves native
  recursion checks. Mirror cameras receive culling/property and mono shader
  integration; nested renders restore the enclosing camera's depth, bloom, tone
  mapping keywords and active target. Native mirror behavior needs device tests.
- Completed eye colors are copied to an ordinary staging array, then presented
  through Unity's blit path at the XR boundary. The previous direct XR copy has
  not been proved equivalent. This adds one full-screen blit and staging array,
  with no third scene render. Whether it fixes the lower strip is unverified.
- The original PC MIT-licensed DepthBlit resource is translated and embedded.
  Capture uses its 2D or explicit array-slice material, with a checked native slice
  binding. The resource conversion is byte-reproducible and all four GLSL pipelines
  link. Unity resource loading and native-slice binding availability passed;
  actual capture depth pixels remain unverified. Source hashes and the
  upstream license are in `assets/depth/`.
- The opt-in converted cache adds private lookup entries for materials and
  successfully translated shaders reachable from authored preload ranges. This
  exposes animation-only dependencies to shader repair and donor collection.
  Every reachable material/translated shader gets a unique private object alias,
  including those already named in the bundle. Original container entries
  (including repeated sub-asset paths), preload tables,
  object identities and references remain intact. Each alias uses the smallest
  complete authored preload range. Unknown layouts fail atomically; aliases are
  excluded from authored friendly-name lookups. Song files are never rewritten.
- Structurally typed shader names preserve UTF-8 and CR/LF. Both root and parsed
  shader names are isolated. Through the Screen's `Mawntee/DrosteObject` name has
  trailing CR/LF; all 20 shaders are now present in every donor, rather than 18.
  This is a confirmed missing-donor bug, not proof of the later camera exception.
- Donor selection now follows preserved material/shader asset references. Kitchen
  Gun contains three distinct `Hidden/Tube` shaders. Unique object aliases keep
  them distinct; authored paths bridge independently built Quest/Windows bundles.
  A display name is used only when unambiguous. Capability checks and shader roots
  include every distinct shader object, including colliding names.

The dependency cache proof is
`render-dependencies-v5-unique-asset-identity`; cache selection is frozen
for each conversion worker so a setting change cannot mix cache identities.
Validation includes 140 bundle tests with ASan/UBSan and real GLSL linking, 216
renderer policy/frame/mirror recursion checks and 39 specialization proof checks.
A Through the Screen comparison found all 294 objects preserved between baseline
and lookup-enabled conversion except the intended AssetBundle lookup metadata;
all 13 original roots and every other AssetBundle field remained unchanged, with
49 private lookups added. A final audit of all 19 source bundles in all six modes verified byte-identical
shader and authored object bodies against the linked outputs, except for the
intended lookup metadata. On the Quest 3 Adreno 740, all 4,187 distinct shader
pipelines (10,852 references including the four embedded depth pipelines) passed
compile/link. The existing geometry, depth/blit and native stereo pixel-readback
checks also passed. These checks do not execute Unity gameplay.

The distributable build omits all temporary diagnostics. Preserve the user's
existing separate-eye setting, per-map bundle choices and tuner profile. The
screen-texture blit fix remains on; approximate geometry remains off. Latest
practice tests used No Fail and disabled score submission, restoring the exact
saved configuration afterward. No Quest, boundary, tracking, display, clock or
sleep setting changed during prototype 8 tests. Earlier prototype 7 preparation
checks temporarily paused the boundary with explicit authorization and restored
it with a watchdog; that was not needed for the current runs.

Through the Screen's timed effects and both populated eye outputs have been
exercised, but no matched Windows/Quest headset recording or moving-controller
comparison establishes full visual parity. Preparation, driver linking and a
successful full-song run each test different properties.

One earlier startup crashed inside installed MetaCore 1.6.2's A64 hook relocation
for AudioTimeSyncController.Update, before map loading. Official debug symbols
matched the installed binary's complete text section. Two subsequent launches
completed successfully; the dependency was not replaced or modified. This
intermittent startup failure is unresolved and is not evidence about map rendering.

## What is implemented

`GlslOptions::singleViewStereoEye` and the corresponding bundle option generate
left/right shaders for non-layered render targets. They preserve the logical eye
in SPI instance IDs, system-value inputs and legacy eye uniforms, and discard
geometry layer writes because each draw's render target selects its eye. This
avoids relying on `GL_EXT_multiview_tessellation_geometry_shader`.

There is no global `_StereoActiveEye` mutation. Eye-specific shaders have isolated
shader names (`__VivifyEye0__/...`, `__VivifyEye1__/...`, `__VivifyMono__/...`,
`__VivifyBlitEye0__/...`, `__VivifyBlitEye1__/...`) and distinct
internal archive names so they can coexist with the ordinary companion and main
bundle. They are **shader donors only**: renamed texture resource streams still
refer to the source bundle. Do not install them as a main bundle or overwrite a
normal Vivify cache entry with one.

The host CLI exposes `--shaders-eye-left` and `--shaders-eye-right`, plus
`--shaders-eye-left-isolated` / `--shaders-eye-right-isolated` for game donors.
The experimental game renderer always uses isolated modes. `--shaders-mono-isolated`
builds ordinary mono-camera programs; `--shaders-blit-left-isolated` and
`--shaders-blit-right-isolated` preserve logical-eye instructions while lowering
the screen input to 2D. Unsupported tessellation remains explicitly refused.
Legacy shader objects without parsed variants are refused for this mode, because
their SPI keyword metadata cannot be recovered reliably.

## Host validation

Build the normal host converter and DXBC harness with the source lists in
`.github/workflows/validate.yml`. These tools need no game or Unity editor.
Use ASan/UBSan and put `glslangValidator` on PATH to enable real GLSL compile/link
checks, then run:

```sh
python3 tools/dxbc/run_tests.py /absolute/path/to/dxbctool
VIVIFY_CONV=/absolute/path/to/conv python3 tools/bundleconvert/run_tests.py
python3 tools/separate-eye/make_probe.py /absolute/path/to/dxbctool /tmp/geometry-shaders
```

## Device graphics checks

The probe creates a 32x32 EGL pbuffer, prints the driver and combined-extension
support, compiles the **translated** vertex/geometry/fragment pipelines and checks
actual readback pixels. It never opens or modifies Beat Saber. Geometry must be
black for left and white for right over a blue clear color. Additional translated
post-effect pipelines sample actual depth textures and color inputs bound as 2D,
with different expected values for each eye. Missing geometry, array samplers,
or lost eye selection fail the device probe.

Build with an Android NDK (not included), for ARM64 and API 24 or newer:

```sh
cmake -S tools/separate-eye -B build-geometry-probe \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake --build build-geometry-probe
adb shell mkdir -p /data/local/tmp/vivify-geometry-probe
adb push build-geometry-probe/geometry-probe /data/local/tmp/vivify-geometry-probe/
adb push /tmp/geometry-shaders/. /data/local/tmp/vivify-geometry-probe/shaders/
adb shell chmod 755 /data/local/tmp/vivify-geometry-probe/geometry-probe
adb shell /data/local/tmp/vivify-geometry-probe/geometry-probe /data/local/tmp/vivify-geometry-probe/shaders
```

The probe compiles and links for Android ARM64 with NDK r27d. The full mod also
compiles and links for Beat Saber 1.40.8. The synthetic geometry/depth/blit and
native two-view pixel readbacks passed on a Quest 3 Adreno 740 GLES 3.2 driver.
A passing probe establishes a shader route, not Unity visual parity.
Prototype 8 gameplay coverage and performance limits are described above. The fixture generator promotes all stages to the same
GLSL ES version, as the real converter already does; Adreno rejects mixed versions.

`geometry-probe --link-list /device/path/pipelines.txt` also compiles and links
caller-supplied pipeline stems (`.vert`, optional `.geom`, `.frag`) without drawing
a map. The manifest and shaders share a directory. Keep caller-owned map shaders
separate from the distributable synthetic fixtures. These checks exposed geometry
pass-through `gs_` outputs whose flat interpolation did not match the fragment
inputs. Converter version 23 now propagates qualifiers to both `vs_` and `gs_`
outputs, and host validation checks matching stage versions and interpolation.

## Adaptive native multiview

The advanced setting **Separate-Eye Renderer (prototype)** is off by default.
The first normal camera frame
learns the real image descriptor. Subsequent frames are classified before culling.

The source shader scan records every geometry/tessellation shader from both pass
references and stored program types. Missing/partial metadata, unknown materials,
stand-ins and unsupported shaders cannot authorize the native shortcut. Every
active map material and screen effect must be compatible; native mode must really
be XR single-pass multiview, not merely a two-layer texture in another XR mode.

The sixth isolated donor (`--shaders-native-perspective-isolated`) specializes
geometry passes only when **all** their relevant variants prove the same early
return on perspective cameras. The proof permits only local declarations and a
PrimitiveID read before `unity_OrthoParams.w != 1` returns without emitting.
Observable vertex storage/synchronization and unproved variants reject it.

Proved geometry passes become a clipped, geometry-free native multiview program.
Other passes of the shader retain their original programs translated for native
multiview. Other shaders remain separate isolated donors and are never selected
for this shortcut. Root/parsed shader names and cabinet names are isolated.
The runtime applies the specialized material only to the perspective main view,
with realtime shadows ruled out, then restores the exact original arrays after
rendering or on disable/reset. Orthographic texture cameras retain real geometry.

Through the Screen's **AreaLit/LightMesh** passes this proof. Its ordinary texture
pass is preserved; its geometry pass is empty for the headset perspective view.
**AshleyScarlet/ParticleScreen/Visualizer** does not pass and retains its geometry.
The authored ViewYoinker object exists from beat 224.5 to 324 at 160 BPM
(84.1875–121.5 seconds). This identifies an expected expensive section, not a
measured guarantee that every other frame will select native rendering.

Compatible frames use the original native multiview scene and post-processing.
They allocate no eye scene targets and submit no replacement eye scenes.
Otherwise the prototype prepares two complete replacement views before main
culling, and suppresses the redundant original geometry/depth draw only after
both finish. Their images are copied into an ordinary staging array and presented
through Unity's blit path at the XR boundary. This costs one additional full-screen
blit and an array target relative to prototype 6's direct-copy shortcut, while
keeping the third scene draw suppressed. Direct XR copies have not been proved
equivalent to native presentation. Frame tickets prevent stale
or different-camera images from being presented.

Quality remains the acceptance target. While this experiment is enabled, effect
resolution stays at 100% and automatic scaling is ignored. Saved controls remain
unchanged for the ordinary renderer; crash recovery retains its existing behavior.
No tuner profile, display resolution, refresh rate or CPU/GPU clock was changed.

## Preparation and CPU work

The menu shows **Preparing map effects...** and uses a separate SongCore play-button
gate until all six donor loads and capability checks complete. Support queries are
spread across menu frames. Required projection/copy bindings are checked when the
map has geometry that needs separate eyes. A conversion/load/unsupported-shader
failure is reported before play rather than silently starting an incomplete map;
disable the experimental setting to return to ordinary rendering. Selection,
disable and unload remove this gate. Requests time out after three minutes.
This checks capabilities, not every keyword variant or rendered GPU pixel.

Map/replacement renderers are registered on spawn, dead entries pruned, and shared
material reads use a rooted reusable List. Snapshot arrays and camera scope storage
are pooled, including inactive slots. Native compatibility is cached only within a
snapshot, so changed shaders/keywords are rechecked on later frames. Material copies
are synchronized once per variant per scope, with a new epoch for screen effects.
Layer-excluded renderers are skipped only when realtime shadows are known disabled;
unknown shadow settings keep them. View-frustum guesses do not discard geometry.

The separate-eye path still renders full scenes so notes, scenery, depth and
transparent sorting remain together. Its pre/main/post effects run in each eye's
image callback. Native bloom state is set for scene rendering as well as filtering.
The camera's redundant culling/depth settings are restored after rendering,
disable or reset. Failure before preparation completes keeps the normal draw;
a late presentation/GPU failure can lose that frame before normal rendering resumes.

Logs report route transitions, native/separate frame counts and periodic
**CPU wall-time** preparation averages/maxima. These are not GPU frame timings or
proof of a frame-rate gain. Two mono cameras still cost separate culling/submission
on frames requiring them. Standalone driver checks do not verify Unity rendering.

## Depth and authored texture cameras

Per-eye scene shaders lower only Unity camera textures and named GrabPass targets
(read from serialized pass metadata) from arrays to ordinary 2D textures. Real
texture array assets remain arrays. Separate eye-aware post-effect donors also
lower `_MainTex`, without changing scene materials that use `_MainTex` arrays.
This avoids substituting a plain shader that may have compiled away eye-dependent
instructions. Cache version `separate-eye-v8-render-dependencies-typed-names-converter23` prevents
reuse of the previous prototypes' incompatible donors. The native perspective donor uses
`perspective-native-v6-render-dependencies-typed-names-converter23`.

Authored prefab texture cameras retain their transforms, projections, clipping,
clear mode, culling, depth, target size and camera ordering. From `OnPreCull`
(before depth generation) they use isolated plain mono donors. The mono shader
substitution does not depend on an eye frame being accepted; these camera fixes
can remain active while the main view falls back. Nested scopes
restore the exact material arrays they found. These scopes take priority over the
older optional single-view camera swap and don't depend on its companion being
ready. Disable/reset/failure paths unwind scopes and release resources.

Blit texture caches check dimension, array depth, format, size and VR usage, so
switching between mono and stereo does not reuse an incompatible texture. Both eye
image callbacks must complete before suppressing the redundant main draw; missing
or wrong-sized/non-2D depth rejects the experimental frame. Temporary bloom
textures and main-effect callbacks are balanced on exceptions.

Explicit-eye shaders also expose reflected stereo variables as named uniforms
instead of depending on Unity to update the multiview stereo UBO for mono cameras.
The renderer supplies both eye matrices and camera positions explicitly. GPU
projection and non-GPU camera projection arrays are distinct. Native multiview
shaders keep the engine-owned stereo block; mono texture-camera shaders use their
ordinary camera variables. This binding change passed frontend validation but
still requires Unity image verification.

## Current supported scope and fallback

The prototype keeps the normal main-camera draw when donors are unavailable, conversion
is incomplete, Vivify CreateCamera secondary cameras or mid-render effects exist,
post effects read/write shared named screen/camera targets, realtime reflection
probes are present, multiplayer/crash recovery/pause is active, or the main camera
needs a prior background. Frames needing separate eyes also require a full viewport,
a resolved stereo target and cross-type array-copy support; native frames do not
need these extra copy restrictions.
Rendering exceptions disable the experimental path for that play session.

**Through the Screen's depth and prefab texture cameras now have an implemented
path.** Its three post effects target the main image; its in-world cameras are
prefab cameras, not Vivify CreateCamera events. This makes it an intended device
test target, not a verified compatible map. No matched Windows/Quest image
comparison or in-game GPU timing measurement has been performed.

Offline `validate_map.py` converts all six donor modes, checks isolated names,
cabinet separation and camera sampler shapes, and links every unique GLSL pipeline
with glslang. It takes a caller-supplied PC bundle; no copyrighted map assets are
included in this repository:

```sh
python3 tools/separate-eye/validate_map.py /path/to/conv /path/to/map.vivify /tmp/map-validation
```

Device checks still needed: Unity's shader selection and texture binding, correct
left/right views and image orientation, scene depth and GrabPass contents, native
bloom behavior, authored camera timing, reset/pause/unload, warmup and frame time.
The existing menu warmup does not draw all donor keyword variants. Two separate scene
cameras still need profiling; performance could prevent full-quality playability.

## Later test sequence

1. Run the standalone probe and retain its driver, extension and pixel-readback log.
2. Install only the full prototype QMOD on Beat Saber 1.40.8 (it replaces Vivify).
   Leave the new setting off and check the ordinary renderer first.
3. Enable the setting, select Through the Screen using its PC bundle, wait until **Preparing map effects...** finishes, and test. Keep ordinary fallback
   and experimental captures separate. Check the map's screens/shadow cameras
   and the depth effects around 73.125, 97.125 and 130.5 seconds.
4. Keep the Vivify session/global logs, a stereo capture and frame timings. A log
   saying `prototype active` proves submission, not correct pixels or Windows parity.
5. Turn the setting off to compare. To roll back, reinstall the official full
   Vivify build; its normal cache files are untouched by donor conversion.

The long-term acceptance target is the original Windows effects, stereo, depth,
camera behavior and timing, assessed against matched captures. Approximate meshes,
plain shading, or simply loading a map do not satisfy that target.

Do not re-enable the old `multipassRendering` code. It does not switch XR rendering
mode and its global eye state is deliberately disabled upstream.

Tessellation requires a separate DXBC hull/domain translator (including patch
constants, control-point and fork/join phases), plus primitive/topology handling.
It is not implemented in this branch.

## Guarded material feature specialization

Prototype 6 integrates an exact fixed-feature profile for Sunao Shader/Cutout.
This addresses shader variants exceeding the Quest driver's sampler limit when
their materials leave the optional features disabled. It replaces only matching
scalar uniform declarations with the authored constants; texture instructions,
geometry and other material settings are retained for the GLES optimizer.

Authorization requires all difficulty files to use a known non-material-writing
event schema. The converter also requires readable material and shader property
definitions, every affected material matching the complete float profile, known
component types, transform-only animation bindings, no animation callbacks and
no conflicting saved value types for the baked flags. Unknown inputs retain the
ordinary shader path. A separate cache proof generation prevents older results
being reused under changed conditions. Main, replay/helper and all six renderer
donor conversions use the same authorization and profile.

Menu preparation and instantiated objects register the marked materials and
renderers. Runtime checks compare the actual float values and shader identity,
inspect current material assignments, and reject renderer property blocks. They
reuse a material list and property IDs. A condition change blocks Play, disables
the experimental renderer and reports a visual problem instead of silently
continuing with frozen values. The temporary menu diagnostic also checks a
private material copy: changing a baked flag must fail validation and restoring
it must pass, while the source material remains unchanged.

Both supplied Windows bundles passed driver linking: 335 isolated programs from
Windows2021 and 390 from the Windows2019 bundle selected by the current resolver.
The final affected-map menu check validated 24 actual materials, passed the
private-copy change check and loaded all six donors without unresolved pass
binding failures. Through the Screen also passed a final preparation check after
switching from that map. These checks do not establish rendered Windows parity,
gameplay frame rate, or support for mutable Sunao material profiles. No PR has
been opened. The prototype 8 sweep repeated all 18 installed maps after the
asset-lifetime and stereo-copy fixes, including this guarded profile.

## Opt-in diagnostic builds

`VIVIFY_MENU_DIAGNOSTICS=ON` adds an explicitly enabled local harness. The
default is OFF, and distributable QMODs omit it. Without the additional gameplay
flag its commands are limited to `prepare`, `status` and `unload` for installed
SongCore levels. Preparation/unloading refuses active gameplay. `collect_unused`
optionally checks selected cached assets across unused-resource cleanup and GC.

`VIVIFY_GAMEPLAY_DIAGNOSTICS=ON` additionally requires the menu flag, and is only
for explicitly authorized local testing. It adds bounded No Fail practice,
owned-run stop, camera-texture readback and the already-observed Health and
Safety Continue handler. It refuses gameplay when Vivify score submission is
enabled and never mutates saved modifiers. The gameplay diagnostic selection
invokes SongCore's selection event with the actual level hash; it still does not
reproduce every UI coordinator callback. Both flags must be OFF for release.
No headset tracking or native view override is installed.

The Quest 3 run loaded all 18 installed Vivify maps without playing. All menu
preparations completed; six converted-PC maps loaded all six renderer donors.
The other 12 used their official Quest bundles, with ordinary shader repair
where applicable. All 67 difficulty files were inspected as data. Settings kept
Windows selection per map and did not change the tuner profile. Missing official
visual bundles were downloaded; authored notes, events, music and bundles were
not rewritten. Conversion outputs remain separate generated caches.

Pass binding diagnostics distinguish non-draw passes using a caller-supplied
serialized pass-layout catalog. Type 2 is GrabPass, which normally returns false
from Material.SetPass. Classification requires all source subshader layouts of
the active pass count to agree; ambiguous or missing metadata is unresolved.
The Projector-enabled run had 3,114 source-material and 862 donor pass-binding attempts, with zero
unresolved binding failures. This covers the tested menu variants, not every
possible runtime keyword combination or any rendered map pixels.

The checks exposed and fixed selection/preparation lifecycle problems: loading
the legacy texture-camera helper no longer cancels the independent renderer
donors; preparing donors preserves original shader/material records captured
during bundle loading; stale companion source paths are cleared on selection
changes. Warmup restores the multiview keyword even if binding throws.

The screen-texture helper also retains the newest request while an older Unity
bundle/material/prefab request drains. Selection generations reject stale
background callbacks, including an A -> B -> A selection sequence and explicit
reselection of the same level. Clearing a selection invalidates queued work;
replacing this helper leaves the independent renderer donors intact. Background
helper conversions are serialized to prevent concurrent writes to the same
cache file, and conversion exceptions are reported rather than escaping a
detached thread.

Projector materials need their own collection, warmup and camera scope because
a Projector is not a Renderer. Beat Saber 1.40.8 retains the native Projector
implementation and material internal calls, but strips its managed class. Its
Unity 2022.3.33f1 engine returns these components as base Behaviour wrappers.
The bridge accepts an actual managed Projector when available; otherwise it
requires an exact Behaviour class, Unity's native Projector class string and
matching component identity from Unity's own lookup. It uses resolved material
internal calls without native offsets or fabricated wrappers. Validation is
cached by wrapper and instance ID; destroyed components must still pass Unity's
alive check before access.

On Quest 3 the bridge recognized Through the Screen's TvProjector in the source
and donor prefabs. Its shader now participates in donor collection (18 shaders
in each ordinary donor, previously 17), menu warmup, texture decoding, shader
repair, legacy single-view material registration and experimental camera scopes.
Projectors are classified separately from renderers: their own GameObject layer
does not identify receiver layers and must not exclude their materials from
camera classification. Camera scopes snapshot and restore the exact material.
Menu preparation verifies material reads and shader binding; native material
replacement/restoration during rendering and projected pixels still require
targeted playback. Compilation success must not be reported as full map parity.
Tessellation and the existing guarded rendering dependencies retain their
documented limits.

## Camera-texture parity candidate 9 (offline)

CreateScreenTexture now starts from the actual main image descriptor, as PC
Vivify does. It retains stereo array layers, HDR precision and attachment flags,
then applies only the authored dimensions, ratios, format and filter overrides.
Allocation waits until a real descriptor is available. Temporary eye callbacks
and other cameras cannot replace that shared main descriptor with a mono one.
Named feedback images persist across equal descriptors; recreation compares all
13 allocation fields and checks native storage. Secondary-camera captures,
mid-render targets and both post-effect scratch targets use the same identity
policy. Resolution scaling remains governed by the existing settings.

The saved official Quest shaders independently confirm the array requirement:
End Times' Aeroluna/Bloom and Kitchen Gun's Hidden/Old use sampler2DArray for
their named screen textures in multiview variants. Allocating an ordinary
Tex2D for those globals is incompatible even with the native Quest bundles.

Camera property resets now restore saved depth, clear flags and background on
non-main cameras as well. A requested depth capture remains part of its camera's
baseline. Empty culling whitelists retain their authored meaning. Invalid blit
passes no longer poison valid passes in a material-wide cache; cached validity
also follows live shader/support/fallback changes. Secondary-camera capture no
longer adds an unsolicited copy to the headset destination, matching PC's
OnRenderImage callback. These callback changes need Quest playback validation.

Per-eye material refresh skips only stereo-array uploads whose identifiers are
proved unused across every runnable GLES variant and reflected parameter table.
Opaque blocks, unknown declarations, comments/macros, unreadable metadata and
pre-existing GLSL retain the previous bindings. Donor identity uses the exact
shader object/name, with duplicate names unioned. No shader bodies are changed.
For the saved TTS donors, a full shader refresh would use 116 rather than 240
array uploads; this is a static call count, not a measured frame-rate gain.

Offline checks: 44 texture-policy cases, 5,019 conservative binding cases,
141 converter/GLSL integration cases, and 4,800 corruption mutations passed.
All 36 freshly generated donor bundles (six converted maps, six modes each)
were byte-identical to conversions built from the exact candidate-8 source.
The normal and diagnostic ARM64 builds are checked separately. No headset,
song file or system setting is changed by this offline candidate.

Candidate 8's device results must not be attributed to candidate 9. Before
installation, retain candidate 8 for rollback. Validate End Times' bloom,
Kitchen Gun's history/capture composition, camera overrides/reset, stereo/HDR
resize and lost-storage recreation, then TTS/Yoi with moving head and hands.
Matched Windows images and actual CPU/GPU timings remain necessary. This
candidate neither implements tessellation nor removes the renderer's guards
for shared targets, secondary cameras, mid-render effects or realtime probes.
It is not a claim of complete Windows parity or native multiview performance.

## Screen-effect failure investigation (candidate 10)

The headset still had candidate 8 when the user reported a flat yellow view in
WHY'S THIS DEALER, a black view in Lifelike after its grass transition, and
corruption in Yoi. Candidate 9 diagnostics reproduced Dealer's flat yellow view
in both eyes. A mono reference of the same live map/effect produced structured
yellow-and-black imagery. The exact installed 2019 bundle's Custom/PostProcess
uses sampler2D _MainTex, while the native main image is a two-layer array. A
successful shader link did not detect this Unity binding mismatch.

Candidate 10 adds main-only stereo image processing using the existing explicit
blit donors: extract each native eye image, run its screen effects on an ordinary
2D target, combine the two results in an ordinary array, then present through
Unity's XR-aware blit. This renders no additional scene cameras. Required camera
texture arrays are scoped to corresponding ordinary eye slices and restored
before presentation. Shared named feedback/camera targets retain the existing
guard; this path must not advance their state twice. Preparation, copy capability,
pause, recovery, multiplayer, layout and camera ownership are checked. Partial
results are not presented.

Explicit blit materials also need draw transforms separate from scene transforms.
The old clones supplied the headset's perspective VP even to Graphics.Blit's
0..1 screen mesh. Candidate 10 supplies a GPU-adjusted orthographic draw transform
to those draw-matrix arrays, retaining the actual camera projection, camera/world
transforms and eye positions for reconstruction. The arrays are rooted and reused.
This addresses a concrete matrix mismatch in Lifelike's Warm effect, as well as
TTS and Yoi's later screen effects. It is a pending runtime fix, not a claim that
their displayed results are verified.

Yoi's reported early corruption precedes its authored screen effects; its root
cause remains unresolved. The saved earlier low-headset captures are insufficient
to clear that report. Inspect eye images and the native main-effect path at the
reported pose before changing geometry, clipping or bloom based on guesses.
Local disassembly of the saved game's Kawase/Pyramid main-effect Render methods
shows source vrUsage/descriptor-based temporary allocation; it does not establish
the active effect type or identify a bloom fault. No bloom implementation changed.

The headset was unplugged after the bounded diagnostic runs ended. Candidate 10
has not run on it. The installed diagnostic library and temporarily disabled
Vivify score submission require normal-build/settings restoration on reconnect;
the exact saved configuration is retained outside the repository. No system,
boundary, tuner or song-file changes were made. Candidate 10 needs targeted
Dealer, Lifelike, Yoi and TTS validation before a normal installation or PR.

## Offline coverage audit (candidate 11)

The saved Yoi bundle selected in the user's failing run has one Roystan/Grass
subshader and one pass containing hull and domain programs. Grass, Grass_tree
and Grass_100 each store _TessellationUniform=5. The hull programs write a shared
factor to edge/inside tessellation outputs; the domain programs interpolate
position, normal and tangent, with the stereo form also copying instance data.
No explicit tessellation-property writer was found in the saved ExpertPlus event
data. This is a real missing stage, not proof of the reported corrupted strip's
cause. No guessed replacement grass or mesh subdivision was introduced.

Shader scans now report tessellation presence separately from the broader
geometry/separate-eye classification. Conversion reports count shaders whose
hull/domain stages remain DirectX even when their VS/FS/GS programs link. The
runtime preparation log and diagnostic status expose that missing coverage;
successful support checks no longer print an unqualified map-effects-ready line
for these bundles. Existing supported draws remain available. This changes
reporting, not shader stage support, and does not establish tessellation parity.

The stereo image-processing scope also catches managed/native failures, with
scope setup inside the restoration guard. No candidate-11 changes have run on
the headset. Candidate-9 diagnostics and the saved settings still need restoration
on reconnect as recorded above.

### Upstream 0.15.9 integration (candidate 12)

Rebased the complete candidate-11 patch onto upstream commit
`45ed52167fd6589bc3438c45403dacc91d6e410c`. The previous source is preserved at
`backup/test11-before-upstream-0.15.9` (checkpoint `c51fbe13`).

Kept upstream saber/trail/pause fixes, native Quest effect metadata and per-eye
blits, integer uniforms, reflection refresh, headset layers and `_ScreenParams`
updates. Conflicts in Assets, Core, PostProcessing and RuntimeInternal retain
the rooted asset registry, full render-texture descriptors, typed material
validity cache and explicit destination-slice handling from our patch.

The ordinary renderer keeps upstream's geometry flattening option. Explicit-eye
and mono donors always preserve supported geometry stages. Reflection-probe
camera callbacks also use scoped mono donors when available. The flat-colour
guard remains available in the ordinary renderer; experimental rendering
bypasses effect killing to preserve authored effects. Both ordinary and donor
conversion cache versions changed, so stale pre-update conversions are rebuilt.

Offline validation: 151 converter cases with GLSL linking, 235 DXBC checks,
4,800 malformed-input conversion runs without a crash, and 5,318 renderer,
texture, binding and specialization checks. Android normal and diagnostic
builds passed, and the final diagnostics-disabled rebuild was byte-identical.

Candidate 12 has not been installed or run on Quest. The Dealer/TTS/Lifelike
screen-input and draw-matrix fixes remain device-unverified. Yoi's early
corruption is unresolved; tessellation is still unimplemented. Full PC visual
parity and native performance are not verified. Device diagnostics and the
temporary score-submission setting still need restoration on reconnect.

### Screen texture routing and cleanup (candidate 13)

Fixed additional source-level faults found in the merged 0.15.9 blit path:

* A native Quest effect reading a flat 2D screen now samples that source
  directly for both eyes. It no longer attempts to copy nonexistent layer 1.
* For stereo input, the input scratch matches the source dimensions and
  graphics format. The output matches the destination, allowing the blit to
  scale between different screen sizes. Unresolved/unknown layouts cannot
  enter this raw-copy route.
* Every immediate and mid-render effect records `_ScreenParams` for its actual
  destination, restoring main-camera dimensions before presentation. Nested
  camera scopes also save and restore the enclosing `_ScreenParams`.
* The native per-eye path restores the original stereo keyword after errors
  during disabling, command recording or execution. Immediate blit resources
  clear partial commands before releasing temporaries and restore the active
  target; failures also restore the saved screen-size global.

Added 98 SDK-free routing, sizing and failure-injection checks to CI. Android
normal/diagnostic builds passed and the final normal rebuild was byte-identical.
Candidate-12 converter, DXBC, corruption and existing policy results are
inherited; that code did not change. These changes add no scene renders. The
flat-input route also avoids its unnecessary input temporary and layer copies.

No candidate-13 device tests or installation have occurred. These fixes address
demonstrable code faults, not a confirmed cause of Yoi corruption or proof of
PC visual parity. Tessellation remains unimplemented. Preserve the candidate-12
QMOD/source/manifest as rollback artifacts.

### Depth/projection snapshot consistency (candidate 14)

CameraBindings now snapshots `_ZBufferParams` and `_ProjectionParams` with the
saved depth image, screen and bloom state. Restoration attempts every captured
field even if an earlier setter fails, and failed snapshots remain retryable.
The existing reflection-camera shader callback moved from `onPreRender` to
`onPreCull`, before culling/depth generation.

The Quest menu-only diagnostic performs two nested captures with deliberately
different `_CameraDepthTexture`, `_ScreenParams`, `_ZBufferParams` and
`_ProjectionParams` values. Child, parent and original-state round trips passed
on the connected Quest. No map was played for this check. Normal/menu-only
diagnostic builds passed; the final normal rebuild was byte-identical.

Complete-render snapshots remain around existing owned camera/mirror/effect
boundaries. Global snapshot restoration was deliberately not added to arbitrary
texture-camera `OnPostRender` callbacks: Unity can run image processing after
that callback, so restoring parent depth there would be premature. Material
scope lifetime is unchanged. This follows Unity's documented
[rendering callback order](https://docs.unity3d.com/es/2018.3/Manual/ExecutionOrder.html).

Test 14 adds no scene renders. Its map visuals/performance remain unverified;
the specific cause of the reported corruption is not proved by this fix.
Yoi's early corruption is unresolved and gameplay tessellation remains disabled.
The compiler prototype documented in tools/dxbc/TESSELLATION.md is inactive.
Vivify preferences were preserved, including score submission. Quest system
settings and song files were unchanged. Test 13 remains the rollback QMOD.

### Offline startup and camera fixes (candidate 15)

Auxiliary cameras now refresh every PC-copied camera property in OnPreCull.
The previous mask/FOV/clip-only gate missed isolated changes to aspect, stereo
target eye, depth, layer culling distances and target texture. A menu-only
diagnostic checks an inactive disposable camera against the real main camera,
including source preservation and cleanup. It performs no render or blit.
Its runtime result is pending headset reconnection.

The SolidColor clear restriction now applies only to replacement eye renders.
Proven native multiview substitutions keep the original camera and its clear
behavior; they no longer fail this replacement-only restriction. Other
eligibility guards are unchanged. These changes add no scene renders.

The five saved startup failures reproduce exactly from their PC/instruction/
fault-source registers: a private And64 hook relocator zero-extends a negative
literal displacement and reads four GiB too high. Vivify builds a corrected,
dependency-hash-checked relocator. Related signed ADR, ADRP and conditional
branch decoding is corrected, including original-page ADRP handling. See
tools/startup/README.md for the scope and sanitizer regressions.

Other mods have hidden private copies. Hash-pinned offline literal-only repair
candidates exist for Chroma 2.10.3, Noodle Extensions 1.6.3 and Theater 0.1.0;
each changes one proven instruction. Installed hashes must be checked before
deployment, and their other relocation defects remain unpatched. No new
candidate has been installed or tested on the disconnected headset.

Normal/menu diagnostic builds and offline tests do not establish PC visual
parity or startup reliability on Quest. Next: verify installed binary identity,
back up original libraries, deploy matching repairs, run repeated cold starts,
run nested/auxiliary menu camera checks, then validate shader warmup and bounded
map rendering. Player testing comes after those checks. Yoi corruption,
full gameplay parity, tessellation and native-speed performance remain open.
