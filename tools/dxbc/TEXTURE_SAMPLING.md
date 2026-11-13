# Texture sampling fixes and offline validation

Test 19 includes Test 18's live material refresh work and corrects four shader
translation cases:

- Biased sampling with a constant texel offset placed bias before the offset.
  Core GLSL requires offset before bias; explicit LOD uses the opposite order.
- Comparison gathers with offsets placed the comparison after the offset.
  Core GLSL requires comparison before offset; non-comparison gathers place
  their selected channel after the offset.
- `sample_c_lz` became an ordinary implicit-LOD sample. For a mipmapped texture
  in a fragment shader, this could select a different mip as derivatives change.
  The corrected 2D shadow case uses explicit LOD zero. Array/cube shadow cases
  use the core zero-gradient form without requesting a shadow-LOD extension.
- Implicit array-shadow sampling with offsets emitted a nonexistent core
  overload. It now uses the gradient-offset overload, with fragment coordinate
  derivatives or base-image gradients outside fragment programs.

Comparison instructions also require their actual comparison operand. Ordinary
implicit comparison sampling remains implicit. Array layers do not acquire a
texel offset or become a derivative dimension.

The authoritative overloads and gradient rules are in
[GLSL ES 3.20, section 8.9](https://registry.khronos.org/OpenGL/specs/es/3.2/GLSL_ES_Specification_3.20.html#texture-functions).
Microsoft specifies that
[`sample_c_lz`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-c-lz--sm4---asm-)
uses LOD zero and ignores derivatives.

## Cache behavior

`BundleConvert::kShaderConversionVersion` is now the shared version authority
for ordinary conversion and its companion markers, six explicit-eye donor
modes, and the optional tessellation pair's context key. Version 25 invalidates
previous translated output on selection. No song file or original PC bundle
is rewritten. The first preparation after upgrading may reconvert shaders.

## Host evidence

The extended existing DXBC suite passes 264 checks with ASan/UBSan, including
reference compilation of every successful generated program. The pre-fix
translator fails 19 of these checks, including nine reference-compiler
rejections. This covers 2D, array and volume offsets; shadow gathers; implicit
versus level-zero sampling; and vertex-stage cases without invalid derivatives.
The 151 bundle checks and 613 tessellation prototype checks also pass.

A read-only audit of the 18 locally saved installed-map PC bundles parsed
11,878 stored DXBC programs. Fifteen bundles contained fragment level-zero
comparisons (2,185 instruction occurrences, including repeated variants).
Through the Screen contains them in `AreaLit/Standard` and `Standard`.
The other three corrected offset cases did not occur in those stored programs;
their regressions use assembled bytecode fixtures.

These are stored-program counts, not evidence of selected runtime variants or
affected on-screen pixels. Many copies belong to inactive shadow variants,
and an un-mipmapped texture could already select the same image. This audit
does not identify the cause of a reported screenshot or establish PC parity.
All source-bundle hashes were unchanged after inspection.

Both Android build configurations and actual-map pipeline-link results are
recorded with the separately packaged offline candidate. No Quest operation
or device validation was performed for Test 19. The extended existing menu
fixture and known problem scenes remain the next device checks.
