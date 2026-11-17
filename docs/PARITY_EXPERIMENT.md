# Experimental renderer extensions above upstream 0.15.15

This contribution preserves upstream geometry eye routing, per-eye slice blits,
trail recovery, native crash reporting, updater and ordinary renderer. It adds
an opt-in renderer path and independent shader/lifetime fixes. Neither this
branch nor its device samples establish full PC parity or native performance.

## What changes

- Explicit-eye shader companions for effects and authored texture cameras,
  with camera/depth/global binding scopes and projector-material handling.
- Native multiview scene reuse when materials permit it. Incompatible sections
  can require two replacement eye views and can be substantially slower.
- Additional texture-sampling overload corrections, geometry interpolation
  matching and conservative immutable-material/dependency handling.
- Live material/shader identity refresh and ownership-aware render-texture
  release/reallocation, including nested camera restoration.
- An optional uniform-factor triangle tessellation pattern for main views.
  Adaptive tessellation and authored-camera/blit tessellation remain unsupported.
- Vivify's embedded ARM64 relocation/hook-entry corrections and a process-local
  compatibility switch for the 1.40.8 custom-types debug traversal hooks.
  Intermittent startup failures remain unresolved.

The Separate-Eye Renderer and Triangle Tessellation switches default OFF.
The converter revision is 27 so it cannot reuse output from either upstream
revision 25 or the earlier experimental revision 26. The standalone converter
links the same triangle translator and reports the same revision.

## Validation for this merge

- Android arm64 normal build for Beat Saber 1.40.8_7379.
- 264 DXBC checks with ASan/UBSan and glslang, also repeated with upstream's
  geometry right-eye copy enabled; 151 bundle-conversion checks.
- 613 triangle tessellation prototype checks, including malformed-pair refusal.
- 50 checks of actual material/native-route methods and 636 checks of actual
  texture allocation/cleanup methods with controllable API stand-ins.
- 512 generated-relocator integration checks and 87 installer/hook-chain checks
  with protection intercepted, plus changed-dependency refusal. No generated
  machine instructions execute in those host tests.
- Upstream recovery tests and standalone converter C API smoke tests.
- 1,767 saved-map pipeline links across Through the Screen and Yoi's native
  and six companion modes, plus Aether's native mode; no link errors.

Host links do not validate Quest pixels. The new 0.15.15 integration has not
been played on a headset. Earlier experimental 0.15.11 builds were checked on
Quest 3, including a complete unattended Through the Screen practice run with
No Fail and scores disabled. Fixed headset/controller poses, diagnostic capture
stalls and isolated replay processing limit what those runs establish.
Reported visual regressions, moving-head alignment and sustained frame rate
still require controlled comparisons against Windows.

## Diagnostic builds

Distributable builds exclude menu command polling and practice control.
`VIVIFY_MENU_DIAGNOSTICS` enables the app-private menu checks;
`VIVIFY_GAMEPLAY_DIAGNOSTICS` additionally enables bounded owned practice runs
and requires the first switch. Practice requires score submission OFF, uses
separate No Fail/practice settings and refuses to replace active gameplay.
The direct-start diagnostic path sets ScoreSaber's supported process-local
`disable_ss_upload` switch to bypass replay/upload finish processing. Restarting
into the normal build removes that override. It does not test the ordinary
score/replay completion path.

Deliberately stopped diagnostic runs can trigger upstream crash recovery on the
next launch. Preserve recovery state before testing and restore only entries
attributable to the test; do not erase another map's existing recovery state.
No song edits or other-mod binary patches are part of this contribution.

See `tools/separate-eye/` for the focused regression suites and historical
investigation records. Optional supplied-map tessellation integration tests
require local map bundles and UnityPy; copyrighted song assets are not included.
