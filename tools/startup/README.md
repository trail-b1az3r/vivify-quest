# Embedded ARM64 hook relocation and chaining

This changes only Vivify's embedded And64 hook installer. Other mods' binaries,
SDK dependencies, headset settings and game files are not rewritten.

The original relocator mixed signed displacements with unsigned masks in five
instruction families: literal loads, ADR, ADRP, conditional branches and test
branches. Backward targets could resolve to unrelated memory. In addition, a
previous hook's literal entry could put pointer data inside the next installer's
instruction-copy window.

`cmake/FixArm64Relocator.cmake` generates a reviewed SDK source copy at build
time. The input SHA-256 is pinned; a changed dependency stops configuration.
The generated source uses the pure signed-offset decoders in
`VivifyArm64Relocation.hpp`. It preserves original ADRP page addresses and
avoids negative signed shifts when encoding relocated displacements.

`VivifyArm64HookStub.hpp` recognizes the supported existing LDR/BR/pointer
entries and preserves their destination before publication. New entries use
code-only MOVZ/MOVK/MOVK/BR stubs with NOP padding where needed. Unsupported
pointer widths, malformed recognized entries, immediate recursion and duplicate
targets refuse installation. This does not fix a later legacy-to-legacy hook
chain installed by other mods.

On Beat Saber 1.40.8, setup also uses custom-types' existing process-local
`CT_DISABLE_LIVENESS_CHECKS` switch. The saved failure and matching game layout
identified the optional legacy debug traversal hook's filter-class offset as
incompatible. The switch bypasses those debug checks; original Unity traversal,
garbage collection and custom-type registration remain active. Existing
environment values are preserved, and the switch ends with the app process.
It is excluded from the 1.45.3 game target.

## Validation

```sh
python3 tools/startup/run_integration.py
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I include tools/startup/relocation_test.cpp -o /tmp/relocation-test
/tmp/relocation-test
```

The integration suite calls the actual generated relocator against ordinary
buffers and verifies instruction targets and embedded literal bytes. It also
exercises the installer with memory protection intercepted, including entry
publication ordering and refusal paths. No generated instructions execute and
no executable memory is requested. A changed SDK source is rejected.

These checks do not establish that every hook chain is safe. Intermittent
startup crashes remain unresolved and are not attributed to one mod by these
host tests or the small device launch samples.
