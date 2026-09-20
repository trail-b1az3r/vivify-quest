# Contributing

## Reporting bugs

Please use the bug report issue template and include the contents of
`VivifyReport.txt` (per-level) and, if the game crashed, `VivifySession.txt` —
both live at `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/` on the
headset. Reports without this diagnostic info are much harder to act on.

## Making changes

1. One logical change per pull request — makes it possible to actually
   bisect if something regresses.
2. Bump the version in `mod.json` and `qpm.json` per [Semantic
   Versioning](https://semver.org/) — see the Versioning section in
   `README.md`.
3. Add an entry to `CHANGELOG.md` under `[Unreleased]`.
4. If you're touching the bundle converter or shader translator, note in the
   PR description whether you've tested it against a real converted map, or
   only the desktop unit tests — both are useful information, but they mean
   different things.

## Testing the bundle converter without a headset

`src/VivifyBundleConvert.cpp` has no Unity/il2cpp dependency and builds as
plain C++20:

```
g++ -std=c++20 -O1 -g -fsanitize=address,undefined \
    -I include -o /tmp/conv tools/bundleconvert/main.cpp src/VivifyBundleConvert.cpp
python3 tools/bundleconvert/run_tests.py
python3 tools/bundleconvert/fuzz.py
```

This is a fast way to check changes to the converter without a full
Quest build-and-deploy cycle.
