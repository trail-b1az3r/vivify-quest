# Upstream 0.15.11 integration — Test 21

Base: `e83fa2cc58c06941660ee3fcc448c42a89ba59cc` (upstream main, PR #114).
Previous offline source: Test 20, `b47893d4e195a421226e873e3682c6454eeda749`.
No headset commands, device configuration changes or song edits were made.
Test 17 remains the last installed candidate. Test 21 is an offline candidate.

## Upstream takes priority

| Overlap | Integration decision |
|---|---|
| Geometry stage output under native multiview | Use upstream `gl_Layer` routing. Remove our blanket geometry rejection from native-material reuse; supported geometry no longer forces two scene draws. |
| Geometry fallback preference | Accept upstream `Flat Geometry Effects`, new `geometryEffectsFlatV2` key and default OFF. The former enabled-by-default flat preference is not carried over. |
| Normal per-eye effects and guard-probe copies | Use upstream `BlitSlice` draws instead of the old potentially mismatched raw element copies. The all-layer presentation wrapper delegates to this same helper. |
| HUD visibility | Accept upstream layer 17 in the PC headset mask. |
| Normal-renderer screen guard | Replace the old three-sample guard with upstream's 16-block, two-second guard and level-wide disable/reset state. The four upstream helper/method bodies were checked against the fetched source and match. |
| Version/cache | Version 0.15.11. Shared custom cache epoch increases from 25 to 26 so every affected main/companion/donor cache can pick up upstream's new geometry output. Upstream's epoch 24 would otherwise reuse newer custom caches. |

Explicit-eye donors continue to suppress array-layer output: each donor renders
into an ordinary 2D texture, so writing layer 1 there is inappropriate. This is
scoped to explicit-eye output; native multiview uses upstream routing.

## Retained features absent upstream

- Animated screen material refresh and donor reselection on each use; scene
  snapshot invalidation when a material changes shader.
- The Test 19 sampling corrections for bias/offset ordering, shadow gathers and
  explicit level-zero comparison sampling.
- Ownership-aware release of declared and secondary-camera global textures.
- Ordinary camera, depth, projection, bloom, mirror and projector integration;
  explicit-eye globals and source-typed screen effects; resource lifetime and
  failure-restoration handling. These remain experimental.
- Optional paired uniform triangle tessellation, default OFF. Only an active,
  validated scene-eye tessellation pair forces the eye-scene path on stage
  grounds. Declined/disabled tessellation uses the supported upstream fallback;
  it is not newly translated. Adaptive Lifelike tessellation and tessellation in
  authored cameras/blits remain unsupported.
- The prior generated hook-relocator and scoped Unity liveness compatibility
  changes. This merge changes neither their implementation nor other mods.

The custom explicit-eye backend still leaves the screen guard inactive to avoid
cutting off authored effects during its image-processing route. The upstream
normal renderer uses the new guard when enabled. Its setting text states this
scope. A tripped guard removes effects for visibility; it does not restore PC
parity. Matching-descriptor raw copies used for private array slices/depth and
final staging remain distinct from the mismatched upstream fallback copies.

## Offline validation

- Normal and diagnostics-enabled Android builds passed. Restoring diagnostics
  OFF produced the same normal library bytes. Product binary excludes the
  diagnostic commands and practice controller.
- ASan/UBSan host checks: 264 DXBC, 151 bundle-converter, 613 tessellation
  prototype, 81 map tessellation integration, 47 actual material/native-routing,
  90 actual texture-cleanup/lifecycle, 221 frame/routing policy, 98 blit policy,
  44 render-target layout, 5,019 stereo binding, 39 specialization and 37
  tessellation cache checks.
- Test 20 fails the two new native-geometry preference checks; Test 21 passes.
- Four problem maps converted in native plus all six donor modes; 743 Aether
  additionally converted in native mode. All 2,506 distinct-per-mode pipelines
  linked with glslang, zero errors: Through the Screen 1,113, Dealer 291, Yoi
  561, Lifelike 448, Aether 93. These counts are stored variants, not draw calls.
- All 4,221 exported donor stage files across the four problem maps' 24 donor
  map/mode pairs match the previously validated Test 19 outputs byte-for-byte.
  Source bundle hashes also match the earlier run.
- `validate_map.py --include-native` and `--native-only` now exercise upstream's
  native path as well as the explicit-eye/mono fallbacks.

These checks establish buildability, host behavior and static shader links.
They do not execute the Quest graphics driver. Upstream's new geometry eye
routing, slice draws, gameplay appearance, head-tracked alignment and frame rate
still require a headset test. Full PC parity and native multiview performance
are not achieved or verified by this update. No new device result is implied by
prior Test 17 observations.
