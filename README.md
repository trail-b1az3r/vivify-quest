# Vivify Quest

A Quest port of [Vivify](https://github.com/Aeroluna/Vivify) for Beat Saber — the
mapping mod that lets custom levels run their own shaders, cameras, and
post-processing effects ("modcharts").

This project is built on top of three earlier community ports rather than
written from scratch. See [Credits](#credits).

**Status:** confirmed building and running on-device as of this fork. The
Windows→Android bundle converter and shader translator are implemented but
need testing against a wide range of real maps — see
[Known Issues / Needs Testing](#known-issues--needs-testing).

## What this includes

- Real per-`CameraEvent` render ordering (`Blit` effects fire in the same
  order PC Vivify uses, instead of always drawing after gameplay).
- Legacy custom-event aliases (`PostProcess`, `PostProcessing`, `ScreenEffect`)
  for maps authored before the community standardized on `Blit`.
- On-device conversion of PC ("Windows") asset bundles to Android, including:
  - BC1/BC3/BC7 texture decoding (Quest GPUs can't sample these directly)
  - A DXBC → GLSL ES shader translator for a defined subset of Shader Model 4/5
  - Material/shader repair for bundles whose shaders can't run on this GPU
- Per-level and full-session diagnostic reports written to
  `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/`
- A frame-time watchdog that disables Vivify for a level rather than freezing
  the game if something goes badly wrong mid-song.

## Known Issues / Needs Testing

The shader translator and bundle converter have unit test coverage on desktop
but have only recently been confirmed to compile and run on real hardware.
**They have not yet been validated against a broad set of real Vivify maps.**
If you test this mod, please report:

- Which maps you tried (and whether they shipped a PC-only bundle or an
  Android bundle)
- Anything in `VivifyReport.txt` that says shaders were skipped, refused, or
  fell back to a stand-in
- Any crash, freeze, or visibly wrong effect

See [Issues](../../issues) for the current list of open problems, and please
open a new one using the bug report template if you hit something not
already listed.

## Building

Requirements:
- [QPM.CLI](https://github.com/QuestPackageManager/QPM.CLI)
- Android NDK — this project targets NDK `27.3.x` specifically; newer
  versions (28+) are known to break compilation against the Beat Saber
  modding libraries this depends on.
- CMake + Ninja
- PowerShell 7 (`pwsh`) — used by this project's build scripts

```
qpm restore
cmake -G Ninja -B build
cmake --build build
pwsh ./scripts/createqmod.ps1
```

This produces `Vivify.qmod` in the project root, installable via
[ModsBeforeFriday](https://mbf.bsquest.xyz) or QuestPatcher.

## Versioning

This project follows [Semantic Versioning](https://semver.org/):
`MAJOR.MINOR.PATCH`.

- **MAJOR** — breaking changes to how maps interact with this mod, or a
  rewrite of a core system (e.g. the render pipeline).
- **MINOR** — new features that don't break existing maps (e.g. a newly
  supported event type, or the bundle converter gaining a new capability).
- **PATCH** — bug fixes only.

The version lives in `mod.json` and `qpm.json` and should be bumped in the
same commit/PR as the change it corresponds to. See `CHANGELOG.md` for the
running history.

## Credits

- [Aeroluna](https://github.com/Aeroluna) — original PC/PCVR Vivify.
- axo-lotl ([Gay-Axolotl](https://github.com/Gay-Axolotl) on GitHub) —
  base architecture for this port.
- Braxed ([rbatteries1-design](https://github.com/rbatteries1-design)) and
  Lars27110 — additional fixes merged in from their forks.
