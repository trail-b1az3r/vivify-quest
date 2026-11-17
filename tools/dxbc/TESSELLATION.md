# Triangle tessellation compiler prototype

Status: the narrow uniform-triangle pattern now has an **optional, default-off
scene-eye integration**. The `Triangle Tessellation (heavy, exp.)` setting uses
the existing Separate-Eye Renderer and keeps normal donors when a whole
two-eye pair cannot be proved. General tessellation remains unsupported.
See [integration and validation](TESSELLATION_INTEGRATION_VALIDATION.md).

The parent task's isolated Quest donor test recorded dense Yoi grass in both
eyes. That does not establish Windows visual parity, comfortable performance,
or coverage of authored mono cameras and screen effects. Lifelike's general
hull phases are still rejected. Test 17 subsequently validated the setting,
paired cache and unsupported-map fallback on Quest, with tessellation off by
default. The sections below preserve the earlier prototype evidence; see the
integration document for the later runtime results.

## Verified on 2026-11-11

* Quest 3 / Beat Saber 1.40.8: Unity's `SupportsTessellationShaders` returned
  `true` in a menu-only diagnostic query.
* Adreno 740, OpenGL ES 3.2: all six saved Yoi grass pipelines compiled and
  linked with vertex, tessellation control, tessellation evaluation, geometry
  and fragment stages in the existing offscreen shader probe.
* All eight actual Yoi hull/domain reference pairs translated. Factor names
  and byte addresses came from each hull program's Unity parameter table.
* All twelve actual Lifelike pairs were rejected. Its hull shader computes
  distance/edge-dependent factors with fork/join phases, rather than Yoi's
  simpler uniform factor. It needs a more general hull-phase compiler.
* 613 prototype checks passed, including complete synthetic mono/stereo
  five-stage links, reflection/address rejection, register-write ordering,
  unsupported operations and 250 malformed pairs. The existing 235 DXBC
  checks still passed. The Android library and extended existing driver
  probe built successfully.

Driver linking is not a draw test or proof of Unity selection, correct stereo
images, visual equivalence, or performance. The combined multiview/geometry/
tessellation extension is absent on this device. The verified sources are
single-view pipelines; this does not establish native multiview tessellation.

## Deliberately narrow API

`TranslateTriangleTessellation(hull, domain, actualHullReflection)` lives in
`VivifyTriangleTessellation.cpp`. It requires parsed shader-model-5 programs,
three unchanged control points, triangular/integer/clockwise layout, and the
two uniform-factor fork phases actually observed in Yoi. It verifies both
control-point interfaces, patch signatures, declaration payloads, dynamic
factor indexing, and agreement between all outer/inner factor reads.

The factor must resolve to exactly one reflected scalar float at the actual
constant-buffer byte address; no shader/material-name guess is permitted.
The control shader preserves every declared control-point varying, including
the opaque instance-ID payload used by the stereo variant. The evaluation
shader translates MOV/MUL/MAD arithmetic and explicit source swizzles. Scalar
SSA writes preserve simultaneous vector-register updates and bound generated
source size. Undeclared inputs, undefined temporaries/outputs, unsupported
resources/operations/flags and different phase structures fail closed.

The original DXBC and song files are untouched. This API emits separate GLSL
stage sources, not an installable tessellation-enabled map or QMOD.

## Reproduce the SDK-free checks

```sh
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include \
  tools/dxbc/main.cpp src/VivifyDxbc.cpp src/VivifyTriangleTessellation.cpp \
  -o /tmp/dxbctool
python3 tools/dxbc/run_tests.py /tmp/dxbctool
python3 tools/dxbc/tessellation_tests.py /tmp/dxbctool
```

The prototype tests require `glslangValidator` on PATH. The existing DXBC CLI
also accepts `tess-pair HULL DOMAIN OUTPUT_PREFIX`, using RDEF if present.
For stripped bytecode, optional `BIND BYTE_OFFSET SCALAR_NAME` arguments supply
external reflection explicitly. They must be obtained from the real hull
program's parameter table; supplying a matching address alone does not prove
an arbitrary caller-provided property name correct.

The existing device probe's `--link-list` mode accepts optional `.tesc`/`.tese`
files as a mandatory pair, alongside its existing vertex/fragment/geometry
files. It does not draw these map pipelines or modify Beat Saber.

## Integration and remaining coverage

1. The optional converter now matches all five stages by the same pass/keywords/hardware variant, including
   every declared instance-ID varying. Adreno rejected an initial pairing
   missing that varying even though the reference linker accepted it after
   optimization. The corrected six pipelines passed on Adreno.
2. HULL/DOMAIN source blocks and all five GLES references are replaced
   atomically, retaining actual parameter bindings. Unsupported opt-in inputs
   do not emit a partial donor; runtime preparation keeps the existing pair.
3. An isolated Unity selection/draw succeeded for Yoi. Full generated
   positions/factors/coverage comparisons, moving-head stereo and performance
   validation remain necessary before visual-parity or speed claims.
4. Supply per-camera uniforms in mono and explicit-eye routes, and validate
   depth/shadow passes, transitions, pause and cleanup in Unity.
5. Implement general phase-local register spaces, fork instance expansion,
   dynamic control-point reads and join-phase patch-constant reads for
   Lifelike. Preserve its camera-dependent factors.

## Historical device restoration before Test 17

The temporary capability build was removed. The installed shared/private
library hashes both match the normal Test 13 QMOD:
`7536f7cd683f7aae799ec92e6ef832a96287212d338af58ef37f0dad0aea0d2f`.
Beat Saber was left running at the menu. The earlier diagnostic-only
score-submission preference was restored to `true`; other existing preferences
were preserved. No Quest system settings or song files changed. Temporary
offscreen probe files were removed.

Two initial launches exited during native mod initialization (one on the
previous Test 9 binary, one on normal Test 13); retries reached the menu. Saved
crash traces name bstheater/chroma initialization frames. That identifies where
the crashes occurred, not their root cause; no unrelated mods were changed.

## Format references

* [Khronos GLSL ES 3.20 specification](https://registry.khronos.org/OpenGL/specs/es/3.2/GLSL_ES_Specification_3.20.html),
  tessellation layouts, interfaces and built-in variables.
* [Microsoft hull shader register model](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/registers---hs-5-0),
  control-point, fork and join register spaces.
* [Microsoft partitioning values](https://learn.microsoft.com/en-us/windows/desktop/api/d3dcommon/ne-d3dcommon-d3d_tessellator_partitioning),
  integer versus fractional partitioning.
