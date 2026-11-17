# Isolated triangle tessellation donor validation

This integration was developed from custom checkpoint `cb845252` in an isolated
validation tree, then applied to the renderer after host and Quest tests. The
optional tessellation setting defaults off. Dependency sources are unchanged.

`ShaderConversionOptions::uniformTriangleTessellation` is false by default.
It is accepted only for isolated, explicit single-view scene-eye donors (eye
0 or 1), with geometry retained. Blit donors, the mono authored-camera donor,
native perspective donors and normal multiview builds cannot enable it.

The in-game **Triangle Tessellation (heavy, exp.)** setting also defaults off.
Changing it reselects the song. It requires the existing Separate-Eye Renderer
and a positive read-only Unity tessellation capability query. It can sharply
reduce frame rate, so existing performance settings are preserved.

When enabled, the worker first prepares both scene eyes at new `.tess-scene-*`
paths. A paired proof contains the compiler policy, existing source-cache key,
source content fingerprint, both donor sizes/content fingerprints and the exact
nonzero covered shader names/count. Both files are committed before the proof
is published; interrupted or ineligible pairs never select one tessellated eye.
An unsupported source, including Lifelike, uses the existing normal donors and
keeps Play available. A declined Unity donor load/preflight retries normal
donors once in the menu without changing the saved setting. Default off keeps
the existing v9 paths and preparation behavior.

For a pass with hull/domain stages, all five stages must have one exact match
for the same pass/list, hardware tier and keyword set after stereo remapping.
Every eligible pair uses the existing narrow compiler and the actual hull
parameter table. Vertex-to-hull and domain-to-geometry signatures must match.
The domain compiler accepts no resources or constants; the hull factor must
be an unambiguous scalar float at its reflected buffer byte address. Unsupported
pairs reject the shader and requested output atomically. No geometry-only
approximation is emitted for an opt-in failure. Ordinary conversion with the
option off retains its existing behavior.

The output writes the complete `VERTEX`, `FRAGMENT`, `GEOMETRY`, `HULL` and
`DOMAIN` source to all five participating GLES3.1AEP program references. The
original parameter tables are preserved. The new counter
`tessellationShadersTranslated` describes successfully translated selected
variants. Source-stage classification remains separate: skipped instancing
variants can still retain original DirectX HS/DS entries in the program store.
`ScanShaders::tessellationShaderNames` alone must not be treated as an output
coverage verdict. The runtime retains that source classification and tracks
the successful paired scene-eye coverage separately. Its untranslated scene-eye
count subtracts only those exact proven names after Unity preflight completes.
Read-only diagnostics separately report source, active scene-eye and remaining
scene-eye counts; authored mono-camera and blit coverage remains untranslated.

## Native Unity evidence

Inspected the supplied Beat Saber player
`work/projector-investigation/libunity.so`, SHA-256
`1dbc5f0efc078cc9c7e9bb8eadbea4b81ed9bcc66098c6ba45a1cbe9c17b80ef`.
The native player was inspected without modification. The runtime checks below
used the installed game on a Quest 3.

`GlslGpuProgramGLES::Create` at `0x13e9854` constructs ASCII `HULL` using
immediates at `0x13e9c0c..0x13e9c18`, then calls `glsl::ExtractDefineBlock`
at `0x13e9c50`. It constructs `DOMAIN` at `0x13e9cb0..0x13e9cc4` and extracts
that block at `0x13e9d0c`. Their absence from a printable-string search is not
evidence that Unity ignores these stages.

Independent parent-agent native analysis also found HULL/DOMAIN map to tess
control/evaluation shader enums, patch count is read from linked-program
`GL_TESS_CONTROL_OUTPUT_VERTICES`, and the draw path selects `GL_PATCHES` while
the stage-presence flag is set. These static observations motivated the separate runtime validation below.

## Reproduce

Build without an Android or Unity SDK:

```sh
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include \
  tools/bundleconvert/main.cpp src/VivifyBundleConvert.cpp \
  src/VivifySerializedFile.cpp src/VivifyDxbc.cpp \
  src/VivifyTriangleTessellation.cpp -o /tmp/vivify-tess-conv-asan
VIVIFY_CONV=/tmp/vivify-tess-conv-asan python3 tools/bundleconvert/run_tests.py
python3 tools/bundleconvert/tessellation_integration_tests.py \
  /tmp/vivify-tess-conv-asan /path/to/yoi/bundleWindows2021.vivify \
  /path/to/lifelike/bundleWindows2021.vivify validation \
  /path/to/tessellation-research/driver-pipelines
```

The integration test uses installed `UnityPy` and `lz4` for independent
serialization inspection/mutation, and `glslangValidator` for links. It never
starts Unity, draws or changes a song file. This workspace's Python environment
is available through `PYTHONPATH=../unity-analysis-env/lib/python3.9/site-packages`.

The standalone CLI opts in explicitly:

```sh
VIVIFY_SKIP_UNUSED=1 VIVIFY_EXPOSE_RENDER_DEPENDENCIES=1 \
  /tmp/vivify-tess-conv-asan --shaders-tess-eye-left-isolated SOURCE OUTPUT
```

The corresponding right-eye switch is `--shaders-tess-eye-right-isolated`.

Host results saved in `validation/`: 151 existing converter checks and 81
integration checks passed, including four converted five-stage links, six
previously saved five-stage links, exact stage-reference retagging, preserved
factor metadata, and atomic rejection of missing DS, different hardware tier,
ambiguous HS, wrong factor address, disallowed geometry-drop modes and Lifelike.
Yoi's source hash was unchanged. Another 37 sanitizer-backed pair-cache tests
exercise atomic commits, interrupted/one-eye failures, unknown coverage,
source/content changes, policy/name mismatches, provider exceptions, failed
proof writes and safe default-off behavior. Linux CI additionally uses
`/dev/full` to exercise an actual ENOSPC flush failure without consuming storage;
the macOS run injects a failing proof stream through an unwritable target type.

The parent task's isolated Quest validation successfully selected the donor
programs and recorded dense grass in a bounded Yoi run, with both eyes active.
The integrated setting/cache path also loaded the paired scene-eye programs,
passed Unity material preflight and unused-asset cleanup, and rendered dense
grass in both eyes during a bounded NoFail Yoi run. Switching to Lifelike
rejected the unsupported pair atomically and completed normal preflight with
Play available; switching back reused the validated Yoi pair. Both paths had
zero unexpected material/donor pass failures and no lost assets after cleanup.
Default-off conversion
produced byte-identical normal scene-eye output for Yoi and Lifelike compared
with checkpoint `cb845252`.

Matched diagnostic runs without screenshot readback measured median Unity
frame time of approximately 31.8 ms with Yoi tessellation versus 19.2 ms without
it (25 and 24 samples after ten seconds). These are diagnostic samples rather
than production GPU benchmarks. The authored uniform factor was preserved;
no adaptive quality or device setting was changed. This is a supported runtime
example, not complete Windows parity or native multiview performance. Adaptive
Lifelike tessellation, authored-camera/blit tessellation, complete depth/shadow
coverage and tracked-eye visual parity remain limitations or further tests.
