# License copies and third-party notices

The text files in this directory are verbatim copies of license or notice files from the project and its checked-out dependencies. Original wording, attribution, copyright notices, and license terms are preserved.

## Included copies

| Component | Copied file | Original source path |
|---|---|---|
| Vivify Quest | `VivifyQuest-LICENSE.txt` | `LICENSE` |
| Converter | `converter-LICENSE.txt` | `converter/LICENSE` |
| Depth assets/shader | `depth-assets-LICENSE.txt` | `assets/depth/LICENSE` |
| Beat Saber Hook | `beatsaber-hook-LICENSE.txt` | `extern/includes/beatsaber-hook/LICENSE` |
| RapidJSON | `RapidJSON-license.txt` | `extern/includes/beatsaber-hook/shared/rapidjson/license.txt` |
| RapidJSON Natvis | `rapidjson-natvis-LICENSE.txt` | `extern/includes/beatsaber-hook/shared/rapidjson/contrib/natvis/LICENSE` |
| config-utils | `config-utils-GPL-3.0.txt` | `extern/includes/config-utils/LICENSE` |
| cpp-semver | `cpp-semver-LICENSE.txt` | `extern/includes/cpp-semver/LICENSE` |
| CustomJSONData | `CustomJSONData-LICENSE.txt` | `extern/includes/custom-json-data/LICENSE` |
| CustomTypes / Il2CppQuestTypePatching | `Il2CppQuestTypePatching-LICENSE.txt` | `extern/includes/custom-types/LICENSE` |
| fmt | `fmt-MIT.txt` | `extern/includes/fmt/LICENSE` |
| libatomic_ops (BDWGC) | `libatomic_ops-COPYING.txt` | `extern/includes/libil2cpp/external/bdwgc/libatomic_ops/COPYING` |
| utf8cpp | `utfcpp-Boost-1.0.txt` | `extern/includes/paper2_scotland2/shared/utfcpp/LICENSE` |
| Scotland2 | `scotland2-LICENSE.txt` | `extern/includes/scotland2/LICENSE` |
| Sombrero | `Sombrero-LGPL-2.1.txt` | `extern/includes/sombrero/LICENSE` |
| SongCore | `SongCore-LICENSE.txt` | `extern/includes/songcore/LICENSE` |
| tinyxml2 | `tinyxml2-Zlib.txt` | `extern/includes/tinyxml2/LICENSE` |
| Flamingo | `Flamingo-LICENSE.txt` | Upstream `sc2ad/Flamingo`, pinned ref `version/v1_1_2` |
| BeatLeader replay-format code | `beatleader-qmod-MIT.txt` | Original BeatLeader Quest mod MIT notice |

## Dependencies without a standalone license file found

The following packages are present in the dependency manifest or bundled libraries, but no standalone license file was found at the checked source path/ref during this pass: bs-cordl, Quest-BSML, conditional-dependencies, MetaCore, paperlog, RapidJSONMacros, Tracks, WebUtils, and the Unity IL2CPP portions outside the included libatomic_ops component.

This does **not** mean those components are public domain or license-free. Their licensing and redistribution terms still need to be verified from upstream package metadata, source headers, or the applicable Unity/third-party terms before redistribution. Do not replace a missing upstream license with a guessed license.

## Maintenance

When dependencies change, update these copies from the exact source revision restored by CI. Keep the license text unmodified and add every license/notice for source, headers, submodules, generated or ported code, assets, shaders, and bundled tools. The component/version/ref and whether source or binaries are redistributed should be tracked alongside future additions.
