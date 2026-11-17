# Live material refresh regression

The original material is the authority for animated values and current shader
selection. A helper copy must use the donor corresponding to that shader.
Unity's `Material.CopyPropertiesFromMaterial` copies values, keywords, render
queue and global illumination flags, but explicitly leaves the shader intact:
https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Material.CopyPropertiesFromMaterial.html

Test 17 refreshed ordinary companion properties on each use, but only set the
companion shader when constructing the copy. Reassigning the source shader to
another converted shader therefore reused the old companion indefinitely.
The explicit-eye material cache also returned a same-epoch copy before
checking whether its donor changed. Its blit copies additionally missed
property changes between uses in that epoch.

The corrected paths retain the helper object's identity and lifetime flags,
select the current companion/donor, and refresh screen-effect properties on
every use. Scene materials still share one preparation snapshot while the
donor is unchanged; a new donor invalidates that snapshot. Unchanged shaders
are not reassigned. This adds no scene renders, conversion passes or new
per-blit material allocations. More property copies are required when the
same screen-effect material is used repeatedly, so Quest timing still needs
measurement rather than an assumption of zero CPU cost.

Run the host regression with:

```sh
python3 tools/separate-eye/material_refresh_test.py
```

With the pre-fix checkpoint available, also prove the regression:

```sh
python3 tools/separate-eye/material_refresh_test.py --baseline 6eb054f4
```

The runner extracts the unmodified production `CopyMaterial`,
`Runtime::BlitMaterialFor` and `CanUseNativeMaterial` bodies plus the actual native
policy header into a temporary translation unit. It tests
those actual methods under ASan/UBSan using small API stand-ins with the
documented property-copy behavior. It does not start Unity, connect to a
headset, draw shaders, or establish visual or GPU performance parity.

The existing menu-only disposable-material fixture additionally perturbs its
own helper shader, then verifies the current companion is restored without
changing an asset or drawing. That added device check has not yet run on the
Quest. The previously validated Test 17 installation remains separate from
this offline candidate.

## Upstream geometry preference (Test 21)

The routing regression now also covers upstream 0.15.11 native geometry.
Supported geometry no longer forces separate scene draws just because it has a
geometry stage. Only an active validated optional scene-eye tessellation pair
requires that route on stage grounds. Unsupported native shaders, changed shader
identity, incomplete metadata and unknown provenance still decline native reuse.
Declined or disabled tessellation uses the available upstream native fallback;
this does not make unsupported hull/domain stages equivalent to PC.

All 47 host checks pass. Test 20 fails the two new upstream-preference cases:

```sh
python3 tools/separate-eye/material_refresh_test.py --baseline b47893d4 --baseline-kind native
```

These stand-ins verify material selection decisions, not driver geometry
routing, actual draw coverage or performance. The older live-material baseline
can still be checked with the default `--baseline-kind material`.

## Same-epoch native permission (Test 22)

The [native identity follow-up](NATIVE_IDENTITY.md) adds three checks to the
47-check suite. Changing or removing a known material's shader must invalidate
its cached native permission even without advancing the capture epoch; restoring
the captured shader can reuse the valid permission. All 50 checks pass. Test 21
fails the changed/removed cases. No device result or visual causality is implied.
