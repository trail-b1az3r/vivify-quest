# Vivify Quest converter (library, CLI, Unity Editor)

The asset-bundle converter from the Vivify Quest mod, packaged so other
projects can use it. It turns a PC Vivify bundle (`bundleWindows2021.vivify` or
`bundleWindows2019.vivify`) into `bundleAndroid2021.vivify`. The mod loads that
file from a map's folder before anything else, so a map that ships it needs no
conversion on the headset.

What it does to the bundle:

- retargets it to Android
- translates every shader's Direct3D 11 bytecode to GLSL ES 3, with OpenGL ES
  multiview for single-pass stereo
- translates geometry shaders, including the right-eye copy

It compiles the mod's own sources (`../src/VivifyBundleConvert.cpp`,
`VivifySerializedFile.cpp`, `VivifyDxbc.cpp`), so it converts exactly as the mod
does at the same converter revision. The mod's build does not use this folder and
is unchanged by it.

## Unity Editor

1. Get `VivifyQuestConverter-unity-package` from the **Converter library**
   workflow's artifacts. It contains this `unity/` folder, plus the native
   libraries for Windows, Linux and macOS under `Editor/Plugins`.
2. Install it with **Package Manager > + > Add package from disk...**
   (`package.json`), or copy the folder into `Assets/`.
3. Open **Vivify > Quest Converter**, or use
   **Vivify > Convert Bundle For Quest...** for a one-off conversion.

Plugins in a folder named `Editor` load in the Editor only, so nothing is added
to a player build.

From a build script, after `BuildPipeline.BuildAssetBundles`:

```csharp
using VivifyQuest.Converter;

var result = VivifyQuestConverter.ConvertForQuest("Build/bundleWindows2021.vivify");
if (!result.Succeeded) Debug.LogError(result.message);
// Build/bundleAndroid2021.vivify now sits beside the PC bundle.
```

`VivifyQuestConverter.Convert(source, dest, options)` takes
`VivifyQuestConverter.Options` (start from `Options.Default`). It returns
`Result`, which holds:

- `status`
- `counts`: shaders and variants translated or refused, and output size
- `message`: the converter's summary, including every shader it left alone and why

`Describe(path)` reports what a bundle's shaders were compiled for.

The mod does not check a local `bundleAndroid2021.vivify` against Info.dat's
`android2021` checksum. You can leave that field out, or keep the one Unity
produced.

## Command line

```
vivify-convert [options] <source bundle> <destination>
vivify-convert --describe <bundle>
vivify-convert --version
```

| Option | Effect |
| --- | --- |
| `--retarget-only` | retarget to Android only; shaders stay as they are |
| `--no-right-eye-copy` | geometry stages draw the left eye only |
| `--flat-geometry` | draw geometry-shader variants without that stage |
| `--drop-bad-geometry` | the same, only where the stage cannot be translated |
| `--keep-unused-variants` | also translate GPU-instancing and unsplit stereo variants |
| `--single-view` | single-view programs only |
| `--split-stereo`, `--split-2019` | separate single-view and multiview variants |
| `--rename-files` | new archive file names |

The defaults are what the mod uses on a map.

## C / C++ and other languages

`include/vivify_convert.h` is a plain C API: no global state, UTF-8 paths, and
thread-safe calls. Any language with a C FFI can call it.

```c
VivifyConvertOptions options;
vivify_convert_default_options(&options);
VivifyConvertResult result = { sizeof result };
char message[4096];
int status = vivify_convert_bundle("bundleWindows2021.vivify", "bundleAndroid2021.vivify",
                                   &options, &result, message, sizeof message);
```

Both structs start with `struct_size`. The library reads and writes only as many
bytes as the caller's `struct_size` says, so a program built against an older
header keeps working with a newer library.

## Building

```
cmake -S converter -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release        # C API smoke test (needs Python 3)
```

| Output | Contents |
| --- | --- |
| `vivifyconvert` | shared library: `vivifyconvert.dll`, `libvivifyconvert.so` or `libvivifyconvert.dylib` |
| `vivifyconvert_static` | static library; define `VIVIFY_CONVERT_STATIC` when you use it (CMake does this for you) |
| `vivify-convert` | command-line tool |

Optimisation:

- Release builds use `-O3` (or `/O2` with MSVC).
- The shared library and the tool also use link-time optimisation.
- The static library is built without LTO, so any compiler can link it.
- On Windows (MinGW) and Linux, the shared library links its C++ runtime
  statically and exports only the `vivify_convert_*` functions, so there are no
  other libraries to ship beside it.

To embed it in another CMake project:

```cmake
add_subdirectory(path/to/vivify-quest/converter vivify-convert)
target_link_libraries(my_tool PRIVATE vivifyconvert_static)
```

Cross-compiling the Windows DLL from Linux:

```
cmake -S converter -B build-win -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DBUILD_TESTING=OFF
```

## Converter revision

`vivify_convert_revision()` (`--version`; `VivifyQuestConverter.Revision()`) is
the mod's conversion version. A bundle the mod converted at an older revision
is converted again on the headset. A `bundleAndroid2021.vivify` shipped with a
map is used as it is, so convert it again after updating the converter.
