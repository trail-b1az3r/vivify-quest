# Texture binding ownership during cleanup

Test 20 retains Test 19's shader and material fixes and changes resource
cleanup only. It adds no hook, setting, shader variant or scene render.

Previously, releasing a declared screen or camera unconditionally cleared its
named global shader texture. That could erase a replacement owner's binding,
even if the released object had never allocated a texture. A repeated cleanup
could also clear a newer binding. Separately, a capture controller destroyed
outside the runtime's release path released its per-eye images without first
unbinding them.

The shared `ClearOwnedTextureBinding` compares the current texture identity
before clearing the property. It does not dereference the owned texture.
Runtime cleanup checks both legacy fields and actual per-eye controller maps.
The controller's destruction callback also unbinds its own color/depth images
before releasing them. Another owner's binding remains intact; native images,
camera state and allocation records are still disposed as before.

## Validation scope

```sh
python3 tools/separate-eye/texture_ownership_test.py
# With the pre-fix checkpoint available:
python3 tools/separate-eye/texture_ownership_test.py --baseline f7ba0107
```

The runner compiles the actual production screen cleanup, camera cleanup,
controller binding-release and destruction methods, plus the actual shared
header, with small API stand-ins. Under ASan/UBSan, 90 checks pass; the pre-fix
methods fail 48 of them. Cases include signed/opaque property IDs, mono/left/
right captures, color/depth key aliases, replacement textures, deferred
destruction, missing allocations, repeated cleanup and native-resource release
ordering. These are state/lifetime tests rather than Unity or GPU execution.

The existing menu diagnostics also contain a disposable fixture using two
private uncreated render textures and a saved/restored private shader global.
It checks replacement preservation, repeat cleanup and owned unbinding through
the actual runtime helper. It makes no explicit GPU-storage creation or draw calls, modifies no map
asset, and does not start a song. The fixture compiled but has not run on Quest.

A static review of 67 local charts across 18 maps found no overlapping
CreateScreenTexture/CreateCamera names or property names shared by different
camera IDs. This does not cover script/global-property changes or native
resource lifetimes. The cleanup faults are reproduced invariants, not an
established cause of the reported TV corruption, eye alignment or colored views.

The shader-conversion sources and converter version are unchanged from Test 19;
that candidate's shader/link evidence remains separate from the new cleanup
tests. The installable normal build excludes the diagnostic fixture. No headset
command was issued for this work; Test 20 requires device validation.

## Camera capture resizing (Test 23)

Reallocating a camera's color/depth capture is a separate release path. Test 22
removed the previous image from its capture map and released its storage without
explicitly clearing a matching shader global. Successful later rebinding could
hide that ordering; a null allocation, failed native `Create`, or constructor
exception left no valid replacement to bind. Destruction-time cleanup then had
no old map entry left to inspect.

Test 23 extends the existing ownership helper to this path, before release and
before discarding the old map entry. It clears only that retired image. A global
owned by another camera, or by another eye capture, stays intact. Dead wrappers
and an optional property ID equal to zero are handled without dereferencing the
retired texture. An unchanged allocation takes the same early return as before,
so stable frames do not acquire an extra global read or write. No scene renders,
settings, hooks or shader variants are added.

The existing runner now extracts the actual `EnsureEyeTexture` method too.
The original 90 cleanup checks plus reallocation cases pass 636 host checks
under ASan/UBSan. Test 22 fails 74 checks in the extended suite. Cases cover
color/depth, mono/left/right captures, another owner/eye, unchanged storage,
null allocation, failed creation and constructor exceptions. Descriptor and
allocation API stand-ins supply controllable failures; this does not emulate the
Quest graphics driver or measure Unity's own automatic unbinding behavior.

```sh
python3 tools/separate-eye/texture_ownership_test.py --baseline e84adea2 --baseline-kind resize
```

This verifies explicit ownership and release ordering, not the cause of a saved
screenshot or a guarantee of PC parity. Both Android builds passed, and restoring
diagnostics OFF reproduced the initial normal library bytes. Upstream 0.15.11's
geometry, blit, HUD and guard implementations are unchanged; the Test 22 live
shader fix remains. Shader conversion is unchanged (epoch 26), so Test 22's
18-map native shader-link evidence still applies. No headset command, setting
change, other-mod edit or song edit was made. Test 23 still needs a device run.
