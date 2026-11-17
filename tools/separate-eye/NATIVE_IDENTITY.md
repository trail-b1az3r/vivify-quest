# Native shader identity revalidation — Test 22

This adds one guard to our native-material classifier on top of Test 21's
upstream 0.15.11 integration. Upstream rendering implementations are unchanged.

## Reproduced fault

`CanUseNativeMaterial` caches classification within a material-capture epoch.
Test 21 checked the cache before reading the material's current shader. A known
material classified as native-compatible could therefore keep permission after
its shader changed or was removed, until a later capture advanced the epoch.
The uncached path already rejects those identities; the cached path bypassed
that requirement. This is a source/host reproduction, not an observed Quest
failure or a proven cause of Dealer's yellow frame or TV alignment errors.

The classifier now reads the live shader and verifies it is alive and identical
to the captured source shader before accepting a cached result. It retains the
existing epoch cache for the remaining classification work. Restoring the proven
source can use its original cached permission. Changed or missing shaders take
the existing conservative fallback decision. The fix does not alter shader
translation, add scene draws, allocate new caches, change settings or touch songs.
It adds a live shader identity read on cache hits; its runtime cost is unmeasured.

## Offline verification

The existing material regression runner extracts the actual production method.
Three additional checks exercise a valid cached result, a different shader, a
removed shader and restoration of the original within the same epoch. The first
native classification already existed, so there are 50 total checks. Test 21
fails the changed and removed shader cases; Test 22 passes all 50 under
ASan/UBSan. Existing material refresh and upstream preference checks still pass.

```sh
python3 tools/separate-eye/material_refresh_test.py --baseline 57d9550f --baseline-kind native-cache
```

The normal and diagnostic Android builds pass. Rebuilding with diagnostics OFF
matches the initial normal binary byte-for-byte. The packaged build excludes
diagnostic commands. No Unity player or headset was used during this work.

## Expanded native map coverage

The remaining 13 locally saved PC bundles were converted through the unchanged
Test 21 converter and every emitted native pipeline was linked with glslang.
Combined with Test 21's five native checks, all 18 saved maps pass: 1,715
native pipelines, zero errors. The existing four-map six-donor checks add 2,075
pipelines (3,790 total across the recorded map/mode pairs). Shader conversion
source files still match Test 21, and the shared conversion epoch remains 26.
Source bundle hashes were verified unchanged after validation.

These are stored shader pipelines and host behavior checks. They do not prove
which variants the Quest selects, graphics-driver compatibility, visual parity,
tracked-eye alignment or frame rate. Test 22 is an uninstalled offline candidate;
Test 17 remains the last installed candidate. After this focused fix and the
expanded checks, the next useful verification is a controlled headset run.
