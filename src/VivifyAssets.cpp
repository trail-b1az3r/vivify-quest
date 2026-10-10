#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"
#include "VivifyBundleConvert.hpp"
#include "VivifyTextureDecode.hpp"
#include "VivifyReport.hpp"
#include "VivifyZip.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/AssetBundleRequest.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Renderer.hpp"
#include "UnityEngine/Material.hpp"
#include <set>
#include "UnityEngine/TextureFormat.hpp"
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <condition_variable>
#include <sys/resource.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace Vivify {

namespace {

// What conversion produces for a given input. Bumped whenever the converter
// starts writing a different bundle from the same source.
//
// A converted bundle is cached on the headset and reused forever, keyed on the
// source file. Without this, the shader-translating conversion would never run
// for any map already converted by an earlier build: the cache would answer
// first, with a bundle whose shaders are still DirectX, and the fix would look
// like it had done nothing until someone found the reconvert button.
//
//   1  retarget only (every build up to 0.9.6)
//   2  retarget plus DirectX -> GLSL ES shader translation
//   3  the same, but actually finding the DXBC: version 2 looked for the
//      container at offset zero, where Unity's own program header sits, so it
//      translated nothing at all and its caches are worth no more than a
//      version 1 one
//   4  the same, plus marking block-compressed textures readable so their
//      pixels survive load and can be decoded on device. A version 3 cache has
//      textures whose CPU copy Unity drops, which is a level that renders
//      untextured
//   5  shaders converted through m_ParsedForm: every variant Unity selects is
//      relabelled as a GLES program, vertex and fragment are linked into one
//      program the way Unity stores GLES variants, and everything is emitted
//      for the Quest's single-pass multiview eye buffer. A version 4 cache left
//      m_ParsedForm calling every program Direct3D 11, so Unity found no program
//      it could run in any converted shader (isSupported = false throughout),
//      and its programs could not have drawn into a multiview framebuffer if it
//      had
//   6  the same, translated with the reflection Unity keeps in m_ParsedForm.
//      Built bundles have no RDEF in their DXBC, so version 5 translated no
//      shader at all ("reads constant buffer b0, which its reflection data does
//      not describe" for all 355 in one session) and its caches are no better
//      than version 4's
//   7  the same, with the four-byte padding Unity expects after a program's
//      code. Unity 2019 bundles keep each program's parameter tables right
//      after its code; version 6 wrote the translated GLSL without that padding,
//      Unity read the tables from the wrong offset, and the game crashed as
//      soon as such a level was selected
//   8  the same, with GPU instancing's per-instance arrays sized at load time
//      (UNITY_RUNTIME_INSTANCING_ARRAY_SIZE) instead of at Unity's
//      placeholder of 2. Version 7 gave every instance after the second in a
//      batch garbage transforms: chords and chains were drawn in the wrong
//      places and pointing the wrong way
//   9  the same, reading the per-eye camera matrices from the UnityStereoGlobals
//      uniform block that multiview fills. Version 8 read them as loose
//      uniforms nothing keeps current, and everything a translated shader drew
//      (scenery, custom notes) landed in the wrong place
//  10  separate single-view and multiview programs (see
//      ShaderConversionOptions::separateStereoVariants) when the game uses
//      STEREO_MULTIVIEW_ON. Version 9 put multiview programs in the plain
//      variants, which GL refuses in any single-view framebuffer: blits,
//      render textures and secondary cameras on converted maps flickered or
//      showed garbage. The marker records which way a bundle was converted
//  11  small named cbuffers back to loose uniforms, large ones as uniform
//      blocks with an explicit binding. Version 10 made every named cbuffer a
//      block with no binding, all on binding 0, and custom notes lost their
//      colour
//  12  Unity 2019 bundles no longer split (see ConvertThroughParsedForm).
//      Version 10 and 11 renamed the stereo keyword only in the program
//      entries, which Unity does not select variants by; the eye cameras got
//      the single-view programs and notes and most visuals were invisible
//  13  no stereo split at all: every converted map, 2019 and 2021, gets its
//      stereo twin's multiview code in its plain variants, the layout that
//      has worked on the headset since version 9. The headset never picked a
//      variant renamed to STEREO_MULTIVIEW_ON (versions 10-12 in 2021
//      bundles, 0.14.8 in 2019 ones), so split shaders drew nothing there;
//      0.14.8's merged Hold My Hand kaleidoscope was one. Replay render mode
//      converts single-view instead, into a cache of its own
//  14  a geometry stage that cannot run (multiview forbids one) is dropped and
//      the variant's vertex and fragment linked without it, with the object-
//      to-clip transform added when the geometry stage was the one doing it.
//      Version 13 left such variants on DirectX: grey stand-ins, or nothing
//  15  a shader's own depth output (SV_Depth) is mapped from OpenGL's -1..1
//      clip depth to the 0..1 window depth. Version 14 wrote it unmapped, and
//      raymarchers that write depth drew in front of the whole scene
//  16  back to how 0.14.7 (version 12) converted, which looked best on the
//      headset: 2021 bundles split into single-view and STEREO_MULTIVIEW_ON
//      variants when the game uses that keyword, 2019 bundles not split, no
//      geometry-stage fallback, depth written as it was. Versions 13-15 drew
//      converted maps worse and started songs with longer freezes
//  17  variants a Quest never selects (GPU instancing, which converted
//      materials have turned off, and unsplit single-pass stereo) are left
//      untranslated: in 743Aether 573 of 774 program references, 336 linked
//      variants down to 90. Far less for Unity to load and the driver to
//      compile when a converted map starts
//  18  translated programs keep integers in integer variables (temps are
//      ivec4 bit registers) and copy integer literals exactly. Up to 17 they
//      sat in floats as bit patterns, and the integers 1, 2, 3 are denormals
//      there, which Adreno flushes to zero: loop counters never advanced, the
//      shader looped forever and the GPU hung on a converted map's first
//      frame. -1 (DXBC's "true") was also written 0.0, breaking conditions
//      and loop bounds
//  19  no change to the output: a fresh start for crash-guard strikes. A
//      freeze caused by another mod (Adaptive Audio Latency and others) during
//      a converted map's load counted against the map, and two put it into the
//      grey untranslated fallback for good -- 0.14.23's Yoi Okashi, whose
//      translation compiles and links cleanly
constexpr int kBundleConversionVersion = 19;

// Whether Beat Saber's own shaders use STEREO_MULTIVIEW_ON: -1 not looked yet,
// 0 no, 1 yes. Unity registers every keyword a loaded shader declares, so the
// game's shaders having been compiled with multiview variants shows up in
// Shader.GetAllGlobalKeywords(). Read on the main thread (DetectMultiviewKeyword)
// and used by conversions on worker threads.
std::atomic<int> gMultiviewKeyword{-1};

void DetectMultiviewKeyword() {
  if (gMultiviewKeyword.load() >= 0) return;
  try {
    auto keywords = UnityEngine::Shader::GetAllGlobalKeywords();
    if (!keywords || keywords.size() == 0) return;  // too early; look again later
    bool found = false;
    for (auto const& keyword : keywords) {
      if (keyword.m_Name && std::string(keyword.m_Name) == "STEREO_MULTIVIEW_ON") found = true;
    }
    gMultiviewKeyword.store(found ? 1 : 0);
    PaperLogger.info("Vivify: the game {} STEREO_MULTIVIEW_ON ({} global keywords); converted shaders get {}",
                     found ? "uses" : "does not use", keywords.size(),
                     GetReplayRenderMode() ? "single-view programs only (replay render mode)"
                     : found               ? "separate single-view and multiview programs (2021 bundles)"
                                           : "multiview programs in their plain variants");
  } catch (...) {
    PaperLogger.warn("Vivify: could not list the game's shader keywords; converted shaders keep multiview "
                     "programs in their plain variants");
    gMultiviewKeyword.store(0);
  }
}

// Whether 2021 bundles are split into single-view and multiview variants: when
// the game uses STEREO_MULTIVIEW_ON, as 0.14.7 did.
bool SplitStereoVariants() {
  return gMultiviewKeyword.load() == 1;
}

// What the cache marker records about how a bundle's programs were built:
// 0 = multiview in the plain variants, 1 = 2021 bundles split,
// 4 = single-view only (replay render mode).
int ConversionSplitMode() {
  // +8: geometry-shader effects drawn without their geometry stage (setting).
  int const geometry = GetDrawGeometryShaderEffects() ? 8 : 0;
  if (GetReplayRenderMode()) return 4 + geometry;
  return (SplitStereoVariants() ? 1 : 0) + geometry;
}

std::string ConversionMarkerPath(std::string const& destPath) {
  return destPath + ".version";
}

// A cached conversion counts only if it was produced by this converter, the
// same way this session would produce it. A bundle with no marker beside it
// came from a build that predates them.
bool CachedConversionIsCurrent(std::string const& destPath) {
  std::error_code ec;
  if (!std::filesystem::exists(destPath, ec) || ec) return false;
  std::ifstream marker(ConversionMarkerPath(destPath));
  int version = 0;
  if (!(marker >> version)) return false;
  int split = 0;
  if (!(marker >> split)) split = 0;
  return version == kBundleConversionVersion && split == ConversionSplitMode();
}

void MarkConversionCurrent(std::string const& destPath) {
  std::ofstream marker(ConversionMarkerPath(destPath), std::ios::out | std::ios::trunc);
  if (!marker) {
    PaperLogger.warn("Vivify could not record the converter version beside '{}'; the bundle will "
                     "be reconverted every launch", destPath);
    return;
  }
  marker << kBundleConversionVersion << " " << ConversionSplitMode() << "\n";
}

// CRASH GUARD FOR CONVERTED BUNDLES
//
// A translated shader that Unity or the GPU driver cannot cope with does not
// fail politely: the process dies, with nothing in the log after "bundle
// preloaded". Left alone that repeats on every launch the moment the level is
// selected, and the only way out was deleting files by hand.
//
// So a converted bundle is loaded behind a marker file. <bundle>.loading is
// written before Unity touches the bundle and removed once loading, and the
// first seconds of play, are over. Finding it still there on the next selection
// means that load never finished. The bundle is then redone without shader
// translation (the retarget-only conversion, which loads with stand-in shading)
// and <bundle>.crashed records which converter version crashed, so a later
// version with a fix gets to try translation again.
std::string LoadingMarkerPath(std::string const& destPath) {
  return destPath + ".loading";
}

std::string CrashedMarkerPath(std::string const& destPath) {
  return destPath + ".crashed";
}

bool FileExists(std::string const& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

// True when this converter version's translated output for destPath has already
// taken the game down once.
bool TranslationCrashedBefore(std::string const& destPath) {
  if (!GetCrashGuard()) return false;
  std::ifstream marker(CrashedMarkerPath(destPath));
  int version = 0;
  if (!(marker >> version)) return false;
  int strikes = 2;  // markers from before 0.14.15 carry no count; they meant it
  marker >> strikes;
  return version == kBundleConversionVersion && strikes >= 2;
}

// Turns a leftover .loading marker into a strike in the .crashed one. The
// first strike only keeps the record: a load that never finished is as often
// the game closed during a long first shader compile, or by hand, as a real
// crash, and giving up on translation at once left maps grey for good. The
// second strike in a row throws the bundle away, so the next conversion of it
// is retarget-only. Returns true when that happened.
bool RecordInterruptedLoad(std::string const& destPath) {
  if (destPath.empty() || !FileExists(LoadingMarkerPath(destPath))) return false;
  std::error_code ec;
  std::filesystem::remove(LoadingMarkerPath(destPath), ec);
  // Crash Guard off (0.14.24): an interrupted load is not held against the
  // bundle, and it is never reconverted without translation.
  if (!GetCrashGuard()) return false;
  int strikes = 0;
  {
    std::ifstream marker(CrashedMarkerPath(destPath));
    int version = 0;
    if (marker >> version && version == kBundleConversionVersion && !(marker >> strikes)) strikes = 0;
    if (version != kBundleConversionVersion) strikes = 0;
  }
  strikes++;
  {
    std::ofstream crashed(CrashedMarkerPath(destPath), std::ios::out | std::ios::trunc);
    if (crashed) crashed << kBundleConversionVersion << " " << strikes << "\n";
  }
  if (strikes < 2) {
    PaperLogger.warn("Vivify: the last load of converted bundle '{}' never finished (the game crashed, or was "
                     "closed during it). Trying the translated shaders once more; a second time, it is "
                     "reconverted without translation", destPath);
    return false;
  }
  PaperLogger.error("Vivify: the last two loads of converted bundle '{}' never finished -- the game most "
                    "likely crashes loading its translated shaders. Reconverting it without shader "
                    "translation so the level can be played (stand-in shading)", destPath);
  std::filesystem::remove(destPath, ec);
  std::filesystem::remove(ConversionMarkerPath(destPath), ec);
  return true;
}

// True for a shader whose job is to cover the view rather than to shade a
// surface: a full-screen blit, a skybox, a stencil mask, a fog volume.
//
// The distinction matters only when nothing of the original look can be carried
// across. A stand-in on a piece of scenery is a worse-looking mesh; a stand-in
// on one of these is an opaque quad between the player and everything else.
bool IsScreenSpaceEffectShader(std::string_view shaderName) {
  std::string lowered(shaderName);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Every word here has to name the shader's *job*, not merely appear in its
  // name, because a false positive deletes real scenery.
  //
  // "fog" was on this list and matched Swifter/SimpleTerrainFog -- the shader
  // 743Aether puts on its terrain -- so the ground, the spikes and the terrain
  // notes were all silently dropped and the map rendered as a few white shapes
  // over black. "screen" and "mask" were the same kind of mistake waiting to
  // happen; a real stencil mask is caught by "stencil" without them.
  static constexpr std::string_view screenEffects[] = {
      "blit"sv, "skybox"sv, "stencil"sv, "postpro"sv, "bokeh"sv,
      "shadowcaster"sv, "depthonly"sv, "cleardepth"sv, "vignette"sv,
  };
  for (auto effect : screenEffects) {
    if (lowered.find(effect) != std::string_view::npos) return true;
  }
  return false;
}

// Runs whichever conversion the settings ask for and flattens the two result
// shapes into one.
//
// The shader-translating path is the full conversion: it cross-compiles each
// DirectX program to GLSL ES and rebuilds the archive around the shaders that
// changed size. The retarget-only path is what this mod did before, kept as the
// setting's off position so a translation that turns out worse than an unshaded
// mesh can be backed out by reconverting rather than by waiting for a build.
struct BundleConversionOutcome {
  BundleConvert::Status status = BundleConvert::Status::Corrupt;
  std::string message;
};

// Converted bundles being written right now. Two conversions of the same
// source -- the background pass and a level selection -- both write dest
// through the same .part file, and interleaved they leave a corrupt bundle.
// The second one waits for the first, then uses its result.
std::mutex gConvertingMutex;
std::condition_variable gConvertingDone;
std::set<std::string> gConvertingDests;

struct DestClaim {
  std::string dest;
  explicit DestClaim(std::string d) : dest(std::move(d)) {
    std::unique_lock lock(gConvertingMutex);
    gConvertingDone.wait(lock, [this] { return !gConvertingDests.contains(dest); });
    gConvertingDests.insert(dest);
  }
  ~DestClaim() {
    {
      std::lock_guard lock(gConvertingMutex);
      gConvertingDests.erase(dest);
    }
    gConvertingDone.notify_all();
  }
};

BundleConversionOutcome RunBundleConversionUnclaimed(std::string const& source, std::string const& dest);

BundleConversionOutcome RunBundleConversion(std::string const& source, std::string const& dest) {
  DestClaim const claim(dest);
  // Another conversion may have just written it.
  if (CachedConversionIsCurrent(dest)) {
    return {BundleConvert::Status::Success, "converted by another pass moments ago"};
  }
  return RunBundleConversionUnclaimed(source, dest);
}

BundleConversionOutcome RunBundleConversionUnclaimed(std::string const& source, std::string const& dest) {
  if (GetTranslateShadersOnConversion() && TranslationCrashedBefore(dest)) {
    auto const result = BundleConvert::ConvertToAndroid(source, dest);
    // Marked current, unlike the setting's retarget-only path: this is the
    // version's answer for this bundle until a newer converter retries it.
    if (result.status == BundleConvert::Status::Success) MarkConversionCurrent(dest);
    PaperLogger.warn("Vivify converted '{}' without shader translation, because the translated "
                     "version crashed the game: {}", source, result.message);
    return {result.status, result.message};
  }
  if (!GetTranslateShadersOnConversion()) {
    auto const result = BundleConvert::ConvertToAndroid(source, dest);
    // Deliberately not marked current: a retarget-only bundle is what the
    // marker exists to invalidate, so turning the setting back on has to
    // reconvert rather than reuse this.
    return {result.status, result.message};
  }
  BundleConvert::ShaderConversionOptions options;
  // As 0.14.7 did (version 16 above): 2021 bundles split, 2019 not.
  options.separateStereoVariants = SplitStereoVariants();
  options.splitUnity2019 = false;
  // Opt-in (setting): links a variant whose geometry stage cannot run without
  // it, instead of leaving it to a grey stand-in. Off by default since 0.14.13.
  options.dropUntranslatableGeometry = GetDrawGeometryShaderEffects();
  // See version 17 above. Converted maps only: a donor build for a Quest
  // map's empty shaders keeps everything, since Quest materials keep their
  // instancing.
  options.skipQuestUnusedVariants = true;
  options.singleViewOnly = GetReplayRenderMode();
  auto const conversion = BundleConvert::ConvertShadersToGles(source, dest, options);
  if (conversion.status == BundleConvert::Status::Success) MarkConversionCurrent(dest);
  // Logged here, on the worker, rather than folded into the message: a bundle
  // can refuse several shaders and each reason is a line worth reading on its
  // own when working out why a converted map still looks wrong.
  for (auto const& refusal : conversion.refusals) {
    PaperLogger.info("Vivify shader translation left a shader as it was -- {}", refusal);
  }
  for (auto const& refusal : conversion.variantRefusals) {
    PaperLogger.info("Vivify shader translation left some variants on DirectX -- {}", refusal);
  }
  if (conversion.shadersRefused > static_cast<int>(conversion.refusals.size())) {
    PaperLogger.info("Vivify shader translation left {} further shader(s) as they were",
                     conversion.shadersRefused - static_cast<int>(conversion.refusals.size()));
  }
  PaperLogger.info("Vivify shader conversion: {} of {} shader(s) linked for multiview GLES, {} keyword "
                   "variant(s) linked and {} left on DirectX (one of their stages did not translate), {} "
                   "variant(s) given their single-pass stereo twin's per-eye code, {} stereo variant(s) "
                   "moved to STEREO_MULTIVIEW_ON beside single-view plain variants, {} shader(s) refused",
                   conversion.shadersLinked, conversion.shadersSeen, conversion.variantsLinked,
                   conversion.variantsRefused, conversion.stereoVariantsRemapped, conversion.stereoVariantsSplit,
                   conversion.shadersRefused);
  if (conversion.texturesSeen > 0) {
    PaperLogger.info(
        "Vivify conversion marked {} of {} block-compressed texture(s) readable ({} keep their pixels "
        "in a companion stream). A texture that is not readable loses its pixels at load, and decoding "
        "it then produces solid black.",
        conversion.texturesMarkedReadable, conversion.texturesSeen, conversion.texturesStreamed);
  }
  return {conversion.status, conversion.message};
}

std::string ResolveBundlePath(std::string const& levelPath) {
  std::string bundlePath = JoinPath(levelPath, kBundleFile);
  if (std::filesystem::exists(bundlePath)) {
    return bundlePath;
  }
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator(levelPath, ec)) {
    if (ec) break;
    auto const& p = entry.path();
    if (p.extension() == ".vivify") {
      return p.string();
    }
  }
  return {};
}

// Finds a PC-built Vivify AssetBundle in a song folder, by content.
//
// Upstream Vivify names its bundle from VivifyController.BUNDLE_FILE,
// $"bundle{BUNDLE_SUFFIX}.vivify" where BUNDLE_SUFFIX is "Windows2021" (or
// "Windows2019" on 1.29.1), so a PC map normally ships bundleWindows2021.vivify
// -- extension included. This port's own download path writes
// bundleAndroid2021.vivify, and ResolveBundlePath already finds either by
// extension.
//
// The reason this scan is content-based rather than name-based anyway is that
// the name is only a convention: re-zipped map downloads, hand-built bundles
// and the Unity exporter's own output all turn up under other names, and a
// bundle this function fails to find is a map that can never be converted and
// so can never be played. Every candidate is checked for the UnityFS signature
// instead. Names are used only to rank equally-valid candidates: a "windows"
// name wins over a generic "bundle" name, which wins over anything else that
// happens to be a Unity archive.
std::string ResolvePcBundlePath(std::string const& levelPath) {
  std::error_code ec;
  std::string best;
  int bestScore = -1;
  for (auto const& entry : std::filesystem::directory_iterator(levelPath, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    auto const& path = entry.path();
    std::string lower = path.filename().string();
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Skip the file types a song folder is otherwise made of, then content-check
    // whatever is left. Being permissive here is the point: over-restrictive
    // name matching is what hid these bundles in the first place.
    static constexpr std::string_view kNonBundleExtensions[] = {
        ".dat", ".json", ".ogg", ".egg", ".wav", ".mp3", ".jpg", ".jpeg", ".png", ".bmp", ".txt", ".md",
    };
    std::string const extension = path.extension().string();
    std::string lowerExtension = extension;
    std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (std::find(std::begin(kNonBundleExtensions), std::end(kNonBundleExtensions), lowerExtension) !=
        std::end(kNonBundleExtensions)) {
      continue;
    }

    int score = 1;
    if (lower.find("windows") != std::string::npos) score = 3;
    else if (lower.find("bundle") != std::string::npos || path.extension() == ".vivify") score = 2;
    if (score <= bestScore) continue;
    if (!BundleConvert::IsUnityBundleFile(path.string())) continue;
    best = path.string();
    bestScore = score;
  }
  return best;
}

// Where on-device-converted bundles are cached. Kept out of the song folder so
// syncing or re-downloading a map never trips over a file Vivify generated, and
// so a converted bundle is never mistaken for one the mapper shipped.
std::string ConvertedBundleCacheDir() {
  return "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/ConvertedBundles";
}

// Cache key for a converted bundle.
//
// Deliberately built from the song folder name, the bundle file name, and the
// bundle's size and mtime -- NOT from its absolute path. The bulk pass walks
// SongCore's level roots while level selection uses
// CustomBeatmapLevel::customLevelPath, and on Android the same directory is
// reachable as /sdcard/..., /storage/emulated/0/... and
// /storage/self/primary/... . Keying on the absolute path meant those two
// routes could hash the same file differently, so a bundle converted by the
// bulk pass was not found again at level selection and the map stayed
// unplayable as though nothing had been converted.
//
// Size and mtime still invalidate the entry when the source bundle changes,
// without having to hash hundreds of megabytes.
std::string ConvertedBundlePath(std::string const& sourceBundlePath) {
  std::filesystem::path const source(sourceBundlePath);
  std::string const fileName = source.filename().string();
  std::string const folderName = source.parent_path().filename().string();

  std::error_code ec;
  uint64_t size = std::filesystem::file_size(source, ec);
  if (ec) size = 0;
  ec.clear();
  auto const writeTime = std::filesystem::last_write_time(source, ec);
  uint64_t stamp = 0;
  if (!ec) stamp = static_cast<uint64_t>(writeTime.time_since_epoch().count());

  uint64_t hash = 1469598103934665603ull;
  auto mix = [&hash](std::string_view bytes) {
    for (char c : bytes) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
  };
  mix(folderName);
  mix("\x1f");
  mix(fileName);
  mix("\x1f");
  mix(std::to_string(size));
  mix("\x1f");
  mix(std::to_string(stamp));

  // Keep a readable prefix so the cache directory can be eyeballed against the
  // song list when something looks wrong.
  std::string prefix;
  for (char c : folderName) {
    if (prefix.size() >= 48) break;
    bool const safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      c == '-' || c == '_';
    prefix.push_back(safe ? c : '_');
  }

  // Replay render mode's single-view conversions live beside the normal ones
  // rather than replacing them, so switching modes does not reconvert.
  char suffix[40];
  std::snprintf(suffix, sizeof(suffix), GetReplayRenderMode() ? "_%016llx_replay.vivify" : "_%016llx.vivify",
                static_cast<unsigned long long>(hash));
  return JoinPath(ConvertedBundleCacheDir(), prefix + suffix);
}

// Info.dat's assetBundle checksum for one build ("android2021", "windows2021",
// "windows2019"), under either the v2 (_underscored) or v4 spelling. 0 when absent.
uint32_t ReadBundleChecksumFromInfoDat(std::string const& levelPath, std::string const& build) {
  std::string infoPath = JoinPath(levelPath, "Info.dat");
  if (!std::filesystem::exists(infoPath)) infoPath = JoinPath(levelPath, "info.dat");
  if (!std::filesystem::exists(infoPath)) return 0;
  std::ifstream ifs(infoPath);
  if (!ifs.is_open()) return 0;
  std::string str((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  rapidjson::Document doc;
  doc.Parse(str.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return 0;
  rapidjson::Value const* customData = nullptr;
  if (doc.HasMember("_customData")) customData = &doc["_customData"];
  else if (doc.HasMember("customData")) customData = &doc["customData"];
  if (customData == nullptr || !customData->IsObject()) return 0;
  rapidjson::Value const* assetBundle = nullptr;
  if (customData->HasMember("_assetBundle")) assetBundle = &(*customData)["_assetBundle"];
  else if (customData->HasMember("assetBundle")) assetBundle = &(*customData)["assetBundle"];
  if (assetBundle == nullptr || !assetBundle->IsObject()) return 0;
  for (std::string const key : {"_" + build, build}) {
    auto it = assetBundle->FindMember(key.c_str());
    if (it != assetBundle->MemberEnd() && it->value.IsUint()) return it->value.GetUint();
  }
  return 0;
}

uint32_t ReadAndroidChecksumFromInfoDat(std::string const& levelPath) {
  std::string infoPath = JoinPath(levelPath, "Info.dat");
  if (!std::filesystem::exists(infoPath)) infoPath = JoinPath(levelPath, "info.dat");
  if (!std::filesystem::exists(infoPath)) return 0;
  std::ifstream ifs(infoPath);
  if (!ifs.is_open()) return 0;
  std::string str((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  rapidjson::Document doc;
  doc.Parse(str.c_str());
  if (doc.HasParseError()) return 0;
  rapidjson::Value const* customData = nullptr;
  if (doc.HasMember("_customData")) customData = &doc["_customData"];
  else if (doc.HasMember("customData")) customData = &doc["customData"];
  if (customData == nullptr || !customData->IsObject()) return 0;
  rapidjson::Value const* assetBundle = nullptr;
  if (customData->HasMember("_assetBundle")) assetBundle = &(*customData)["_assetBundle"];
  else if (customData->HasMember("assetBundle")) assetBundle = &(*customData)["assetBundle"];
  if (assetBundle == nullptr || !assetBundle->IsObject()) return 0;
  if (assetBundle->HasMember("_android2021") && (*assetBundle)["_android2021"].IsUint()) {
    return (*assetBundle)["_android2021"].GetUint();
  }
  if (assetBundle->HasMember("android2021") && (*assetBundle)["android2021"].IsUint()) {
    return (*assetBundle)["android2021"].GetUint();
  }
  return 0;
}

struct MaterialFallbackState {
  std::optional<UnityEngine::Color> color;
  UnityEngine::Texture* mainTexture = nullptr;
  int renderQueue = -1;
};

// True for a colour that would leave the mesh looking blank.
bool IsUninformativeColor(UnityEngine::Color const& color) {
  bool const nearWhite = color.r > 0.97f && color.g > 0.97f && color.b > 0.97f;
  bool const invisible = color.a <= 0.01f;
  return nearWhite || invisible;
}

// Emission is additive: black emission means "no glow", not "this object is
// black". Every Standard-shader material carries `_EmissionColor` at its
// default of (0, 0, 0, 1), and when this list checked emission before
// `_Color` that default counted as the material's real colour -- so any
// converted Standard material came out black whatever its albedo was.
bool IsBlankEmission(UnityEngine::Color const& color) {
  return color.r < 0.02f && color.g < 0.02f && color.b < 0.02f;
}

// Names that are colour-shaped but describe something other than the
// surface: a rim, an outline, a shadow tint, a specular highlight, fog. Taking
// one of those as the albedo tints the whole mesh the colour of its edge.
bool IsSecondaryColorName(std::string const& key) {
  static constexpr std::string_view secondary[] = {
      "shadow"sv, "spec"sv,   "rim"sv,   "outline"sv, "fog"sv,   "reflect"sv,
      "fresnel"sv, "edge"sv,  "ambient"sv, "sss"sv,   "subsurface"sv, "highlight"sv,
  };
  for (auto word : secondary) {
    if (key.find(word) != std::string::npos) return true;
  }
  return false;
}

// Recovers the colour a material was actually tinted with.
//
// This used to return the first property that *existed*, which is almost always
// `_Color` -- a property nearly every shader declares and nearly every material
// leaves at its default of white. So the stand-in faithfully carried white
// across and every converted asset came out flat white, even though the
// material's real colour was sitting in `_BaseColor`, `_EmissionColor` or one of
// the map's own named properties. Material property *values* are stored in the
// SerializedFile and are entirely platform-independent, so that colour survives
// conversion perfectly -- it was only ever being looked up wrong.
//
// Three refinements on top of "first informative colour wins":
//   - the albedo names come first and emission last, and a black emission is
//     never taken (see IsBlankEmission);
//   - when the material has an albedo texture, its primary colour is kept even
//     at white, because then the texture carries the look and white is the
//     correct multiplier -- hunting on for "something more colourful" is how a
//     rim or outline colour ended up painted over a whole textured mesh;
//   - HDR colours are brought back into range keeping their hue (see
//     NormalizeHdrColor), since the Quest has no bloom to spend the overflow on
//     and an unscaled (6, 0.8, 0.3) clamps to near-white.
std::optional<UnityEngine::Color> ReadMaterialFallbackColor(UnityEngine::Material* material,
                                                            bool hasAlbedoTexture = false) {
  if (!IsManagedAlive(material)) return std::nullopt;
  static int const primaryIds[] = {
      UnityEngine::Shader::PropertyToID(u"_BaseColor"),
      UnityEngine::Shader::PropertyToID(u"_Color"),
      UnityEngine::Shader::PropertyToID(u"_MainColor"),
      UnityEngine::Shader::PropertyToID(u"_TintColor"),
      UnityEngine::Shader::PropertyToID(u"_Tint"),
  };
  static int const secondaryIds[] = {
      UnityEngine::Shader::PropertyToID(u"_HorizonCol"),
      UnityEngine::Shader::PropertyToID(u"_SkyCol"),
  };
  static int const emissionIds[] = {
      UnityEngine::Shader::PropertyToID(u"_EmissionColor"),
      UnityEngine::Shader::PropertyToID(u"_Emission"),
  };

  std::optional<UnityEngine::Color> firstFound;
  auto consider = [&firstFound](UnityEngine::Color color) {
    color = NormalizeHdrColor(color);
    if (!firstFound.has_value()) firstFound = color;
    return !IsUninformativeColor(color);
  };

  // With a texture, a primary colour is the answer even at white -- but a
  // shader declaring both `_BaseColor` and `_Color` usually sets only one of
  // them, so a non-white primary still beats a white one.
  std::optional<UnityEngine::Color> texturedPrimary;
  for (int id : primaryIds) {
    if (!material->HasProperty(id)) continue;
    auto color = NormalizeHdrColor(material->GetColor(id));
    if (consider(color)) return color;
    if (hasAlbedoTexture && color.a > 0.01f && !texturedPrimary.has_value()) texturedPrimary = color;
  }
  if (texturedPrimary.has_value()) return texturedPrimary;
  for (int id : secondaryIds) {
    if (!material->HasProperty(id)) continue;
    auto color = material->GetColor(id);
    if (consider(color)) return NormalizeHdrColor(color);
  }

  // Then anything colour-shaped the material declares itself. Colours live in
  // the Vector bucket -- MaterialPropertyType has no separate Color member.
  auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Vector);
  if (names) {
    for (auto name : names) {
      if (!name) continue;
      std::string key = NormalizeAssetKey(ToStdString(name));
      if (key.find("color") == std::string::npos && key.find("colour") == std::string::npos &&
          key.find("col") == std::string::npos && key.find("tint") == std::string::npos) {
        continue;
      }
      if (IsSecondaryColorName(key) || key.find("emis") != std::string::npos) continue;
      auto color = material->GetColor(name);
      if (consider(color)) return NormalizeHdrColor(color);
    }
  }

  // Emission last, and only when it is actually glowing. For an emissive-only
  // material -- common in Vivify maps, whose look is all glow -- this is the
  // colour that matters.
  for (int id : emissionIds) {
    if (!material->HasProperty(id)) continue;
    auto color = material->GetColor(id);
    if (IsBlankEmission(color)) continue;
    color.a = 1.0f;
    if (consider(color)) return NormalizeHdrColor(color);
  }
  return firstFound;
}

// Recovers a texture to hand to the stand-in shader.
//
// Material.mainTexture only ever resolves `_MainTex`. Custom shaders routinely
// name their albedo something else (`_BaseMap`, `_Albedo`, `_Tex`), so any
// material not using the conventional name came through untextured.
//
// A texture this GPU cannot sample is not worth carrying. The decode pass runs
// before shader repair and swaps every texture it can rescue for an RGBA32
// copy, so anything still in a block-compressed format by now has no pixels
// available (see ResolveUsableTexture). Handed to a stand-in it samples as flat
// white, and -- worse -- its presence tells ReadMaterialFallbackColor that the
// texture carries the look, so a white `_Color` would be kept as-is. Treating it
// as absent lets the colour search find the material's real tint instead.
bool IsSampleableTexture(UnityEngine::Texture* texture) {
  if (!IsManagedAlive(texture)) return false;
  auto* texture2d = il2cpp_utils::try_cast<UnityEngine::Texture2D>(texture).value_or(nullptr);
  if (!IsManagedAlive(texture2d)) return true;
  return UnityEngine::SystemInfo::SupportsTextureFormat(texture2d->get_format());
}

UnityEngine::Texture* ReadMaterialFallbackTexture(UnityEngine::Material* material) {
  if (!IsManagedAlive(material)) return nullptr;
  if (auto* mainTexture = material->get_mainTexture().unsafePtr(); IsSampleableTexture(mainTexture)) {
    return mainTexture;
  }
  auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Texture);
  if (!names) return nullptr;
  for (auto name : names) {
    if (!name) continue;
    std::string key = NormalizeAssetKey(ToStdString(name));
    // Skip the maps that would look wrong as an albedo.
    if (key.find("bump") != std::string::npos || key.find("normal") != std::string::npos ||
        key.find("mask") != std::string::npos || key.find("metallic") != std::string::npos ||
        key.find("occlusion") != std::string::npos || key.find("smoothness") != std::string::npos ||
        key.find("height") != std::string::npos || key.find("detail") != std::string::npos) {
      continue;
    }
    if (auto* texture = material->GetTexture(name).unsafePtr(); IsSampleableTexture(texture)) {
      return texture;
    }
  }
  return nullptr;
}

MaterialFallbackState CaptureMaterialFallbackState(UnityEngine::Material* material) {
  MaterialFallbackState state;
  if (!IsManagedAlive(material)) return state;
  state.mainTexture = ReadMaterialFallbackTexture(material);
  state.color = ReadMaterialFallbackColor(material, IsManagedAlive(state.mainTexture));
  state.renderQueue = material->get_renderQueue();
  return state;
}

void RestoreMaterialFallbackState(UnityEngine::Material* material, MaterialFallbackState const& state) {
  if (!IsManagedAlive(material)) return;
  if (state.color.has_value()) {
    static int const fallbackColorIds[] = {
        UnityEngine::Shader::PropertyToID(u"_Color"),
        UnityEngine::Shader::PropertyToID(u"_BaseColor"),
        UnityEngine::Shader::PropertyToID(u"_TintColor"),
   };
    for (int id : fallbackColorIds) {
      if (material->HasProperty(id)) {
        material->SetColor(id, state.color.value());
      }
    }
  }
  if (IsManagedAlive(state.mainTexture)) {
    // set_mainTexture only writes _MainTex; a stand-in using URP-style naming
    // wants _BaseMap instead, so write whichever the shader actually declares.
    static int const textureIds[] = {
        UnityEngine::Shader::PropertyToID(u"_MainTex"),
        UnityEngine::Shader::PropertyToID(u"_BaseMap"),
    };
    for (int id : textureIds) {
      if (material->HasProperty(id)) {
        material->SetTexture(id, state.mainTexture);
      }
    }
  }
  if (state.renderQueue >= 0) {
    material->set_renderQueue(state.renderQueue);
  }
}
}

void Runtime::HandleLevelSelected(SongCore::API::LevelSelect::LevelWasSelectedEventArgs const& event) {
  // Getting back to level selection means whatever was loading last is done.
  DisarmLoadGuard();
  // Before anything here converts a bundle: which way to convert depends on it.
  DetectMultiviewKeyword();

  std::string incomingLevelPath;
  if (event.isCustom && event.customBeatmapLevel != nullptr) {
    incomingLevelPath = std::string(event.customBeatmapLevel->customLevelPath);
  }
  if (!incomingLevelPath.empty() && incomingLevelPath != _selectedLevelPath) {
    if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
      _mainBundle->Unload(true);
      _mainBundle = nullptr;
    }
    _preloadedBundlePath.clear();
    ClearBlitCompanion();
  }
  ResetRuntime("left the level");

  _activeSabers.clear();
  _selectedLevelPath.clear();
  _selectedBundlePath.clear();
  _selectedMapHasVivifyRequirement = false;
  CancelPendingDownload();
  if (!event.isCustom || event.customBeatmapLevel == nullptr) {
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    return;
  }
  _selectedLevelPath = std::string(event.customBeatmapLevel->customLevelPath);
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify level selected: path='{}' isCustom={} hasDetails={}",
                     _selectedLevelPath, BoolText(event.isCustom), BoolText(event.customLevelDetails.has_value()));
  }
  if (event.customLevelDetails) {
    auto const& requirements = event.customLevelDetails->difficultyDetails.requirements;
    _selectedMapHasVivifyRequirement = std::any_of(requirements.begin(), requirements.end(), [](std::string const& requirement) {
      return requirement == kCapability;
    });
  }
  if (!_selectedMapHasVivifyRequirement) {
    MetaCore::Game::SetScoreSubmission("Vivify", true);
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    return;
  }

  // Vivify changes how a map looks, not how it plays: no note timing, no
  // scoring, nothing a leaderboard measures. Disabling submission for every
  // Vivify map -- which is what this did unconditionally -- also stopped
  // BeatLeader and ScoreSaber recording a replay, which is why Vivify maps had
  // no replays to watch or render. Submission is on by default now, with a
  // setting for anyone who wants the old behaviour.
  bool const submit = GetSubmitScoresOnVivifyMaps();
  MetaCore::Game::SetScoreSubmission("Vivify", submit);
  PaperLogger.info("Vivify score submission: {} for this Vivify map",
                   submit ? "enabled" : "disabled by setting");
  // The settings that decide whether a map renders at all, recorded once per
  // level. Chasing "geometry stopped being repaired halfway through a session"
  // took a guess at which toggle had moved, because nothing in the log said
  // what any of them were set to at the time.
  PaperLogger.info("Vivify settings for this level: standInShading={} convertPcBundlesOnDevice={} "
                   "disableCustomNoteVisuals={} disableAllBlits={} multipassRendering={} effectResolution={}% "
                   "sceneDepth={} prepareShaders={} geometryEffects={} offscreenCulling={} farCulling={}m crashGuard={} "
                   "pcBundleForMissingShaders={}",
                   BoolText(GetStandInShading()), BoolText(GetConvertPcBundlesOnDevice()),
                   BoolText(GetDisableCustomNoteVisuals()), BoolText(GetDisableAllBlits()),
                   BoolText(GetMultipassRenderingEnabled()), GetEffectResolutionPercent(),
                   BoolText(GetSceneDepthTexture()), BoolText(GetPrepareShadersBeforePlaying()),
                   BoolText(GetDrawGeometryShaderEffects()), BoolText(GetOffscreenCulling()),
                   GetFarCullingDistance(), BoolText(GetCrashGuard()), BoolText(GetUsePcBundleForMissingShaders()));

  std::string const androidBundlePath = JoinPath(_selectedLevelPath, std::string(kBundleFile));

  if (std::filesystem::exists(androidBundlePath)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle selection: using local Android bundle '{}'", androidBundlePath);
    }
    BeginAndroidBundleLoad(_selectedLevelPath, androidBundlePath);
    return;
  }

  // No Android bundle in the song folder. Work out what else is available
  // before deciding, and log the whole picture unconditionally -- when a map
  // will not start, this one line says exactly which branch was taken and why.
  std::string const pcBundleFallback = ResolvePcBundlePath(_selectedLevelPath);
  uint32_t const androidChecksum = ReadAndroidChecksumFromInfoDat(_selectedLevelPath);
  std::string const cachedConversion =
      pcBundleFallback.empty() ? std::string() : ConvertedBundlePath(pcBundleFallback);
  RecordInterruptedLoad(cachedConversion);
  bool const haveCachedConversion =
      !cachedConversion.empty() && CachedConversionIsCurrent(cachedConversion);

  PaperLogger.info(
      "Vivify bundle selection: level='{}' androidBundle=no android2021={} pcBundle='{}' convertedCache='{}' cached={}",
      _selectedLevelPath, androidChecksum,
      pcBundleFallback.empty() ? std::string("<none>") : pcBundleFallback,
      cachedConversion.empty() ? std::string("<none>") : cachedConversion,
      BoolText(haveCachedConversion));

  // An already-converted bundle is on disk and ready, so use it now instead of
  // going to the network.
  //
  // Checking the download first was wrong: a map that ships a PC bundle
  // usually has no Android build in the bundle repo to download -- that is why
  // it only ships a PC bundle -- so the request fails, or worse hangs, and the
  // play button sits on "Downloading assets..." while a perfectly good
  // converted bundle is sitting in the cache unused. That is what made
  // already-converted levels stay unplayable.
  if (haveCachedConversion) {
    _selectedBundlePath = cachedConversion;
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    PreloadBundle(cachedConversion);
    PrepareBlitCompanion(_selectedLevelPath, pcBundleFallback);
    return;
  }

  if (androidChecksum != 0) {
    BeginBundleDownload(androidChecksum, _selectedLevelPath, pcBundleFallback);
    return;
  }
  if (!pcBundleFallback.empty()) {
    ConvertPcBundleAsync(_selectedLevelPath, pcBundleFallback);
    return;
  }
  if (TryDownloadPcBundle(_selectedLevelPath)) return;
  PaperLogger.warn("Vivify: '{}' has no Android bundle, no PC bundle to convert, and no bundle checksum in "
                   "Info.dat to download one by", _selectedLevelPath);
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "No Vivify assets found for this map.");
}

// A Vivify map with no Quest assets at all: download its PC bundle by the
// windows checksum in Info.dat (from the same repository the Quest bundles
// come from) into the song folder, then convert it as usual. Next time the
// song folder has it, and the conversion is cached. False when Info.dat names
// no PC bundle.
bool Runtime::TryDownloadPcBundle(std::string const& levelPath) {
  if (!GetConvertPcBundlesOnDevice()) return false;
  uint32_t checksum = ReadBundleChecksumFromInfoDat(levelPath, "windows2021");
  std::string destName = "bundleWindows2021.vivify";
  if (checksum == 0) {
    checksum = ReadBundleChecksumFromInfoDat(levelPath, "windows2019");
    destName = "bundleWindows2019.vivify";
  }
  if (checksum == 0) return false;
  std::string const dest = JoinPath(levelPath, destName);
  PaperLogger.info("Vivify: no Quest assets for '{}'; downloading its PC bundle (checksum {}) to convert",
                   levelPath, checksum);
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Downloading PC assets...");
  DownloadBundleTo(checksum, dest, [this, levelPath, dest](bool ok) {
    if (levelPath != _selectedLevelPath) return;
    if (!ok) {
      SongCore::API::PlayButton::DisablePlayButton("Vivify", "PC asset download failed.");
      return;
    }
    ConvertPcBundleAsync(levelPath, dest);
  });
  return true;
}

void Runtime::CancelPendingDownload() {
  _downloadGeneration++;
  _downloadDeadline = -1.0f;
  _pendingDownloadLevelPath.clear();
  _pendingDownloadPcFallback.clear();
}

// WebUtils does not promise a callback on every failure mode (a dropped
// connection or a stalled request can simply never resolve), and a level whose
// play button is waiting on one would stay unplayable for the rest of the
// session. Time it out and take the conversion path instead.
void Runtime::CheckDownloadTimeout() {
  if (_graftDeadline >= 0.0f && UnityEngine::Time::get_realtimeSinceStartup() >= _graftDeadline) {
    _graftDeadline = -1.0f;
    _graftGeneration++;  // the late callback, if it ever comes, is ignored
    PaperLogger.warn("Vivify: the PC bundle download for the empty shaders timed out; loading the Quest bundle "
                     "as it is");
    FinishAndroidBundleLoad(_graftPendingLevel, _graftPendingAndroid);
  }
  if (_downloadDeadline < 0.0f) return;
  if (UnityEngine::Time::get_realtimeSinceStartup() < _downloadDeadline) return;

  std::string const levelPath = _pendingDownloadLevelPath;
  std::string const pcBundleFallback = _pendingDownloadPcFallback;
  CancelPendingDownload();
  PaperLogger.warn("Vivify asset download timed out for '{}'", levelPath);
  if (levelPath != _selectedLevelPath) return;
  if (!pcBundleFallback.empty()) {
    ConvertPcBundleAsync(levelPath, pcBundleFallback);
    return;
  }
  if (TryDownloadPcBundle(levelPath)) return;
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Asset download timed out.");
}

void Runtime::BeginBundleDownload(uint32_t checksum, std::string const& levelPath,
                                  std::string const& pcBundleFallback) {
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Downloading assets...");
  CancelPendingDownload();
  int const generation = _downloadGeneration;
  _downloadDeadline = UnityEngine::Time::get_realtimeSinceStartup() + kAssetDownloadTimeoutSeconds;
  _pendingDownloadLevelPath = levelPath;
  _pendingDownloadPcFallback = pcBundleFallback;

  DownloadBundle(checksum, levelPath, [this, generation, levelPath, pcBundleFallback](bool success) {
    // A newer selection (or the timeout) already moved on from this download.
    if (generation != _downloadGeneration) return;
    CancelPendingDownload();
    if (levelPath != _selectedLevelPath) return;
    if (success) {
      std::string downloaded = ResolveBundlePath(levelPath);
      if (!downloaded.empty()) {
        BeginAndroidBundleLoad(levelPath, downloaded);
        return;
      }
      SongCore::API::PlayButton::EnablePlayButton("Vivify");
      return;
    }
    // The download is the preferred path, but a map that ships a PC bundle is
    // still recoverable without it.
    if (!pcBundleFallback.empty()) {
      PaperLogger.warn("Vivify asset download failed; falling back to converting the PC bundle '{}'",
                       pcBundleFallback);
      ConvertPcBundleAsync(levelPath, pcBundleFallback);
      return;
    }
    if (TryDownloadPcBundle(levelPath)) return;
    SongCore::API::PlayButton::DisablePlayButton("Vivify", "Failed to download assets.");
  });
}

// Converts a PC-built AssetBundle into an Android-loadable one on the device.
//
// This replaces the old "Allow Unsafe Windows Bundle Fallback" toggle, which
// handed the Windows bundle straight to UnityEngine.AssetBundle.LoadFromFile.
// Unity does not reject that outright -- it returns a bundle object whose
// GetAllAssetNames() is empty, which is exactly the reported "the experimental
// Windows bundle doesn't have any assets" behaviour. Retargeting the archive
// first is what actually makes Unity enumerate and load its contents.
void Runtime::ConvertPcBundleAsync(std::string const& levelPath, std::string const& sourceBundlePath) {
  if (!GetConvertPcBundlesOnDevice()) {
    PaperLogger.warn("Vivify found a PC asset bundle but on-device conversion is disabled: '{}'", sourceBundlePath);
    SongCore::API::PlayButton::DisablePlayButton("Vivify",
                                                 "PC bundle found; enable Convert PC Bundles On Device in settings.");
    return;
  }

  if (_bundleConversionSource == sourceBundlePath) {
    // Already running for this bundle; its completion handler will re-enable
    // the play button.
    SongCore::API::PlayButton::DisablePlayButton("Vivify", "Converting PC assets...");
    return;
  }

  std::string const destPath = ConvertedBundlePath(sourceBundlePath);
  RecordInterruptedLoad(destPath);
  if (CachedConversionIsCurrent(destPath)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify using cached converted bundle: '{}'", destPath);
    }
    _selectedBundlePath = destPath;
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    PreloadBundle(destPath);
    PrepareBlitCompanion(levelPath, sourceBundlePath);
    return;
  }

  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Converting PC assets...");
  PaperLogger.info("Vivify converting PC asset bundle on device: '{}' -> '{}'", sourceBundlePath, destPath);
  _bundleConversionSource = sourceBundlePath;

  // Conversion decompresses the whole archive, so it must not run on the main
  // thread. Results are handed back the same way the download path does it.
  std::thread([this, levelPath, sourceBundlePath, destPath]() {
    BundleConversionOutcome const result = RunBundleConversion(sourceBundlePath, destPath);
    // A bundle that was already Android-targeted needs no rewrite; load it as-is.
    std::string loadPath = result.status == BundleConvert::Status::AlreadyAndroid ? sourceBundlePath : destPath;
    bool const usable = result.status == BundleConvert::Status::Success ||
                        result.status == BundleConvert::Status::AlreadyAndroid;
    std::string const message = result.message;
    std::string const statusText{BundleConvert::StatusText(result.status)};

    // Report what the source bundle's shaders were actually compiled for. This
    // is the one place with the answer to "why does this map render with
    // stand-in shading" that is not a guess: it reads the Shader assets and
    // names their target platforms. Done on the worker, since it unpacks the
    // archive a second time.
    std::string shaderScanText;
    if (usable) {
      auto const scan = BundleConvert::ScanShaders(sourceBundlePath);
      shaderScanText = BundleConvert::DescribeShaderScan(scan);
    }

    BSML::MainThreadScheduler::Schedule([this, levelPath, sourceBundlePath, loadPath, usable, message, statusText, shaderScanText]() {
      if (!shaderScanText.empty()) {
        _sourceBundleScanText = shaderScanText;
        PaperLogger.info("Vivify source bundle shaders: {}", shaderScanText);
      }
      // Only clear the in-flight marker if a newer conversion has not claimed it.
      if (_bundleConversionSource == sourceBundlePath) _bundleConversionSource.clear();
      if (usable) {
        PaperLogger.info("Vivify bundle conversion succeeded: {}", message);
      } else {
        PaperLogger.warn("Vivify bundle conversion failed ({}): {}", statusText, message);
      }
      if (levelPath != _selectedLevelPath) return;
      if (!usable) {
        SongCore::API::PlayButton::DisablePlayButton("Vivify", "Convert failed: " + statusText);
        return;
      }
      _selectedBundlePath = loadPath;
      SongCore::API::PlayButton::EnablePlayButton("Vivify");
      PreloadBundle(loadPath);
      if (loadPath != sourceBundlePath) PrepareBlitCompanion(levelPath, sourceBundlePath);
    });
  }).detach();
}

// ---------------------------------------------------------------------------
// Blit companion
//
// A converted map's shaders hold multiview programs, which is what the eye
// cameras need, and they cannot hold a single-view program beside them that
// the headset would pick (the stereo keyword split never worked there; see
// kBundleConversionVersion 13). But a blit into a single texture -- a screen
// texture, a temporary -- is single-view, and a multiview program draws
// nothing into it: those effects went missing.
//
// So a map that uses blits also gets a second, single-view conversion of its
// PC bundle, with its internal files renamed so it loads beside the main one.
// Its shaders, by name, are what BlitMaterialFor (VivifyPostProcessing.cpp)
// swaps in for blits into single textures.
// ---------------------------------------------------------------------------

namespace {

std::string BlitCompanionPath(std::string const& pcBundlePath) {
  std::string path = ConvertedBundlePath(pcBundlePath);
  size_t const dot = path.rfind(".vivify");
  if (dot != std::string::npos) path.insert(dot, "_blit");
  return path;
}

// Whether any difficulty of the level has a Blit event (or a legacy alias).
// A plain text search: false positives only cost a conversion.
bool LevelUsesBlits(std::string const& levelPath) {
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator(levelPath, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string const name = entry.path().filename().string();
    if (name.size() < 4 || name.substr(name.size() - 4) != ".dat") continue;
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "info.dat") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (char const* type : {"\"Blit\"", "\"PostProcess\"", "\"PostProcessing\"", "\"ScreenEffect\""}) {
      if (text.find(type) != std::string::npos) return true;
    }
  }
  return false;
}

}  // namespace

void Runtime::PrepareBlitCompanion(std::string const& levelPath, std::string const& pcBundlePath) {
  // Opt-in (0.14.11) until it has been seen working on a headset. Replay
  // render mode's main conversion is single-view already.
  if (!GetBlitScreenTextureFix() || GetReplayRenderMode() || !GetTranslateShadersOnConversion() ||
      pcBundlePath.empty()) {
    return;
  }
  if (_blitCompanionLevel == levelPath && !_singleViewShaders.empty()) return;
  std::thread([this, levelPath, pcBundlePath]() {
    if (!LevelUsesBlits(levelPath)) return;
    std::string const companion = BlitCompanionPath(pcBundlePath);
    if (!CachedConversionIsCurrent(companion)) {
      BundleConvert::ShaderConversionOptions options;
      options.singleViewOnly = true;
      options.renameArchiveFiles = true;
      options.skipQuestUnusedVariants = true;  // same materials as the main conversion
      auto const result = BundleConvert::ConvertShadersToGles(pcBundlePath, companion, options);
      if (result.status != BundleConvert::Status::Success) {
        PaperLogger.warn("Vivify Blit: the single-view build for blits into screen textures failed: {}",
                         result.message);
        return;
      }
      MarkConversionCurrent(companion);
    }
    BSML::MainThreadScheduler::Schedule([this, levelPath, companion]() {
      if (levelPath != _selectedLevelPath) return;
      LoadBlitCompanion(levelPath, companion);
    });
  }).detach();
}

void Runtime::LoadBlitCompanion(std::string const& levelPath, std::string const& companionPath) {
  // One load at a time: a second copy of the same companion would clash with
  // the one in flight. The one in flight checks the level when it lands.
  if (_blitCompanionCreate || _blitCompanionAssets) return;
  ClearBlitCompanion();
  // Asynchronous, advanced by PollBlitCompanion. 0.14.10 loaded the bundle and
  // every material in it (and so their textures) on the main thread in one
  // call, which froze the game on converted maps.
  auto* request = UnityEngine::AssetBundle::LoadFromFileAsync(StringW(companionPath));
  if (request == nullptr) {
    PaperLogger.warn("Vivify Blit: the single-view build '{}' did not start loading", companionPath);
    return;
  }
  _blitCompanionCreate = request;
  _blitCompanionLoadingLevel = levelPath;
  _blitCompanionLoadingPath = companionPath;
}

void Runtime::PollBlitCompanion() {
  if (_blitCompanionCreate) {
    if (!_blitCompanionCreate->get_isDone()) return;
    auto bundle = _blitCompanionCreate->get_assetBundle();
    _blitCompanionCreate = nullptr;
    if (!IsAlive(bundle.unsafePtr())) {
      PaperLogger.warn("Vivify Blit: the single-view build '{}' did not load", _blitCompanionLoadingPath);
      return;
    }
    if (_blitCompanionLoadingLevel != _selectedLevelPath) {
      bundle->Unload(false);
      return;
    }
    _blitCompanionBundle = bundle.unsafePtr();
    // Blit materials are always assets of their own (a Blit event names one),
    // so the materials reach every shader a blit can use.
    _blitCompanionAssets = _blitCompanionBundle->LoadAllAssetsAsync<UnityEngine::Material*>();
    return;
  }
  if (!_blitCompanionAssets) return;
  if (!_blitCompanionAssets->get_isDone()) return;
  auto all = _blitCompanionAssets->get_allAssets();
  _blitCompanionAssets = nullptr;
  bool const current = _blitCompanionLoadingLevel == _selectedLevelPath;
  if (current && all) {
    for (auto object : all) {
      auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(object.unsafePtr()).value_or(nullptr);
      if (!IsAlive(material)) continue;
      auto* shader = material->get_shader().unsafePtr();
      if (!IsAlive(shader) || !shader->get_isSupported()) continue;
      shader->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
      _singleViewShaders.emplace(ShaderNameForLog(shader), shader);
    }
  }
  if (IsAlive(_blitCompanionBundle)) _blitCompanionBundle->Unload(false);
  _blitCompanionBundle = nullptr;
  if (!current) return;
  _blitCompanionLevel = _blitCompanionLoadingLevel;
  PaperLogger.info("Vivify Blit: {} single-view shader(s) ready for blits into screen textures ('{}')",
                   _singleViewShaders.size(), _blitCompanionLoadingPath);
}

void Runtime::ClearBlitCompanion() {
  for (auto const& [original, copy] : _singleViewBlitMaterials) {
    if (IsAlive(copy)) UnityEngine::Object::Destroy(copy);
  }
  _singleViewBlitMaterials.clear();
  // Let the next UnloadUnusedAssets take them.
  for (auto const& [name, shader] : _singleViewShaders) {
    if (IsAlive(shader)) shader->set_hideFlags(UnityEngine::HideFlags::None);
  }
  _singleViewShaders.clear();
  _blitCompanionLevel.clear();
}

// ---------------------------------------------------------------------------
// PC shaders for empty Quest shaders
//
// Unity sometimes fails to compile a shader for Android and ships it in the
// map's Quest bundle with no programs at all. Hold My Hand's raymarched
// AudioLink kaleidoscope (Custom/PoofShaders/Audio_Kaleidoscope/
// World_AudioLink_2.0) is one: its program store is four bytes, a count of
// zero, for both GLES and Vulkan, and no runtime can draw it from that bundle.
//
// The map's PC build has the same shader compiled for DirectX, which this mod
// translates. So before a level's Android bundle loads, its shaders are
// scanned; for any that shipped empty, the PC bundle (the song folder's, or
// downloaded by Info.dat's windows checksum) is converted as a PC-only map's
// would be, the PC versions of exactly those shaders are taken out of it, and
// materials whose shader is one of them are pointed at the PC version
// (RepairMaterialShader).
//
// The two bundles share their internal file names, and Unity refuses to have
// both loaded at once, so the converted PC bundle is loaded first, its shaders
// kept (DontUnloadUnusedAsset) and the bundle unloaded without destroying
// them, and only then is the Android bundle loaded.
// ---------------------------------------------------------------------------

void Runtime::BeginAndroidBundleLoad(std::string const& levelPath, std::string const& androidBundlePath) {
  _selectedBundlePath = androidBundlePath;
  int const generation = ++_graftGeneration;
  // Always, whatever the conversion settings (0.14.10): an empty shader draws
  // nothing, and its PC build is the only thing that can. Only turning shader
  // translation off stops it, since the PC build is DirectX without it.
  if (!GetTranslateShadersOnConversion()) {
    FinishAndroidBundleLoad(levelPath, androidBundlePath);
    return;
  }
  // The Quest bundle with the PC builds of its empty shaders merged in
  // (0.14.6), made once and cached like any converted bundle.
  std::string const merged = ConvertedBundlePath(androidBundlePath);
  RecordInterruptedLoad(merged);
  if (TranslationCrashedBefore(merged)) {
    // The merged shaders took the game down; standing the same programs in at
    // run time would too. The Quest bundle loads as it shipped.
    PaperLogger.warn("Vivify: the Quest bundle with PC shaders merged in crashed the game last time; loading "
                     "'{}' as it shipped", androidBundlePath);
    FinishAndroidBundleLoad(levelPath, androidBundlePath);
    return;
  }
  if (CachedConversionIsCurrent(merged)) {
    PaperLogger.info("Vivify: loading the Quest bundle with its broken shaders replaced by their PC builds: '{}'",
                     merged);
    FinishAndroidBundleLoad(levelPath, merged);
    return;
  }
  if (_graftLevelPath == levelPath && !_graftedShaders.empty()) {
    // Already stood in for this level this session.
    FinishAndroidBundleLoad(levelPath, androidBundlePath);
    return;
  }
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Checking shaders...");
  std::thread([this, generation, levelPath, androidBundlePath]() {
    auto const scan = BundleConvert::ScanShaders(androidBundlePath);
    std::vector<std::string> const empty = scan.emptyShaderNames;
    // The PC build to take the shaders from. The one built by the same Unity
    // version as the Quest bundle comes first: only its shader bodies can be
    // merged into the Quest bundle's file. Hold My Hand's song folder had only
    // the 2019 build beside a 2021 Quest bundle, so 0.14.6 could not merge and
    // never fetched the 2021 one. Never the Android bundle being loaded
    // (ResolvePcBundlePath is deliberately permissive and matched
    // bundleAndroid2021.vivify itself in 0.14.2).
    bool const quest2019 = scan.unityVersion.rfind("2019", 0) == 0;
    std::string const wantBuild = quest2019 ? "windows2019" : "windows2021";
    std::string const otherBuild = quest2019 ? "windows2021" : "windows2019";
    std::string const preferred =
        JoinPath(levelPath, quest2019 ? "bundleWindows2019.vivify" : "bundleWindows2021.vivify");
    std::string const other = JoinPath(levelPath, quest2019 ? "bundleWindows2021.vivify" : "bundleWindows2019.vivify");
    std::string preferredFound;
    std::string fallback;
    if (!empty.empty()) {
      if (FileExists(preferred)) preferredFound = preferred;
      if (FileExists(other)) fallback = other;
      if (fallback.empty()) {
        std::string const found = ResolvePcBundlePath(levelPath);
        if (!found.empty() && std::filesystem::path(found) != std::filesystem::path(androidBundlePath) &&
            std::filesystem::path(found) != std::filesystem::path(preferred)) {
          fallback = found;
        }
      }
    }
    BSML::MainThreadScheduler::Schedule([this, generation, levelPath, androidBundlePath, empty, preferred,
                                         preferredFound, fallback, wantBuild, otherBuild, other]() {
      if (generation != _graftGeneration || levelPath != _selectedLevelPath) return;
      if (empty.empty()) {
        FinishAndroidBundleLoad(levelPath, androidBundlePath);
        return;
      }
      std::string names;
      for (auto const& name : empty) names += (names.empty() ? "'" : ", '") + name + "'";
      PaperLogger.warn("Vivify: this map's Quest bundle ships {} shader(s) with no programs ({}); looking for "
                       "the map's PC build to stand in for them", empty.size(), names);
      if (!preferredFound.empty()) {
        ConvertPcForGraft(levelPath, androidBundlePath, preferredFound, empty);
        return;
      }
      // Download the matching build; a PC bundle of the other Unity version
      // can still stand in at load if that fails.
      uint32_t checksum = ReadBundleChecksumFromInfoDat(levelPath, wantBuild);
      std::string dest = preferred;
      if (checksum == 0) {
        if (!fallback.empty()) {
          ConvertPcForGraft(levelPath, androidBundlePath, fallback, empty);
          return;
        }
        checksum = ReadBundleChecksumFromInfoDat(levelPath, otherBuild);
        dest = other;
      }
      if (checksum == 0) {
        PaperLogger.warn("Vivify: no PC bundle in the song folder and no windows checksum in Info.dat; those "
                         "shaders stay undrawn");
        FinishAndroidBundleLoad(levelPath, androidBundlePath);
        return;
      }
      PaperLogger.info("Vivify: downloading the map's {} PC bundle for its shaders", dest == preferred ? wantBuild
                                                                                                        : otherBuild);
      SongCore::API::PlayButton::DisablePlayButton("Vivify", "Downloading PC shaders...");
      // A download that never calls back must not hold the play button:
      // CheckDownloadTimeout loads the Quest bundle as it is when this passes.
      _graftDeadline = UnityEngine::Time::get_realtimeSinceStartup() + 180.0f;  // PC bundles run to tens of MB
      _graftPendingLevel = levelPath;
      _graftPendingAndroid = androidBundlePath;
      DownloadBundleTo(checksum, dest, [this, generation, levelPath, androidBundlePath, dest, empty,
                                        fallback](bool ok) {
        if (generation != _graftGeneration || levelPath != _selectedLevelPath) return;
        _graftDeadline = -1.0f;
        if (ok) {
          ConvertPcForGraft(levelPath, androidBundlePath, dest, empty);
        } else if (!fallback.empty() && fallback != dest) {
          PaperLogger.warn("Vivify: the map's PC bundle could not be downloaded; using '{}' instead", fallback);
          ConvertPcForGraft(levelPath, androidBundlePath, fallback, empty);
        } else {
          PaperLogger.warn("Vivify: the map's PC bundle could not be downloaded; the empty shaders stay undrawn");
          FinishAndroidBundleLoad(levelPath, androidBundlePath);
        }
      });
    });
  }).detach();
}

namespace {

// The PC build of a Quest map's empty shaders is converted on its own,
// without the stereo split: its plain variants hold the multiview programs
// the headset draws. The split build (0.14.8's merge used the map's normal
// conversion) put single-view programs in the plain variants, and the merged
// kaleidoscope drew nothing; 0.14.7's stand-in at load, from an unsplit 2019
// build, drew it.
std::string UnsplitDonorPath(std::string const& pcBundlePath) {
  std::string path = ConvertedBundlePath(pcBundlePath);
  size_t const dot = path.rfind(".vivify");
  if (dot != std::string::npos) path.insert(dot, "_donor");
  return path;
}

bool DonorIsCurrent(std::string const& donorPath) {
  std::error_code ec;
  if (!std::filesystem::exists(donorPath, ec) || ec) return false;
  std::ifstream marker(ConversionMarkerPath(donorPath));
  int version = 0;
  return static_cast<bool>(marker >> version) && version == kBundleConversionVersion;
}

}  // namespace

void Runtime::ConvertPcForGraft(std::string const& levelPath, std::string const& androidBundlePath,
                                std::string const& pcBundlePath, std::vector<std::string> const& names) {
  // "Use Only PC Bundle For Missing Shaders" (0.14.24): the whole map is
  // played from its converted PC build instead of the Quest bundle with the
  // missing shaders patched in.
  if (GetUsePcBundleForMissingShaders()) {
    PaperLogger.info("Vivify: the Quest bundle '{}' is missing {} shader(s) its PC build has; playing the PC "
                     "build '{}' instead (Use Only PC Bundle For Missing Shaders)",
                     androidBundlePath, names.size(), pcBundlePath);
    _graftDeadline = -1.0f;
    ConvertPcBundleAsync(levelPath, pcBundlePath);
    return;
  }
  int const generation = _graftGeneration;
  std::string const donor = UnsplitDonorPath(pcBundlePath);
  std::string const merged = ConvertedBundlePath(androidBundlePath);
  bool const cached = DonorIsCurrent(donor);
  SongCore::API::PlayButton::DisablePlayButton("Vivify", cached ? "Merging PC shaders..." : "Converting PC shaders...");
  std::thread([this, generation, levelPath, androidBundlePath, pcBundlePath, donor, merged, names, cached]() {
    bool ok = cached;
    std::string message;
    if (!cached) {
      BundleConvert::ShaderConversionOptions options;
      options.separateStereoVariants = false;  // see UnsplitDonorPath
      auto const result = BundleConvert::ConvertShadersToGles(pcBundlePath, donor, options);
      ok = result.status == BundleConvert::Status::Success;
      message = result.message;
      if (ok) {
        std::ofstream marker(ConversionMarkerPath(donor), std::ios::out | std::ios::trunc);
        if (marker) marker << kBundleConversionVersion << " 0\n";
      }
    }
    // Merge the PC builds into the Quest bundle itself, so the materials load
    // with working shaders and nothing is swapped at run time. Only possible
    // between bundles of the same Unity version; otherwise the stand-in at
    // load below does the job.
    bool mergedOk = false;
    if (ok) {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(merged).parent_path(), ec);
      auto const merge = BundleConvert::MergeShadersInto(androidBundlePath, donor, names, merged);
      mergedOk = merge.status == BundleConvert::Status::Success;
      if (mergedOk) MarkConversionCurrent(merged);
      std::string skipped;
      for (auto const& line : merge.skipped) skipped += (skipped.empty() ? "" : "; ") + line;
      for (auto const& line : merge.notes) skipped += (skipped.empty() ? "" : "; ") + line;
      if (mergedOk) {
        PaperLogger.info("Vivify: {} ('{}' -> '{}', {} bytes){}{}", merge.message, androidBundlePath, merged,
                         merge.outputBytes, skipped.empty() ? "" : "; ", skipped);
      } else {
        PaperLogger.info("Vivify: the PC shaders are stood in at load instead of merged: {}{}{}", merge.message,
                         skipped.empty() ? "" : "; ", skipped);
      }
    }
    BSML::MainThreadScheduler::Schedule([this, generation, levelPath, androidBundlePath, donor, merged, names, ok,
                                         mergedOk, message]() {
      if (generation != _graftGeneration || levelPath != _selectedLevelPath) return;
      if (mergedOk) {
        FinishAndroidBundleLoad(levelPath, merged);
        return;
      }
      if (ok) {
        GraftShadersFrom(levelPath, donor, names);
      } else {
        PaperLogger.warn("Vivify: converting the map's PC bundle for its shaders failed: {}", message);
      }
      FinishAndroidBundleLoad(levelPath, androidBundlePath);
    });
  }).detach();
}

void Runtime::GraftShadersFrom(std::string const& levelPath, std::string const& convertedPath,
                               std::vector<std::string> const& names) {
  // Whatever bundle is loaded may share the converted one's internal names.
  if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    _mainBundle->Unload(true);
    _mainBundle = nullptr;
  }
  _preloadedBundlePath.clear();
  _graftedShaders.clear();
  _graftLevelPath = levelPath;
  _graftDonorPath = convertedPath;
  ScanBundleForSceneDepth(convertedPath);
  _graftApplied = 0;

  auto bundle = UnityEngine::AssetBundle::LoadFromFile(StringW(convertedPath));
  if (!IsAlive(bundle.unsafePtr())) {
    PaperLogger.warn("Vivify: the converted PC bundle '{}' did not load; the empty shaders stay undrawn",
                     convertedPath);
    return;
  }
  std::set<std::string> const wanted(names.begin(), names.end());
  std::set<std::string> found;
  std::set<std::string> seen;
  auto consider = [&](UnityEngine::Shader* shader) {
    if (!IsAlive(shader)) return;
    std::string const name = ShaderNameForLog(shader);
    seen.insert(name);
    if (!wanted.contains(name) || !found.insert(name).second) return;
    if (!shader->get_isSupported()) {
      PaperLogger.warn("Vivify: the PC build of '{}' did not translate for this GPU either", name);
      return;
    }
    shader->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
    _graftedShaders[name] = shader;
  };
  auto considerMaterial = [&](UnityEngine::Material* material) {
    if (IsAlive(material)) consider(material->get_shader().unsafePtr());
  };
  // Every asset, not only Shader ones: LoadAllAssets returns what the bundle
  // lists as its assets, and a map's shader is almost always pulled in
  // implicitly by the material or prefab that uses it, so asking for shaders
  // alone found none (0.14.2-0.14.4). Reading allAssets finishes the load on
  // the spot; the synchronous LoadAllAssets is stripped from this build.
  auto* request = bundle->LoadAllAssetsAsync();
  auto all = request != nullptr ? request->get_allAssets() : decltype(request->get_allAssets())(nullptr);
  if (all) {
    for (auto object : all) {
      auto* ptr = object.unsafePtr();
      if (!IsAlive(ptr)) continue;
      if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(ptr).value_or(nullptr)) {
        consider(shader);
      } else if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(ptr).value_or(nullptr)) {
        considerMaterial(material);
      } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(ptr).value_or(nullptr)) {
        auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
        for (int i = 0; i < renderers.size(); i++) {
          if (!IsAlive(renderers[i])) continue;
          auto materials = renderers[i]->get_sharedMaterials();
          if (!materials) continue;
          for (int j = 0; j < materials.size(); j++) considerMaterial(materials[j].unsafePtr());
        }
      }
    }
  }
  // Keeps the objects already loaded (the kept shaders among them); the rest
  // go with the next Resources.UnloadUnusedAssets.
  bundle->Unload(false);
  PaperLogger.info("Vivify: {} of {} empty shader(s) have a working PC build standing in ({} found in the PC "
                   "bundle, {} shader(s) seen there)", _graftedShaders.size(), names.size(), found.size(),
                   seen.size());
  if (found.size() < wanted.size()) {
    std::string missing;
    for (auto const& name : wanted) {
      if (!found.contains(name)) missing += (missing.empty() ? "'" : ", '") + name + "'";
    }
    std::string there;
    for (auto const& name : seen) there += (there.empty() ? "'" : ", '") + name + "'";
    PaperLogger.warn("Vivify: not in the PC bundle: {}; it uses: {}", missing, there.empty() ? "none" : there);
  }
}

void Runtime::FinishAndroidBundleLoad(std::string const& levelPath, std::string const& androidBundlePath) {
  if (levelPath != _selectedLevelPath) return;
  _selectedBundlePath = androidBundlePath;
  SongCore::API::PlayButton::EnablePlayButton("Vivify");
  PreloadBundle(androidBundlePath);
}

void Runtime::DownloadBundle(uint32_t checksum, std::string const& levelPath, std::function<void(bool)> callback) {
  DownloadBundleTo(checksum, JoinPath(levelPath, kBundleFile), std::move(callback));
}

void Runtime::DownloadBundleTo(uint32_t checksum, std::string const& destPath, std::function<void(bool)> callback) {
  std::string url = "https://repo.totalbs.dev/api/v1/bundles/" + std::to_string(checksum);
  std::string bundlePath = destPath;
  // Logged unconditionally: whether a bundle downloaded is the first thing
  // to know about a map that does not work.
  PaperLogger.info("Vivify bundle download: checksum={} metadataUrl='{}' saving to '{}'", checksum, url,
                   bundlePath);
  WebUtils::GetAsync<WebUtils::StringResponse>(WebUtils::URLOptions(url), [bundlePath, callback, url](WebUtils::StringResponse res) {
    if (!res.IsSuccessful() || !res.responseData.has_value()) {
      PaperLogger.warn("Vivify bundle download failed: metadata request unsuccessful url='{}' http={} curl={}",
                       url, res.get_HttpCode(), res.get_CurlStatus());
      BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
      return;
    }
    rapidjson::Document doc;
    doc.Parse(res.responseData->c_str());
    if (doc.HasParseError() || !doc.HasMember("downloadUrl") || !doc["downloadUrl"].IsString()) {
      PaperLogger.warn("Vivify bundle download failed: metadata response did not contain downloadUrl");
      BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
      return;
    }
    std::string downloadUrl = doc["downloadUrl"].GetString();
    PaperLogger.info("Vivify bundle download URL resolved: '{}'", downloadUrl);
    WebUtils::GetAsync<WebUtils::DataResponse>(WebUtils::URLOptions(downloadUrl), [bundlePath, callback](WebUtils::DataResponse dataRes) {
      if (!dataRes.IsSuccessful() || !dataRes.responseData.has_value()) {
        PaperLogger.warn("Vivify bundle download failed: data request unsuccessful path='{}' http={} curl={}",
                         bundlePath, dataRes.get_HttpCode(), dataRes.get_CurlStatus());
        BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
        return;
      }
      // Written beside the destination and renamed into place, and only if it
      // is an asset bundle at all: the destination is the song folder, which
      // outlives this mod's version. A download cut short by the game being
      // closed, or an error page served as a success, used to stay there as
      // the map's bundle -- and every version, older ones included, then
      // loaded it.
      auto const& bytes = *dataRes.responseData;
      static constexpr char kSignature[] = "UnityFS";
      bool const isBundle = bytes.size() > sizeof(kSignature) &&
                            std::memcmp(bytes.data(), kSignature, sizeof(kSignature) - 1) == 0;
      bool written = false;
      if (isBundle) {
        std::string const partPath = bundlePath + ".part";
        std::ofstream os(partPath, std::ios::binary | std::ios::trunc);
        if (os.is_open()) {
          os.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          os.close();
          std::error_code ec;
          if (os.good()) std::filesystem::rename(partPath, bundlePath, ec);
          written = os.good() && !ec;
          if (!written) std::filesystem::remove(partPath, ec);
        }
      } else {
        PaperLogger.warn("Vivify bundle download failed: '{}' is not an asset bundle ({} bytes); not saved",
                         bundlePath, bytes.size());
      }
      PaperLogger.info("Vivify bundle download complete: path='{}' bytes={} written={}",
                         bundlePath, bytes.size(), BoolText(written));
      BSML::MainThreadScheduler::Schedule([callback, written] { callback(written); });
    });
  });
}


// Why a bundle shader cannot draw on this device.
//
// The port used to record a single bit per shader -- Shader.isSupported -- which
// collapses two completely different failures into one indistinguishable
// "unsupported", and every question about a broken map ("is this a geometry
// shader problem?") stalls on not being able to tell them apart:
//
//   NoProgram       the bundle carries no shader program for this platform at
//                   all. This is every shader in a converted PC bundle: the
//                   archive was built by Unity for Windows, so its programs are
//                   DirectX bytecode. Conversion rewrites the container's target
//                   platform so the assets, meshes and materials load, but it
//                   cannot invent GLES/Vulkan programs that were never compiled.
//                   No device could run these, and no setting changes that.
//
//   DeviceRejected  the bundle does carry programs -- so it was built for
//                   Android -- but this GPU accepts none of the subshaders.
//                   THIS is the bucket a geometry-shader shader lands in.
//                   Unity records each subshader's hardware requirements and
//                   refuses the ones the device cannot meet; a geometry stage is
//                   unavailable under Vulkan on Adreno (Qualcomm has never
//                   exposed VkPhysicalDeviceFeatures.geometryShader), though it
//                   is available under OpenGL ES 3.2 via GL_EXT_geometry_shader.
//
// Unity picks the highest-LOD subshader whose requirements the device meets, so
// a shader that ships a geometry-shader subshader *and* a plain one is already
// handled automatically and lands in Runnable. Only a shader whose every
// subshader needs a stage this device lacks reaches DeviceRejected.
namespace {
enum class ShaderVerdict { Runnable, NoProgram, DeviceRejected, Dead };

ShaderVerdict ClassifyBundleShader(UnityEngine::Shader* shader) {
  if (!IsManagedAlive(shader)) return ShaderVerdict::Dead;
  if (shader->get_isSupported()) return ShaderVerdict::Runnable;
  // subshaderCount counts the subshaders that survived compilation *for this
  // build target*. Zero means the serialised shader has no program for Android,
  // which is the converted-PC-bundle case; non-zero means programs exist and the
  // device turned every one of them down.
  return shader->get_subshaderCount() > 0 ? ShaderVerdict::DeviceRejected : ShaderVerdict::NoProgram;
}
}  // namespace

void Runtime::CacheBundleAssets() {
  if (_mainBundle == nullptr || !UnityEngine::Object::op_Implicit_bool(_mainBundle)) return;
  auto assetNames = _mainBundle->GetAllAssetNames();
  if (!assetNames) return;
  _assets.clear();
  _assetsByName.clear();
  _supportedShadersByName.clear();
  int shadersSeen = 0;
  int shadersRunnable = 0;
  int shadersNoProgram = 0;
  int shadersDeviceRejected = 0;
  std::vector<std::string> deviceRejectedNames;
  for (auto assetName : assetNames) {
    if (!assetName) continue;
    std::string originalAssetPath = il2cpp_utils::detail::to_string(assetName);
    std::string key = NormalizeAssetKey(originalAssetPath);

    // One bad asset must not take the whole bundle (or the game) down. A
    // converted PC bundle in particular carries DirectX shader programs and
    // BC/DXT texture data that this GPU cannot consume, and those surface here
    // as the asset is realised.
    UnityEngine::Object* asset = nullptr;
    try {
      asset = _mainBundle->LoadAsset(assetName).unsafePtr();
    } catch (std::exception const& ex) {
      PaperLogger.warn("Vivify asset load threw, skipping: path='{}' error={}", originalAssetPath, ex.what());
      continue;
    } catch (...) {
      PaperLogger.warn("Vivify asset load threw, skipping: path='{}'", originalAssetPath);
      continue;
    }
    if (!IsAlive(asset)) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify asset load failed: path='{}'", originalAssetPath);
      }
      continue;
    }
    if (!key.empty()) _assets[key] = asset;
    auto name = asset->get_name();
    if (name) {
      auto nameKey = NormalizeAssetKey(il2cpp_utils::detail::to_string(name));
      if (!nameKey.empty() && !_assetsByName.contains(nameKey)) {
        _assetsByName[nameKey] = asset;
      }
      if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr);
          IsAlive(shader)) {
        shadersSeen++;
        switch (ClassifyBundleShader(shader)) {
          case ShaderVerdict::Runnable:
            shadersRunnable++;
            if (!nameKey.empty()) _supportedShadersByName[nameKey] = shader;
            break;
          case ShaderVerdict::NoProgram:
            shadersNoProgram++;
            break;
          case ShaderVerdict::DeviceRejected:
            shadersDeviceRejected++;
            // Named individually: this is the bucket that a map author can act
            // on, by shipping a subshader without the stage this device lacks.
            if (deviceRejectedNames.size() < 12) {
              deviceRejectedNames.push_back(fmt::format("'{}' (subshaders={} passes={} maxLOD={})",
                                                        ShaderNameForLog(shader),
                                                        shader->get_subshaderCount(),
                                                        shader->get_passCount(),
                                                        shader->get_maximumLOD()));
            }
            break;
          case ShaderVerdict::Dead:
            break;
        }
      }
    }
    if (GetVivifyDebugLogging()) {
      if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
          IsAlive(material)) {
        LogMaterialShader("bundle-load", originalAssetPath, material);
      } else if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr);
                 IsAlive(shader)) {
        PaperLogger.info("Vivify shader asset: path='{}' shader='{}' supported={} subshaders={}",
                         originalAssetPath, ShaderNameForLog(shader), BoolText(shader->get_isSupported()),
                         shader->get_subshaderCount());
      }
    }
  }

  LogBundleShaderAudit(shadersSeen, shadersRunnable, shadersNoProgram, shadersDeviceRejected,
                       deviceRejectedNames);
}

// One unconditional verdict per bundle, so a report of "the map is invisible" or
// "geometry shaders don't work" can be answered from the log without a repro.
void Runtime::LogBundleShaderAudit(int seen, int runnable, int noProgram, int deviceRejected,
                                   std::vector<std::string> const& deviceRejectedNames) {
  _auditShadersSeen = seen;
  _auditShadersRunnable = runnable;
  _auditShadersNoProgram = noProgram;
  _auditShadersDeviceRejected = deviceRejected;
  _auditRejectedNames = deviceRejectedNames;
  if (seen == 0) return;

  bool const converted = !_preloadedBundlePath.empty() &&
                         _preloadedBundlePath.rfind(ConvertedBundleCacheDir(), 0) == 0;
  PaperLogger.info(
      "Vivify shaders: bundle='{}' converted={} total={} runnable={} noAndroidProgram={} deviceRejected={}",
      _preloadedBundlePath, BoolText(converted), seen, runnable, noProgram, deviceRejected);

  if (noProgram > 0) {
    PaperLogger.warn(
        "Vivify: {} of {} shaders in this bundle carry no Android program, so they cannot run on any Quest. "
        "{} Materials using them fall back to stand-in shading, which keeps the models visible but loses their "
        "real shading -- raymarching, post-processing and geometry-stage effects included.",
        noProgram, seen,
        converted ? "This bundle was converted from a PC build: its shader programs are DirectX bytecode, which "
                    "conversion cannot translate. Only a bundle built for Android carries runnable programs."
                  : "The bundle was not built for Android.");
  }

  if (deviceRejected > 0) {
    // Programs exist, so the bundle really was built for Android and the GPU is
    // the one refusing. A geometry or tessellation stage is the usual reason.
    PaperLogger.warn(
        "Vivify: {} of {} shaders were built for Android but this GPU accepts none of their subshaders. "
        "A shader stage the device lacks is the usual cause -- under Vulkan on Adreno there is no geometry or "
        "tessellation stage at all. A shader that also ships a subshader without that stage is selected "
        "automatically and does not appear here.",
        deviceRejected, seen);
    for (auto const& name : deviceRejectedNames) {
      PaperLogger.warn("Vivify shader rejected by device: {}", name);
    }
    if (deviceRejected > static_cast<int>(deviceRejectedNames.size())) {
      PaperLogger.warn("Vivify: ... and {} more",
                       deviceRejected - static_cast<int>(deviceRejectedNames.size()));
    }
  }
}

// Formats the on-device report for the current level.
//
// Written twice per level -- once at load, once at the end -- so that a level
// which never finishes still leaves the load block behind. Everything a
// "why is this map broken" question needs is here, because asking a player to
// retrieve paperlog output is not reasonable.
std::string Runtime::BuildLevelReport(std::string_view outcome) const {
  std::string text;
  auto line = [&text](std::string const& value) { text += value + "\n"; };
  auto yesNo = [](bool value) { return value ? "yes" : "no"; };

  line(fmt::format("Vivify {}   outcome: {}", VERSION, outcome));
  line(fmt::format("Device:  {}", _graphicsSummary.empty() ? std::string("(not yet probed)") : _graphicsSummary));
  line("");

  line("Level");
  line(fmt::format("  folder:        {}", _selectedLevelPath.empty() ? std::string("(none)") : _selectedLevelPath));
  line(fmt::format("  bundle loaded: {}", _preloadedBundlePath.empty() ? std::string("(none)") : _preloadedBundlePath));
  line(fmt::format("  converted:     {}",
                   yesNo(!_preloadedBundlePath.empty() &&
                         _preloadedBundlePath.rfind(ConvertedBundleCacheDir(), 0) == 0)));
  line("");

  // The numbers to read first for a freeze: all of this runs on the main thread
  // while the level loads.
  line("Level load timings (main thread)");
  line(fmt::format("  cache bundle assets: {:8.0f} ms", _loadMsCacheAssets));
  line(fmt::format("  decode textures:     {:8.0f} ms", _loadMsDecodeTextures));
  line(fmt::format("  repair shaders:      {:8.0f} ms", _loadMsRepairShaders));
  line(fmt::format("  total:               {:8.0f} ms", _loadMsTotal));
  line("");

  line("Frame watchdog");
  line(fmt::format("  worst frame:        {:8.1f} ms", _worstFrameMs));
  line(fmt::format("  stood down:         {}", yesNo(_selfDisabledThisLevel)));
  if (_selfDisabledThisLevel) {
    line("  Vivify disabled itself for this level after a sustained run of slow");
    line("  frames, so the game kept running instead of freezing.");
  }
  line("");

  line("Shaders in the loaded bundle");
  line(fmt::format("  total:            {}", _auditShadersSeen));
  line(fmt::format("  runnable here:    {}", _auditShadersRunnable));
  line(fmt::format("  no Android build: {}  (DirectX-only; cannot run on any Quest)", _auditShadersNoProgram));
  line(fmt::format("  refused by GPU:   {}  (built for Android, this GPU took none of them)",
                   _auditShadersDeviceRejected));
  for (auto const& name : _auditRejectedNames) {
    line(fmt::format("    refused: {}", name));
  }
  line("");

  line("Shader repair");
  line(fmt::format("  unusable materials: {}", _shaderRepairAttempts));
  line(fmt::format("  given a stand-in:   {}", _shaderRepairSucceeded));
  line(fmt::format("  left broken:        {}", _shaderRepairFailed));
  line("");

  line("Textures");
  line(fmt::format("  decoded:            {}", _texturesDecoded));
  line(fmt::format("  skipped (over time budget): {}", _texturesSkipped));
  line(fmt::format("  refused, not readable on CPU: {}", _texturesUnreadable));
  line(fmt::format("  refused, decoded blank:       {}", _texturesBlank));

  if (!_sourceBundleScanText.empty()) {
    line("");
    line("Source bundle before conversion");
    line(fmt::format("  {}", _sourceBundleScanText));
  }
  return text;
}

void Runtime::WriteLevelStartReport() {
  _levelReportOpen = true;
  Report::Append("LEVEL STARTED", BuildLevelReport("still playing (this block is written at load)"));
}

void Runtime::WriteLevelEndReport(std::string_view outcome) {
  // Only for levels that actually opened a report, so leaving the menu does not
  // append an empty block every time something calls ResetRuntime.
  if (!_levelReportOpen) return;
  _levelReportOpen = false;
  Report::Append("LEVEL ENDED", BuildLevelReport(outcome));
}

void Runtime::PreloadBundle(std::string const& bundlePath) {
  if (_preloadedBundlePath == bundlePath && _mainBundle != nullptr &&
      UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle already preloaded: '{}'", bundlePath);
    }
    return;
  }
  if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    _mainBundle->Unload(true);
    _mainBundle = nullptr;
  }
  _preloadedBundlePath = bundlePath;
  if (TranslationCrashedBefore(bundlePath)) {
    PaperLogger.warn("Vivify: '{}' is in the crash guard's grey fallback (converted without shader "
                     "translation after two loads that never finished). Force Reconvert All, or turning Crash "
                     "Guard off, gives it its translated shaders back", bundlePath);
  }
  ArmLoadGuard(bundlePath);
  _mainBundle = UnityEngine::AssetBundle::LoadFromFile(StringW(bundlePath));
  if (_mainBundle == nullptr) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle preload failed: '{}'", bundlePath);
    }
    _preloadedBundlePath.clear();
    DisarmLoadGuard();
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify bundle preloaded: '{}'", bundlePath);
  }
  CacheBundleAssets();
  // Loaded and every asset realised. Play re-arms it, since that is when the
  // driver first compiles the programs.
  DisarmLoadGuard();
  StartShaderWarmup(bundlePath);
  ScanBundleForSceneDepth(bundlePath);
}

// ---------------------------------------------------------------------------
// Scene depth only for maps that read it (0.14.21)
//
// The main camera's depth texture costs a second pass over the whole scene
// every frame. Every Vivify map used to get it, on the belief that raymarchers
// such as Hold My Hand's needed it; a scan of Hold My Hand's bundles (its own
// Quest build and its converted PC build) found no shader that samples it.
// Shaders name it in their program text whatever the platform, so a scan of
// the map's bundles tells whether any shader can read it. Until a scan has
// answered, the depth texture stays on, as before.
// ---------------------------------------------------------------------------

namespace {
std::string DepthScanKey(std::string const& path) {
  std::error_code ec;
  auto const time = std::filesystem::last_write_time(path, ec);
  auto const size = std::filesystem::file_size(path, ec);
  return path + "|" + std::to_string(ec ? 0 : static_cast<long long>(time.time_since_epoch().count())) + "|" +
         std::to_string(ec ? 0 : size);
}
}

void Runtime::ScanBundleForSceneDepth(std::string const& bundlePath) {
  if (bundlePath.empty()) return;
  std::string const key = DepthScanKey(bundlePath);
  {
    std::lock_guard lock(_depthScanMutex);
    if (_depthScanResults.contains(key) || !_depthScansRunning.insert(key).second) return;
  }
  std::thread([this, bundlePath, key]() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
    bool needed = true;
    bool known = false;
    std::string shaders;
    try {
      auto const scan = BundleConvert::ScanShaders(bundlePath);
      if (scan.parsed && scan.undecodableShaders == 0) {
        known = true;
        needed = scan.samplesCameraDepth;
        for (auto const& name : scan.cameraDepthShaderNames) shaders += (shaders.empty() ? "" : ", ") + name;
      }
    } catch (...) {
    }
    PaperLogger.info("Vivify scene depth scan: '{}' -> {}{}", bundlePath,
                     !known ? "unknown (depth stays on)" : (needed ? "needed by " : "not needed"), shaders);
    std::lock_guard lock(_depthScanMutex);
    _depthScansRunning.erase(key);
    // An unreadable bundle is recorded as needing depth, so it is not
    // rescanned on every load.
    _depthScanResults[key] = needed;
  }).detach();
}

std::optional<bool> Runtime::MapShadersNeedSceneDepth() {
  bool const grafted = !_graftedShaders.empty() && !_graftDonorPath.empty();
  std::string const sourcesFor = _preloadedBundlePath + "\n" + (grafted ? _graftDonorPath : std::string());
  if (sourcesFor != _depthSourcesFor) {
    _depthSourcesFor = sourcesFor;
    _depthDecision.reset();
    _depthSourceKeys.clear();
    if (!_preloadedBundlePath.empty()) _depthSourceKeys.push_back(DepthScanKey(_preloadedBundlePath));
    if (grafted) _depthSourceKeys.push_back(DepthScanKey(_graftDonorPath));
  }
  if (_depthDecision.has_value() || _depthSourceKeys.empty()) return _depthDecision;
  bool needed = false;
  std::lock_guard lock(_depthScanMutex);
  for (auto const& key : _depthSourceKeys) {
    auto found = _depthScanResults.find(key);
    if (found == _depthScanResults.end()) return std::nullopt;
    needed = needed || found->second;
  }
  _depthDecision = needed;
  return _depthDecision;
}

// ---------------------------------------------------------------------------
// Prepare Shaders Before Playing
//
// The GPU driver compiles a shader variant the first time something draws
// with it. A converted PC map has hundreds of translated variants, and all of
// that used to land on the first frames of the song: a 0.14.7 log shows the
// game stalled for 88 seconds at song start, long enough to be closed as hung
// (and the crash guard then made the map grey). Here the same compiling is
// done in the menu, right after the map is selected and its bundle loaded:
// every material the bundle holds is used once, a couple a frame, with and
// without the stereo keyword the eye cameras turn on. The play button says
// how far it has got.
// ---------------------------------------------------------------------------

void Runtime::StartShaderWarmup(std::string const& bundlePath) {
  _warmupQueue.clear();
  _warmupIndex = 0;
  if (!GetPrepareShadersBeforePlaying()) return;
  if (bundlePath.rfind(ConvertedBundleCacheDir(), 0) != 0) return;  // only bundles this mod translated
  std::set<UnityEngine::Material*> seen;
  auto add = [&](UnityEngine::Material* material) {
    if (IsAlive(material) && seen.insert(material).second) _warmupQueue.push_back(material);
  };
  for (auto const& [path, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr)) {
      add(material);
    } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr)) {
      auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
      for (int i = 0; i < renderers.size(); i++) {
        if (!IsAlive(renderers[i])) continue;
        auto materials = renderers[i]->get_sharedMaterials();
        if (!materials) continue;
        for (int j = 0; j < materials.size(); j++) add(materials[j].unsafePtr());
      }
    }
  }
  if (_warmupQueue.empty()) return;
  _warmupLevel = _selectedLevelPath;
  _warmupBundle = bundlePath;
  _warmupStarted = UnityEngine::Time::get_realtimeSinceStartup();
  // A driver crash while compiling counts against the bundle like one in play.
  ArmLoadGuard(bundlePath);
  SongCore::API::PlayButton::DisablePlayButton(
      "Vivify", "Preparing shaders 0/" + std::to_string(_warmupQueue.size()) + "...");
  PaperLogger.info("Vivify: preparing the shaders of {} material(s) before play ('{}')", _warmupQueue.size(),
                   bundlePath);
}

void Runtime::PollShaderWarmup() {
  if (_warmupQueue.empty()) return;
  if (_warmupLevel != _selectedLevelPath) {
    // Another level was picked; its own load starts its own preparation.
    _warmupQueue.clear();
    _warmupIndex = 0;
    return;
  }
  // The stereo keyword the eye cameras turn on, so the variants they use are
  // compiled too (a split 2021 shader keeps its two-eye program under it).
  std::optional<UnityEngine::Rendering::GlobalKeyword> multiview;
  if (auto keywords = UnityEngine::Shader::GetAllGlobalKeywords()) {
    for (auto const& keyword : keywords) {
      if (keyword.m_Name && std::string(keyword.m_Name) == "STEREO_MULTIVIEW_ON") multiview = keyword;
    }
  }
  // Slowly: two materials a frame, so the menu keeps drawing.
  constexpr size_t kPerFrame = 2;
  for (size_t n = 0; n < kPerFrame && _warmupIndex < _warmupQueue.size(); n++) {
    auto* material = _warmupQueue[_warmupIndex++];
    if (!IsAlive(material)) continue;
    try {
      int const passes = material->get_passCount();
      for (int p = 0; p < passes; p++) material->SetPass(p);
      if (multiview.has_value()) {
        auto keyword = multiview.value();
        bool const was = UnityEngine::Shader::IsKeywordEnabled(::ByRef<UnityEngine::Rendering::GlobalKeyword>(keyword));
        if (!was) UnityEngine::Shader::EnableKeyword(::ByRef<UnityEngine::Rendering::GlobalKeyword>(keyword));
        for (int p = 0; p < passes; p++) material->SetPass(p);
        if (!was) UnityEngine::Shader::DisableKeyword(::ByRef<UnityEngine::Rendering::GlobalKeyword>(keyword));
      }
    } catch (...) {
      // One material that will not bind is no reason to stop.
    }
  }
  if (_warmupIndex < _warmupQueue.size()) {
    if (_warmupIndex % 10 == 0 || _warmupIndex < 3) {
      SongCore::API::PlayButton::DisablePlayButton("Vivify", "Preparing shaders " + std::to_string(_warmupIndex) +
                                                                 "/" + std::to_string(_warmupQueue.size()) + "...");
    }
    return;
  }
  float const seconds = UnityEngine::Time::get_realtimeSinceStartup() - _warmupStarted;
  PaperLogger.info("Vivify: shaders of {} material(s) prepared in {:.1f}s", _warmupQueue.size(), seconds);
  _warmupQueue.clear();
  _warmupIndex = 0;
  DisarmLoadGuard();
  SongCore::API::PlayButton::EnablePlayButton("Vivify");
}

void Runtime::ArmLoadGuard(std::string const& bundlePath) {
  DisarmLoadGuard();
  if (bundlePath.rfind(ConvertedBundleCacheDir(), 0) != 0) return;
  std::ofstream marker(LoadingMarkerPath(bundlePath), std::ios::out | std::ios::trunc);
  if (!marker) return;
  marker << kBundleConversionVersion << "\n";
  marker.close();
  _loadGuardPath = bundlePath;
}

void Runtime::DisarmLoadGuard() {
  if (_loadGuardPath.empty()) return;
  std::error_code ec;
  std::filesystem::remove(LoadingMarkerPath(_loadGuardPath), ec);
  // Loaded fine: a single earlier strike is forgiven, so two have to come in
  // a row. A bundle already reconverted without translation keeps its record.
  if (!TranslationCrashedBefore(_loadGuardPath)) std::filesystem::remove(CrashedMarkerPath(_loadGuardPath), ec);
  _loadGuardPath.clear();
}

void Runtime::LoadMainBundle() {
  LogUnityPlatformInfoOnce();
  if (_selectedLevelPath.empty()) {
    if (_selectedMapHasVivifyRequirement && GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle load skipped: selected level path is empty");
    }
    return;
  }
  // Level selection may have settled on a bundle outside the song folder (a
  // downloaded one, or one converted on device), so that choice wins over
  // re-scanning the folder.
  std::string bundlePath = _selectedBundlePath;
  if (bundlePath.empty() || !std::filesystem::exists(bundlePath)) {
    bundlePath = ResolveBundlePath(_selectedLevelPath);
  }
  if (bundlePath.empty()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle not found in '{}' (no *.vivify file)", _selectedLevelPath);
    }
    return;
  }
  // Held through the first seconds of play (Update disarms it), which is when
  // the GPU driver compiles the translated programs for the first time.
  ArmLoadGuard(bundlePath);
  if (!_preloadedBundlePath.empty() && _preloadedBundlePath == bundlePath &&
      _mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle preloaded, rebuilding asset caches: '{}'", bundlePath);
    }
    CacheBundleAssets();
    DecodeUnsupportedBundleTextures();
    RepairLoadedMaterialShaders();
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify loading asset bundle: path='{}'", bundlePath);
  }
  _mainBundle = UnityEngine::AssetBundle::LoadFromFile(StringW(bundlePath));
  if (_mainBundle == nullptr) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify asset bundle load failed: path='{}'", bundlePath);
    }
    return;
  }
  _preloadedBundlePath = bundlePath;

  // Time each phase unconditionally. All of this runs on the main thread while
  // the level is loading, so when someone reports a freeze these three numbers
  // say which phase to look at without needing a repro.
  auto phase = [](char const* name, auto&& work) {
    auto const start = std::chrono::steady_clock::now();
    work();
    double const ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 250.0) {
      PaperLogger.warn("Vivify level load: {} took {:.0f}ms", name, ms);
    } else {
      PaperLogger.info("Vivify level load: {} took {:.0f}ms", name, ms);
    }
    return ms;
  };

  _loadMsCacheAssets = phase("cache bundle assets", [this]() { CacheBundleAssets(); });
  _loadMsDecodeTextures = phase("decode textures", [this]() { DecodeUnsupportedBundleTextures(); });
  _loadMsRepairShaders = phase("repair shaders", [this]() { RepairLoadedMaterialShaders(); });
  _loadMsTotal = _loadMsCacheAssets + _loadMsDecodeTextures + _loadMsRepairShaders;
  PaperLogger.info("Vivify level load: {:.0f}ms total", _loadMsTotal);

  // Written now, not at the end of the level: if this map is about to freeze,
  // this block is the only one that will ever reach disk.
  WriteLevelStartReport();
}

UnityEngine::Object* Runtime::LookUpAsset(std::string_view assetName) const {
  auto key = NormalizeAssetKey(assetName);
  if (auto it = _assets.find(key); it != _assets.end()) {
    return it->second;
  }
  if (auto nameIt = _assetsByName.find(key); nameIt != _assetsByName.end()) {
    return nameIt->second;
  }
  return nullptr;
}

UnityEngine::Object* Runtime::GetAssetObject(std::string_view assetName) const {
  auto* asset = LookUpAsset(assetName);
  if (asset == nullptr && GetVivifyDebugLogging()) {
    PaperLogger.warn("Vivify asset lookup miss: '{}'", std::string(assetName));
  }
  return asset;
}

// Names the graphics API, which decides whether a shader stage like a geometry
// shader can exist on this device at all.
//
// Adreno exposes GL_EXT_geometry_shader under OpenGL ES 3.2, but reports
// VkPhysicalDeviceFeatures.geometryShader as false under Vulkan -- Qualcomm has
// never supported geometry or tessellation stages in their Vulkan driver. So on
// Vulkan a geometry shader cannot run here no matter what the bundle contains,
// and no mod-side setting changes that: the graphics API is baked into the
// game's APK at build time.
std::string_view GraphicsApiName(int32_t graphicsDeviceType) {
  switch (graphicsDeviceType) {
    case 0x0b: return "OpenGLES3";
    case 0x10: return "Metal";
    case 0x11: return "OpenGLCore";
    case 0x15: return "Vulkan";
    default: return "other";
  }
}

void Runtime::LogUnityPlatformInfoOnce() {
  if (_loggedUnityPlatformInfo) return;
  _loggedUnityPlatformInfo = true;
  auto stereoMode = UnityEngine::XR::XRSettings::get_stereoRenderingMode();
  auto graphicsType = UnityEngine::SystemInfo::get_graphicsDeviceType();
  int const shaderLevel = UnityEngine::SystemInfo::get_graphicsShaderLevel();

  // Shader level is reported as 10x the shader model: 45 is SM4.5. Geometry
  // stages need SM4.0, so anything below 40 rules them out outright; at or
  // above 40 it comes down to the API above.
  _graphicsSummary = fmt::format(
      "api={} ({}) shaderLevel={} (SM{}.{}) geometryShaderStagePossible={} gpu='{}'",
      GraphicsApiName(graphicsType.value__), graphicsType.value__, shaderLevel, shaderLevel / 10,
      shaderLevel % 10,
      BoolText(shaderLevel >= 40 && graphicsType.value__ != 0x15),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceName()));
  PaperLogger.info("Vivify graphics: {}", _graphicsSummary);

  if (!GetVivifyDebugLogging()) return;

  PaperLogger.info(
      "Vivify platform: os='{}' device='{}' gpu='{}' vendor='{}' api={} stereoMode={} xrOcclusionMesh={} supportsInstancing={} supportsR8={} supportsDepthRT={}",
      ToStdString(UnityEngine::SystemInfo::get_operatingSystem()),
      ToStdString(UnityEngine::SystemInfo::get_deviceModel()),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceName()),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceVendor()),
      graphicsType.value__,
      stereoMode.value__,
      BoolText(UnityEngine::XR::XRSettings::get_useOcclusionMesh()),
      BoolText(UnityEngine::SystemInfo::get_supportsInstancing()),
      BoolText(UnityEngine::SystemInfo::SupportsRenderTextureFormat(UnityEngine::RenderTextureFormat::R8)),
      BoolText(UnityEngine::SystemInfo::SupportsRenderTextureFormat(UnityEngine::RenderTextureFormat::Depth)));
}

UnityEngine::RenderTextureFormat Runtime::SupportedRenderTextureFormat(UnityEngine::RenderTextureFormat requested,
                                                                       std::string_view context) const {
  if (UnityEngine::SystemInfo::SupportsRenderTextureFormat(requested)) {
    return requested;
  }
  auto fallback = UnityEngine::RenderTextureFormat::ARGB32;
  if (GetVivifyDebugLogging()) {
    PaperLogger.warn("Vivify RT format unsupported: context={} requested={} fallback={}",
                     context, requested.value__, fallback.value__);
  }
  return fallback;
}

void Runtime::LogMaterialShader(std::string_view context, std::string_view assetPath, UnityEngine::Material* material) const {
  if (!GetVivifyDebugLogging()) return;
  if (!IsAlive(material)) {
    PaperLogger.warn("Vivify material missing: context={} asset={}", context, assetPath);
    return;
  }
  auto* shader = material->get_shader().unsafePtr();
  auto shaderName = ShaderNameForLog(shader);
  PaperLogger.info("Vivify material: context={} asset={} material='{}' shader='{}' supported={} internalError={}",
                   context,
                   assetPath,
                   ToStdString(material->get_name()),
                   shaderName,
                   BoolText(IsAlive(shader) && shader->get_isSupported()),
                   BoolText(IsInternalErrorShaderName(shaderName)));
}

// Indexes every supported shader loaded in this process by name, once per level.
//
// This is what makes a map's own shader names mean something. A PC bundle does
// not only carry shaders the map author wrote: it carries a *copy* of every
// shader its materials referenced, including the game's own and Unity's
// built-ins, each compiled for DirectX and useless here. A real session log
// shows the result:
//
//   material='snail' shader='BeatSaber/Standard' supported=false
//   material='tube'  shader='BeatSaber/Tube_OptimizedNoise_FastMath' supported=false
//   material='Disco_Lights' shader='Legacy Shaders/Particles/Additive' supported=false
//
// Beat Saber has BeatSaber/Standard. Unity has Legacy Shaders/Particles/
// Additive. Both are sitting in the process, working, and the map wants exactly
// them -- but Shader.Find returns the bundle's broken copy of the same name, it
// fails the isSupported test, and the material was handed a generic stand-in
// instead of the shader it actually asked for.
//
// Scanning for a shader that both matches the name and runs finds the real one.
void Runtime::EnsureGameShaderIndex() {
  if (_gameShaderIndexBuilt) return;
  _gameShaderIndexBuilt = true;

  auto allShaders = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Shader*>();
  if (!allShaders) return;
  for (int i = 0; i < allShaders.size(); i++) {
    auto* candidate = allShaders[i];
    // Only shaders that run are indexed, so a bundle's dead copy of a name can
    // never shadow the working one.
    if (!IsAlive(candidate) || !candidate->get_isSupported()) continue;
    auto name = candidate->get_name();
    if (!name) continue;
    std::string key = NormalizeAssetKey(ToStdString(name));
    if (key.empty()) continue;
    _gameShadersByName.emplace(std::move(key), candidate);
  }
  PaperLogger.info("Vivify shader index: {} runnable shader name(s) available in this process",
                   _gameShadersByName.size());
  // Which shaders a Quest build actually ships decides which stand-in Vivify can
  // pick, and that list is not knowable from a PC checkout -- it has to come off
  // a headset. Logging the names once per level costs one line and makes a
  // session log enough to tune the fallback ordering.
  if (!_gameShadersByName.empty()) {
    std::string names;
    for (auto const& entry : _gameShadersByName) {
      if (!names.empty()) names += ", ";
      names += entry.first;
      // One line, not a log flood: the tail is only ever more of the same.
      if (names.size() > 4000) {
        names += ", ...";
        break;
      }
    }
    PaperLogger.info("Vivify shader index contents: {}", names);
  }
}

UnityEngine::Shader* Runtime::FindUsableShader(std::string const& shaderName) {
  if (shaderName.empty()) return nullptr;
  auto const key = NormalizeAssetKey(shaderName);
  if (auto it = _supportedShadersByName.find(key);
      it != _supportedShadersByName.end() && IsAlive(it->second) && it->second->get_isSupported()) {
    return it->second;
  }
  // The map asked for this shader by name and something in the process answers
  // to it. That is a far better answer than a generic stand-in: same name means
  // same properties, so the material's colours and textures land where they
  // were meant to.
  EnsureGameShaderIndex();
  if (auto it = _gameShadersByName.find(key);
      it != _gameShadersByName.end() && IsAlive(it->second) && it->second->get_isSupported()) {
    return it->second;
  }
  // Deliberately the quiet lookup: this searches by *shader* name
  // ("Swifter/VFX/Star"), and the asset maps are keyed by asset path and file
  // name, so a miss here is the normal case rather than a problem. Routing it
  // through GetAssetObject logged one "asset lookup miss" warning per attempt
  // -- 332 of them in a single session, one for every shader the repair pass
  // tried to find a supported twin for.
  auto* bundled = il2cpp_utils::try_cast<UnityEngine::Shader>(LookUpAsset(shaderName)).value_or(nullptr);
  if (IsAlive(bundled) && bundled->get_isSupported()) {
    return bundled;
  }
  auto found = UnityEngine::Shader::Find(StringW(shaderName));
  auto* foundShader = found.unsafePtr();
  if (IsAlive(foundShader) && foundShader->get_isSupported()) {
    return foundShader;
  }
  return nullptr;
}

// Picks a shader that can stand in for one the GPU cannot run.
//
// This used to ask Shader.Find for "Unlit/Texture", "Unlit/Color",
// "Sprites/Default" and "Standard". Shader.Find only resolves shaders that are
// actually included in the build (or already loaded from a bundle), and Unity
// strips built-in shaders nothing references -- so in Beat Saber's IL2CPP build
// every one of those lookups returns null, the repair silently gave up, and the
// material kept a shader that draws nothing. That is why converted bundles came
// up with no models: the meshes and renderers were all there, but every
// material was bound to a dead shader.
//
// Enumerating the shaders the process has actually loaded finds something real.
UnityEngine::Shader* Runtime::FindFallbackShader() {
  if (IsAlive(_fallbackShader) && _fallbackShader->get_isSupported()) {
    return _fallbackShader;
  }
  // A failed search has to be remembered too. This walks every shader object
  // loaded in the process -- thousands, in Beat Saber -- calling get_isSupported,
  // get_name and FindPropertyIndex on each. RepairMaterialShader calls it for
  // every material it cannot fix, and prefab instances bring fresh materials, so
  // without a negative cache one unfixable bundle turns into a full shader-database
  // scan per material per spawn. That is not a slow frame, it is a stopped game.
  if (_fallbackShaderSearchFailed) return nullptr;
  _fallbackShader = nullptr;
  EnsureGameShaderIndex();

  // Shader.Find only resolves shaders included in the build or already loaded
  // from a bundle, and on Quest almost none of Beat Saber's own shaders answer
  // to it -- which is why a by-name search kept falling through to Unity's
  // built-ins. The index built from Resources.FindObjectsOfTypeAll does answer,
  // so it is asked first.
  auto resolveByName = [this](std::string_view name) -> UnityEngine::Shader* {
    auto const key = NormalizeAssetKey(std::string(name));
    if (auto it = _gameShadersByName.find(key); it != _gameShadersByName.end()) {
      if (IsAlive(it->second) && it->second->get_isSupported()) return it->second;
    }
    auto* found = UnityEngine::Shader::Find(StringW(std::string(name))).unsafePtr();
    if (IsAlive(found) && found->get_isSupported()) return found;
    return nullptr;
  };

  // A stand-in that cannot be tinted is why notes came out white. Note and
  // saber replacements are coloured by writing _Color into a
  // MaterialPropertyBlock, so a shader is only fully acceptable here if the
  // colour can actually land on it.
  auto carriesColour = [](UnityEngine::Shader* shader) {
    return shader->FindPropertyIndex(StringW("_Color")) >= 0 ||
           shader->FindPropertyIndex(StringW("_BaseColor")) >= 0;
  };

  // Names worth trying directly, best first. Every entry here shades opaque 3D
  // geometry.
  //
  // Sprites/Default and UI/Default used to be on this list, and that is what
  // turned converted maps black: a sprite shader carries _Color, so requiring
  // _Color promoted it over Unlit/Texture, and then it was handed 3D meshes.
  // Sprites/Default multiplies by the vertex COLOR stream, blends against the
  // frame, and writes no depth -- a mesh with no vertex-colour channel (which
  // is most map geometry) reads whatever the driver leaves in that register,
  // and on the Quest's GLES driver that is zero. Black geometry, blended over
  // a black frame. Neither shader belongs anywhere near a mesh.
  // An explicit choice wins over every heuristic below.
  //
  // Which shader stands in acceptably depends on what a given Beat Saber build
  // ships and how the map lights its scene, and neither is knowable from
  // anywhere but a headset. Rather than another round of guessing, a name from
  // the shader index this logs at level start can be put in
  // standInShaderName in the mod's config and tried at once.
  std::string const requested = GetStandInShaderName();
  if (!requested.empty()) {
    auto* chosen = resolveByName(requested);
    if (chosen != nullptr) {
      _fallbackShader = chosen;
      PaperLogger.info("Vivify fallback shader: using '{}', named by the standInShaderName setting",
                       ShaderNameForLog(chosen));
      return _fallbackShader;
    }
    PaperLogger.warn("Vivify fallback shader: the standInShaderName setting asks for '{}', which is "
                     "not among the runnable shaders on this device; choosing automatically instead",
                     requested);
  }

  // Unlit first, lit last, and that order is the whole point.
  //
  // A Vivify map replaces the environment, and the environment is where Beat
  // Saber's lights live. A lit shader in a scene with no lights returns black
  // no matter what colour or texture is fed to it -- which is what a converted
  // level looked like for several builds while Custom/SimpleLit, a lit shader,
  // sat at the top of this list. The giveaway was that particles still showed:
  // particle materials are unlit and additive, so they were the only things a
  // missing light source could not switch off.
  //
  // The scored scan below has always ranked "unlit" above "simplelit"; this
  // list was overriding it before the scan ever ran.
  static constexpr std::string_view preferredNames[] = {
      "BeatSaber/Unlit Glow"sv,  "Custom/UnlitGlow"sv,     "Unlit/Texture"sv,
      "Unlit/Color"sv,           "Custom/Glowing"sv,       "Custom/GlowingInstancedHD"sv,
      "Custom/OpaqueNeonLight"sv,
      // Everything past here needs a light to show anything at all, and is only
      // reached when the device has none of the above.
      "Custom/SimpleLit"sv,      "Standard"sv,             "Mobile/Diffuse"sv,
      "Legacy Shaders/Diffuse"sv,
  };
  // A named 3D shader that cannot be tinted still beats a sprite shader, so a
  // colourless one is kept as a runner-up rather than discarded outright.
  UnityEngine::Shader* colourlessRunnerUp = nullptr;
  for (auto name : preferredNames) {
    auto* candidate = resolveByName(name);
    if (candidate == nullptr) continue;
    if (carriesColour(candidate)) {
      _fallbackShader = candidate;
      PaperLogger.info("Vivify fallback shader: using '{}' (named candidate, tintable, "
                       "mainTex={})",
                       ShaderNameForLog(candidate),
                       BoolText(candidate->FindPropertyIndex(StringW("_MainTex")) >= 0));
      return _fallbackShader;
    }
    if (colourlessRunnerUp == nullptr) colourlessRunnerUp = candidate;
  }

  // Nothing by name -- score every shader currently loaded and take the best.
  //
  // The category decides the winner and the property bonuses only break ties
  // within a category. They used to be worth 50 each against category scores
  // 10 apart, so "some sprite shader with a texture and a colour" outranked
  // every real lit shader in the process.
  auto scoreShader = [](std::string const& lowerName) -> int {
    // Shaders that exist but would draw nothing useful for arbitrary geometry.
    static constexpr std::string_view excluded[] = {
        "hidden/"sv,   "internal"sv, "text"sv,   "font"sv,    "skybox"sv,
        "shadow"sv,    "depth"sv,    "blit"sv,   "postpro"sv, "compositor"sv,
        "cursor"sv,    "mask"sv,     "stencil"sv, "occlusion"sv,
    };
    for (auto bad : excluded) {
      if (lowerName.find(bad) != std::string::npos) return -1;
    }
    // Sprite and UI shaders sort *below* an unrecognised shader, not above it:
    // they are 2D shaders and drawing a mesh with one is the failure this
    // ordering exists to avoid. They stay on the list only as a last resort.
    if (lowerName.find("sprite") != std::string::npos) return 5;
    if (lowerName.find("ui/") != std::string::npos) return 4;
    if (lowerName.find("unlit") != std::string::npos) return 100;
    if (lowerName.find("simplelit") != std::string::npos) return 90;
    if (lowerName.find("standard") != std::string::npos) return 80;
    if (lowerName.find("glow") != std::string::npos) return 70;
    if (lowerName.find("lit") != std::string::npos) return 60;
    if (lowerName.find("diffuse") != std::string::npos) return 50;
    if (lowerName.find("particle") != std::string::npos) return 15;
    return 10;
  };

  int bestScore = 0;
  auto allShaders = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Shader*>();
  if (allShaders) {
    for (int i = 0; i < allShaders.size(); i++) {
      auto* candidate = allShaders[i];
      if (!IsAlive(candidate) || !candidate->get_isSupported()) continue;
      auto name = candidate->get_name();
      if (!name) continue;
      std::string lowerName = ToStdString(name);
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      int score = scoreShader(lowerName);
      if (score <= 0) continue;
      // A stand-in is only useful if the original material's look can be
      // carried across. One that exposes neither _MainTex nor a colour renders
      // everything flat white, which is what made converted maps come up
      // partially or fully white. These break ties inside a category; they
      // never promote one category over another.
      score *= 100;
      if (candidate->FindPropertyIndex(StringW("_MainTex")) >= 0) score += 30;
      if (carriesColour(candidate)) score += 40;
      if (score <= bestScore) continue;
      bestScore = score;
      _fallbackShader = candidate;
    }
  }

  if (IsAlive(_fallbackShader)) {
    PaperLogger.info("Vivify fallback shader: scanned loaded shaders, using '{}' (score {})",
                     ShaderNameForLog(_fallbackShader), bestScore);
    return _fallbackShader;
  }

  if (IsAlive(colourlessRunnerUp)) {
    _fallbackShader = colourlessRunnerUp;
    PaperLogger.info("Vivify fallback shader: using '{}' (named candidate; it has no colour "
                     "property, so stand-ins wear their texture untinted)",
                     ShaderNameForLog(_fallbackShader));
    return _fallbackShader;
  }

  _fallbackShaderSearchFailed = true;
  PaperLogger.warn(
      "Vivify found no usable stand-in shader among the shaders loaded in this process. "
      "Materials with unsupported shaders will be left alone rather than rescanning every time.");
  return nullptr;
}

void Runtime::RepairMaterialShader(UnityEngine::Material* material, std::string_view context) {
  if (!IsAlive(material)) return;
  // No GPU instancing for a converted bundle's materials. Beat Saber draws
  // notes that are on screen together -- chords, chain links -- as one
  // instanced batch, and on the Quest the translated instanced variants put
  // every copy but one in the wrong place, pointing the wrong way. Single
  // notes, drawn through the plain variant, were right. With instancing off
  // every object is drawn on its own through that plain variant; notes are
  // few enough that the extra draw calls cost little.
  if (_preloadedBundlePath.rfind(ConvertedBundleCacheDir(), 0) == 0 && material->get_enableInstancing()) {
    material->set_enableInstancing(false);
    _instancingDisabledMaterials++;
  }
  if (_repairedMaterials.contains(material)) return;
  // A shader the level's Quest bundle shipped empty, with a PC build standing
  // in for it (BeginAndroidBundleLoad).
  if (!_graftedShaders.empty()) {
    auto* current = material->get_shader().unsafePtr();
    // By name, not by isSupported: a shader shipped with no programs reports
    // itself supported and draws nothing, so 0.14.2-0.14.6 never swapped it.
    if (IsAlive(current)) {
      auto graft = _graftedShaders.find(ShaderNameForLog(current));
      if (graft != _graftedShaders.end() && IsAlive(graft->second) && graft->second != current) {
        material->set_shader(graft->second);
        _graftApplied++;
        PaperLogger.info("Vivify: material '{}' now uses the PC build of '{}'", ToStdString(material->get_name()),
                         graft->first);
        _repairedMaterials.insert(material);
        return;
      }
    }
  }
  auto shader = material->get_shader();
  auto* rawShader = shader.unsafePtr();
  auto originalShaderName = ShaderNameForLog(rawShader);
  int const originalPassCount = IsAlive(rawShader) ? material->get_passCount() : 0;
  if (GetVivifyDebugLogging() &&
      (!IsAlive(rawShader) || IsInternalErrorShaderName(originalShaderName) ||
       (IsAlive(rawShader) && !rawShader->get_isSupported()))) {
    PaperLogger.warn("Vivify shader diagnostic: context={} material='{}' shader='{}' supported={} internalError={} passes={}",
                     context,
                     ToStdString(material->get_name()),
                     originalShaderName,
                     BoolText(IsAlive(rawShader) && rawShader->get_isSupported()),
                     BoolText(IsInternalErrorShaderName(originalShaderName)),
                     originalPassCount);
  }
  // A shader is only left alone if the GPU says it can actually run it.
  //
  // The "|| originalPassCount > 0" escape hatch that used to be here made every
  // converted PC bundle render nothing: Material.passCount reports the passes
  // declared in the shader's subshaders, which a DirectX-only shader still has
  // on Android even though it carries no GLES program. So every broken shader
  // took this early return, was recorded as repaired, and kept a shader that
  // draws nothing.
  if (IsAlive(rawShader) && rawShader->get_isSupported() &&
      !IsInternalErrorShaderName(originalShaderName)) {
    ApplyStereoKeywords(material);
    _repairedMaterials.emplace(material);
    return;
  }
  _shaderRepairAttempts++;
  auto fallbackState = CaptureMaterialFallbackState(material);
  UnityEngine::Shader* replacement = nullptr;
  if (IsAlive(rawShader)) {
    auto shaderName = rawShader->get_name();
    if (shaderName) {
      replacement = FindUsableShader(ToStdString(shaderName));
    }
  }
  if (!IsAlive(replacement)) {
    replacement = FindFallbackShader();
  }
  // A material with nothing to carry over used to be left with its dead shader,
  // on the reasoning that a flat white mesh is worse than none. In practice it
  // is the other way round, and this is why models go missing on maps whose
  // shaders otherwise work: a material with no colour-shaped property and no
  // texture -- which is most of a raymarch or effect material, whose properties
  // are things like _Speed and _Iterations -- simply never drew. The 0.8.9 log
  // from a real session declined 299 of them in one sitting, and the issue
  // thread describes the result exactly: "blank for some of the intro, then it
  // has the graphics for some of the sections".
  //
  // The stand-in is taken now, tinted a dim neutral grey rather than left at
  // white. That keeps the original worry honest -- a large or full-screen mesh
  // no longer flashes blinding white -- while the geometry is at least present.
  // Anyone who prefers the old behaviour has the "Stand-In Shading" toggle,
  // which is the one case still declined here.
  bool const canCarryLook = fallbackState.color.has_value() || IsManagedAlive(fallbackState.mainTexture);
  bool const usingGenericStandIn = IsAlive(replacement) && replacement == _fallbackShader;
  if (usingGenericStandIn && !GetStandInShading()) {
    _shaderRepairFailed++;
    PaperLogger.warn("Vivify shader stand-in declined: material '{}' (shader '{}') -- "
                     "stand-in shading is turned off in settings",
                     ToStdString(material->get_name()), originalShaderName);
    _repairedMaterials.emplace(material);
    return;
  }
  if (usingGenericStandIn && !canCarryLook && IsScreenSpaceEffectShader(originalShaderName)) {
    // A grey stand-in is right for a mesh and wrong for a screen effect.
    //
    // A blit, a skybox, a stencil mask and a fog volume are all geometry that
    // covers the view: their own shader is what makes them subtle or invisible,
    // and none of that survives the substitution. Painting them opaque grey
    // does not approximate the effect, it hangs a wall in front of the map --
    // which is what a converted level looked like even after two hundred
    // materials were "repaired". These are left undrawn instead, which is what
    // they would have been before the stand-in existed.
    _shaderRepairFailed++;
    _screenEffectsDeclined++;
    PaperLogger.warn("Vivify shader stand-in declined: material '{}' (shader '{}') is a screen or "
                     "masking effect, and a stand-in for one covers the view instead of "
                     "approximating it",
                     ToStdString(material->get_name()), originalShaderName);
    _repairedMaterials.emplace(material);
    return;
  }
  if (usingGenericStandIn && !IsManagedAlive(fallbackState.mainTexture) && fallbackState.color.has_value() &&
      IsUninformativeColor(*fallbackState.color)) {
    // The only colour on offer is the default white every material starts at,
    // and there is no texture to multiply it by. On PC the map's own shader
    // turned that white into the intended look; the stand-in cannot, so the
    // mesh comes out as a flat, fully lit white shape -- and because a converted
    // level is mostly such meshes, the whole map reads as white. A soft grey
    // keeps the geometry legible without glaring over everything else. Note,
    // saber and debris replacements are unaffected: their colour arrives
    // through the MaterialPropertyBlock at spawn, which overrides this.
    fallbackState.color = UnityEngine::Color(0.55f, 0.55f, 0.58f, 1.0f);
    _standInsDimmedFromWhite++;
  }
  if (usingGenericStandIn && !canCarryLook) {
    // Dim, opaque, and deliberately unlike anything a map would author, so it
    // reads as "this is a stand-in" rather than as the intended look.
    fallbackState.color = UnityEngine::Color(0.25f, 0.25f, 0.28f, 1.0f);
    PaperLogger.warn("Vivify shader stand-in: material '{}' (shader '{}') had no colour or texture "
                     "to carry over, so it is drawn in neutral grey rather than not at all",
                     ToStdString(material->get_name()), originalShaderName);
  }

  if (IsAlive(replacement)) {
    material->set_shader(replacement);
    RestoreMaterialFallbackState(material, fallbackState);
    ApplyStereoKeywords(material);
    _shaderRepairSucceeded++;
    if (replacement == _fallbackShader) {
      // Remember that this material is only wearing a stand-in. Substituting a
      // generic shader lets a mesh be seen, but doing the same for a
      // full-screen blit would smear an unrelated shader over the whole frame,
      // so CanUseBlitMaterial refuses these outright and the blit is skipped.
      _fallbackShadedMaterials.emplace(material);
    }
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify shader repaired: context={} material='{}' from='{}' to='{}' preservedColor={} preservedTexture={}",
                       context, ToStdString(material->get_name()), originalShaderName, ShaderNameForLog(replacement),
                       BoolText(fallbackState.color.has_value()), BoolText(IsManagedAlive(fallbackState.mainTexture)));
    }
  } else {
    _shaderRepairFailed++;
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify shader repair failed: context={} material='{}' original='{}'",
                       context, ToStdString(material->get_name()), originalShaderName);
    }
  }
  _repairedMaterials.emplace(material);
}

void Runtime::RepairGameObjectMaterials(UnityEngine::GameObject* gameObject, std::string_view context) {
  if (!IsAlive(gameObject)) return;
  auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
  for (int i = 0; i < renderers.size(); i++) {
    auto* renderer = renderers[i];
    if (!IsAlive(renderer)) continue;
    auto materials = renderer->get_sharedMaterials();
    if (!materials) continue;
    for (int j = 0; j < materials.size(); j++) {
      RepairMaterialShader(materials[j].unsafePtr(), context);
    }
  }
}

void Runtime::SetMaterialKeyword(UnityEngine::Material* material, ::StringW keyword, bool enabled) const {
  if (!IsAlive(material)) return;
  if (enabled) {
    material->EnableKeyword(keyword);
  } else {
    material->DisableKeyword(keyword);
  }
}

void Runtime::ApplyStereoKeywords(UnityEngine::Material* material) const {
  if (!IsAlive(material)) return;

  SetMaterialKeyword(material, u"MULTIPASS_ENABLED", GetMultipassRenderingEnabled());
}

void Runtime::ApplyGameObjectStereoKeywords(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return;
  auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
  for (int i = 0; i < renderers.size(); i++) {
    auto* renderer = renderers[i];
    if (!IsAlive(renderer)) continue;
    auto materials = renderer->get_sharedMaterials();
    if (!materials) continue;
    for (int j = 0; j < materials.size(); j++) {
      ApplyStereoKeywords(materials[j].unsafePtr());
    }
  }
}

void Runtime::RefreshLoadedMaterialStereoKeywords() {
  for (auto const& [_, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr); IsAlive(material)) {
      ApplyStereoKeywords(material);
    } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr); IsAlive(gameObject)) {
      ApplyGameObjectStereoKeywords(gameObject);
    }
  }
}


// Decodes a block-compressed texture this GPU cannot sample into RGBA32.
//
// Quest's Adreno GPUs support ETC2 and ASTC but not S3TC/BC, and a PC-built
// AssetBundle stores its textures as BC1/BC3/BC7. Unity will happily hand back
// the Texture2D object, but nothing can sample it -- which is why converted
// maps came through untextured even once their materials carried the right
// colour. Decoding on the CPU costs memory (BC1 is 4 bits per pixel, RGBA32 is
// 32) but produces something that actually renders.
//
// EVERY refusal path here returns the texture unchanged, and that is the whole
// design. This pass is where converted levels turned black.
//
// Decoding needs the source texture's own bytes, and a texture loaded from an
// AssetBundle only still has them if the map author ticked Read/Write Enabled,
// which almost nobody does: the pixels are uploaded to the GPU and the CPU copy
// is dropped. Ask such a texture for its data anyway and Unity does not
// necessarily fail -- it can hand back an array of exactly the right length
// with nothing in it. That decodes perfectly: an all-zero BC1 block is a valid
// block, and it means opaque black. So every material in the map was given a
// black texture, in place of one that was merely unsampleable, and the level
// went from washed-out to pitch black with only the untextured particles left
// showing. Textures were the one thing that changed between the last build that
// rendered (0.7.2) and the first that did not (0.8.0).
//
// Hence three gates before anything is swapped in: the texture must say its
// pixels are readable, the bytes must not be uniformly zero, and the decoded
// result must have something visible in it. A texture that fails any of them is
// left exactly as it was -- unsampleable, so it draws as flat white, which is
// how 0.7 behaved and is what the map looked like before this pass existed.
UnityEngine::Texture* Runtime::ResolveUsableTexture(UnityEngine::Texture* texture) {
  if (!IsAlive(texture)) return texture;
  if (auto cached = _decodedTextures.find(texture); cached != _decodedTextures.end()) {
    return IsAlive(cached->second) ? cached->second : texture;
  }

  auto* source = il2cpp_utils::try_cast<UnityEngine::Texture2D>(texture).value_or(nullptr);
  if (!IsAlive(source)) return texture;

  int const unityFormat = source->get_format().value__;
  if (UnityEngine::SystemInfo::SupportsTextureFormat(source->get_format())) {
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  auto const format = TextureDecode::FromUnityTextureFormat(unityFormat);
  std::string const name = ToStdString(source->get_name());
  if (format == TextureDecode::Format::Unsupported) {
    PaperLogger.warn("Vivify texture '{}': format {} is unsupported here and cannot be decoded", name, unityFormat);
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  int const width = source->get_width();
  int const height = source->get_height();
  int const mipCount = std::max(1, source->get_mipmapCount());

  // Gate one. A texture that reports its pixels as unreadable has no CPU copy
  // to decode, and whatever GetRawTextureData answers with is not its contents.
  if (!source->get_isReadable()) {
    _texturesUnreadable++;
    if (_texturesUnreadable <= 8) {
      PaperLogger.warn("Vivify texture '{}' ({}, {}x{}): not readable on the CPU, so there are no bytes to "
                       "decode. Left as it is -- it draws untextured rather than black.",
                       name, TextureDecode::FormatName(format), width, height);
    }
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  ArrayW<uint8_t, Array<uint8_t>*> raw = nullptr;
  try {
    raw = source->GetRawTextureData();
  } catch (...) {
    raw = nullptr;
  }
  if (!raw || raw.size() == 0) {
    PaperLogger.warn("Vivify texture '{}' ({}, {}x{}): no raw data available to decode -- the texture was "
                     "imported without read/write enabled, so its CPU copy is gone",
                     name, TextureDecode::FormatName(format), width, height);
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  // Gate two. Right length, no content: the signature of a CPU copy that was
  // already dropped. Decoding it would succeed and produce solid black.
  if (TextureDecode::IsAllZero(raw.begin(), static_cast<size_t>(raw.size()))) {
    _texturesBlank++;
    if (_texturesBlank <= 8) {
      PaperLogger.warn("Vivify texture '{}' ({}, {}x{}): {} byte(s) of raw data, all zero -- the pixels are "
                       "gone even though the texture says it is readable. Left as it is.",
                       name, TextureDecode::FormatName(format), width, height, raw.size());
    }
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  std::vector<uint8_t> decoded;
  if (!TextureDecode::DecodeToRgba32(format, raw.begin(), static_cast<size_t>(raw.size()), width, height, mipCount,
                                     decoded)) {
    PaperLogger.warn("Vivify texture '{}' ({}, {}x{}, {} mip(s)): {} byte(s) of data did not decode",
                     name, TextureDecode::FormatName(format), width, height, mipCount, raw.size());
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  // Gate three. The decode worked on data that was not all zero and still came
  // out with no colour, or nothing but transparency. Whatever that is, drawing
  // it is not an improvement on drawing the original.
  if (TextureDecode::IsBlankRgba32(decoded.data(), decoded.size())) {
    _texturesBlank++;
    if (_texturesBlank <= 8) {
      PaperLogger.warn("Vivify texture '{}' ({}, {}x{}): decoded to nothing visible, so the original is kept",
                       name, TextureDecode::FormatName(format), width, height);
    }
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  auto* replacement = UnityEngine::Texture2D::New_ctor(width, height, UnityEngine::TextureFormat::RGBA32,
                                                       mipCount, false);
  if (!IsAlive(replacement)) {
    _decodedTextures[texture] = nullptr;
    return texture;
  }
  auto managed = ArrayW<uint8_t, Array<uint8_t>*>(static_cast<il2cpp_array_size_t>(decoded.size()));
  std::memcpy(managed.begin(), decoded.data(), decoded.size());
  replacement->LoadRawTextureData(managed);
  replacement->Apply();
  replacement->set_wrapMode(source->get_wrapMode());
  replacement->set_filterMode(source->get_filterMode());
  replacement->set_name(StringW(name + " (decoded)"));

  PaperLogger.info("Vivify texture decoded: '{}' {} {}x{} ({} mip(s)) -> RGBA32", name,
                   TextureDecode::FormatName(format), width, height, mipCount);
  _decodedTextures[texture] = replacement;
  return replacement;
}

// Walks every material in the bundle and swaps any texture this GPU cannot
// sample for a decoded copy. Runs once per bundle load, before shader repair,
// so a material that keeps its own working shader still gets usable textures.
void Runtime::DecodeUnsupportedBundleTextures() {
  int swapped = 0;
  int skipped = 0;
  _texturesUnreadable = 0;
  _texturesBlank = 0;
  _texturesScannedMaterials.clear();

  // Block-compressed decoding is real CPU work on the main thread: a 2048x2048
  // BC7 texture is four million pixels, and a bundle can hold dozens. Left
  // unbounded it stalls the game for as long as it takes, which is
  // indistinguishable from a freeze. Decode what fits in the budget, skip the
  // rest, and say how many were skipped -- a few untextured materials beat a
  // hung game.
  // Raised from two seconds along with the prefab walk above. That walk finds
  // the materials that carry most of a map's textures, so the old budget --
  // sized for the handful of standalone Material assets -- would now be spent
  // long before the scene geometry was reached, and a skipped texture is a
  // black one. A converted map already pays seconds for the conversion itself;
  // several more on first load beat a level that cannot be seen.
  constexpr double kDecodeBudgetMs = 8000.0;
  auto const start = std::chrono::steady_clock::now();
  auto elapsedMs = [&start]() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  };

  auto decodeMaterial = [&](UnityEngine::Material* material) {
    if (!IsAlive(material)) return;
    // A material reached through several renderers is the same material; doing
    // it twice would only spend budget.
    if (!_texturesScannedMaterials.emplace(material).second) return;
    auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Texture);
    if (!names) return;
    for (auto name : names) {
      if (!name) continue;
      auto* current = material->GetTexture(name).unsafePtr();
      if (!IsAlive(current)) continue;
      // An already-decoded texture is a cache hit and costs nothing, so the
      // budget only gates work that has not been done yet.
      if (elapsedMs() > kDecodeBudgetMs && !_decodedTextures.contains(current)) {
        skipped++;
        continue;
      }
      auto* usable = ResolveUsableTexture(current);
      if (IsAlive(usable) && usable != current) {
        material->SetTexture(name, usable);
        swapped++;
      }
    }
  };

  // The same two kinds of asset the shader repair walks, and for the same
  // reason.
  //
  // This used to consider only Material assets. Nearly every material in a
  // Vivify map is not one: it hangs off a renderer inside a prefab
  // (assets/.../prefabs/scene1.prefab and friends), which is exactly why
  // RepairLoadedMaterialShaders walks GameObjects as well. So scene geometry
  // had its shader repaired and its textures left as DirectX block-compressed
  // data that an Adreno cannot sample -- which reads as black. A material with
  // no texture at all was unaffected and drew as a flat pale shape, so the
  // symptom was a black level with a few white objects and the particles still
  // showing.
  for (auto const& [path, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
        IsAlive(material)) {
      decodeMaterial(material);
      continue;
    }
    auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr);
    if (!IsAlive(gameObject)) continue;
    auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
    for (int i = 0; i < renderers.size(); i++) {
      auto* renderer = renderers[i];
      if (!IsAlive(renderer)) continue;
      auto materials = renderer->get_sharedMaterials();
      if (!materials) continue;
      for (int j = 0; j < materials.size(); j++) {
        decodeMaterial(materials[j].unsafePtr());
      }
    }
  }
  _texturesDecoded = swapped;
  _texturesSkipped = skipped;
  // Logged even when nothing was found, and that is the point: this pass
  // printing nothing at all is how it went unnoticed that it was looking in the
  // wrong place. "scanned 0" is a symptom; silence is not.
  PaperLogger.info(
      "Vivify texture decode: scanned {} material(s), replaced {} unsupported texture "
      "reference(s) in {:.0f}ms, skipped {} over budget, refused {} unreadable and {} blank",
      _texturesScannedMaterials.size(), swapped, elapsedMs(), skipped, _texturesUnreadable,
      _texturesBlank);
  if (_texturesUnreadable > 0 || _texturesBlank > 0) {
    PaperLogger.warn(
        "Vivify left {} texture(s) undecoded because their pixels are not available on the CPU. Those "
        "materials draw untextured, which is the look this port had before decoding existed -- decoding "
        "them anyway is what made converted levels black.",
        _texturesUnreadable + _texturesBlank);
  }
  if (skipped > 0) {
    PaperLogger.warn(
        "Vivify stopped decoding textures after {:.0f}ms and left {} reference(s) on their original, "
        "unsampleable format. Those materials render untextured rather than stalling the game.",
        kDecodeBudgetMs, skipped);
  }
}

void Runtime::RepairLoadedMaterialShaders() {
  _shaderRepairAttempts = 0;
  _shaderRepairSucceeded = 0;
  _shaderRepairFailed = 0;
  _screenEffectsDeclined = 0;
  _standInsDimmedFromWhite = 0;
  _instancingDisabledMaterials = 0;
  for (auto const& [path, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr); IsAlive(material)) {
      RepairMaterialShader(material, path);
    } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr); IsAlive(gameObject)) {
      RepairGameObjectMaterials(gameObject, path);
    }
  }
  if (_instancingDisabledMaterials > 0) {
    PaperLogger.info("Vivify: GPU instancing turned off on {} material(s) of this converted bundle, so "
                     "notes drawn together (chords, chains) each get their own transform",
                     _instancingDisabledMaterials);
  }
  if (_shaderRepairAttempts > 0) {
    // Worth logging unconditionally: a bundle whose shaders all had to be
    // replaced is a converted PC bundle rendering with stand-in shading, and a
    // non-zero failure count means some of it will not draw at all.
    PaperLogger.info("Vivify shader repair: {} screen/masking effect(s) left undrawn on purpose, {} white "
                     "untextured stand-in(s) dimmed to grey",
                     _screenEffectsDeclined, _standInsDimmedFromWhite);
  PaperLogger.info("Vivify shader repair: {} material(s) had an unusable shader, {} replaced, {} could not be",
                     _shaderRepairAttempts, _shaderRepairSucceeded, _shaderRepairFailed);
  }
}


// ---------------------------------------------------------------------------
// Bulk conversion
//
// A map that ships only a PC bundle has its play button disabled, so there is
// no way to reach it through normal level selection -- which also means no way
// to trigger a per-level conversion. This pass walks every installed custom
// level directly and converts anything convertible, so those maps become
// playable without having to be playable first.
// ---------------------------------------------------------------------------

namespace {
std::atomic<bool> gBulkConversionRunning{false};
std::atomic<bool> gRedownloadRunning{false};

std::vector<std::filesystem::path> CollectCustomLevelDirectories() {
  std::vector<std::filesystem::path> roots;
  for (auto const& root : SongCore::API::Loading::GetRootCustomLevelPaths()) roots.push_back(root);
  for (auto const& root : SongCore::API::Loading::GetRootCustomWIPLevelPaths()) roots.push_back(root);

  std::vector<std::filesystem::path> levels;
  std::error_code ec;
  for (auto const& root : roots) {
    if (!std::filesystem::is_directory(root, ec) || ec) {
      ec.clear();
      continue;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) break;
      if (entry.is_directory(ec) && !ec) levels.push_back(entry.path());
      ec.clear();
    }
    ec.clear();
  }
  return levels;
}
}

// Whether a song is playing, for the background pass (0.14.12): a pass that
// reconverts dozens of bundles took CPU, memory and storage from the song and
// made every map stutter. It now waits between bundles while one plays.
std::atomic<bool> gSongPlaying{false};

void SetSongPlaying(bool playing) {
  gSongPlaying.store(playing);
}

void Runtime::ReleaseBundlesForReset() {
  if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    _mainBundle->Unload(true);
  }
  _mainBundle = nullptr;
  _preloadedBundlePath.clear();
  _selectedBundlePath.clear();
  _selectedLevelPath.clear();
  _graftedShaders.clear();
  _graftDonorPath.clear();
  _graftLevelPath.clear();
  _warmupQueue.clear();
  _warmupIndex = 0;
  ClearBlitCompanion();
  DisarmLoadGuard();
  SongCore::API::PlayButton::EnablePlayButton("Vivify");
}

std::atomic<bool> gCleanSlateRunning{false};

void ResetToCleanSlate(std::function<void(std::string const&)> onDone) {
  if (gBulkConversionRunning.load() || gRedownloadRunning.load()) {
    if (onDone) onDone("A conversion or redownload is running; reset after it finishes");
    return;
  }
  if (gCleanSlateRunning.exchange(true)) return;
  Runtime::Instance().ReleaseBundlesForReset();
  auto levels = CollectCustomLevelDirectories();
  std::thread([onDone = std::move(onDone), levels = std::move(levels)]() {
    std::error_code ec;
    std::uintmax_t const converted = std::filesystem::remove_all(ConvertedBundleCacheDir(), ec);
    std::string const convertedError = ec ? ec.message() : std::string();
    int pcRemoved = 0;
    for (auto const& level : levels) {
      // Only Quest maps: there the PC bundle is one Vivify downloaded for
      // their empty shaders. A PC-only map's PC bundle is the map itself.
      if (!FileExists(JoinPath(level.string(), std::string(kBundleFile)))) continue;
      for (char const* name : {"bundleWindows2021.vivify", "bundleWindows2019.vivify"}) {
        std::error_code removeError;
        if (std::filesystem::remove(level / name, removeError)) pcRemoved++;
      }
    }
    std::string summary = "Reset done: " + std::to_string(converted) + " converted file(s) and " +
                          std::to_string(pcRemoved) + " downloaded PC bundle(s) deleted, settings reset";
    if (!convertedError.empty()) summary += " (converted folder: " + convertedError + ")";
    PaperLogger.info("Vivify clean slate: {}", summary);
    BSML::MainThreadScheduler::Schedule([onDone, summary]() {
      gCleanSlateRunning.store(false);
      if (onDone) onDone(summary);
    });
  }).detach();
}

// ---------------------------------------------------------------------------
// Redownload every Vivify map from BeatSaver
// ---------------------------------------------------------------------------

namespace {
struct VivifyMapEntry {
  std::filesystem::path folder;
  std::string key;   // BeatSaver key, from a "<key> (<song> - <mapper>)" folder name
  std::string hash;  // SongCore's hash of the installed version
  std::string songName;
  std::string result;
};

// A map is a Vivify map when it carries any Vivify bundle, or its info file
// names Vivify (a requirement or suggestion) or declares asset bundles.
bool IsVivifyLevelFolder(std::filesystem::path const& folder) {
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec) break;
    if (entry.path().extension() == ".vivify") return true;
  }
  for (char const* name : {"Info.dat", "info.dat"}) {
    std::ifstream is(folder / name, std::ios::binary);
    if (!is.is_open()) continue;
    std::string const text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    return text.find("\"Vivify\"") != std::string::npos || text.find("assetBundle\"") != std::string::npos;
  }
  return false;
}

std::string KeyFromFolderName(std::string const& name) {
  size_t length = 0;
  while (length < name.size() && std::isxdigit(static_cast<unsigned char>(name[length]))) length++;
  if (length == 0 || length > 8) return {};
  size_t rest = length;
  while (rest < name.size() && name[rest] == ' ') rest++;
  if (rest < name.size() && name[rest] != '(') return {};
  std::string key = name.substr(0, length);
  for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return key;
}

std::string HashFromLevelId(std::string const& levelId) {
  std::string_view constexpr prefix = "custom_level_";
  if (levelId.rfind(prefix, 0) != 0) return {};
  std::string hash = levelId.substr(prefix.size(), 40);
  if (hash.size() != 40) return {};
  for (auto& c : hash) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return hash;
}

void WriteVivifyMapsList(std::vector<VivifyMapEntry> const& maps) {
  std::ofstream os(VivifyMapsListPath(), std::ios::trunc);
  os << "# Vivify maps found by \"Redownload All Vivify Maps\"\n"
        "# key\thash\tfolder\tsong\tresult\n";
  for (auto const& map : maps) {
    os << (map.key.empty() ? "-" : map.key) << '\t' << (map.hash.empty() ? "-" : map.hash) << '\t'
       << map.folder.string() << '\t' << map.songName << '\t' << (map.result.empty() ? "pending" : map.result)
       << '\n';
  }
}

// The download URL for a map: the exact installed version when BeatSaver
// still has it (looked up by hash), else the map's current version by key.
std::string ResolveBeatSaverDownload(VivifyMapEntry& map, std::string& error) {
  auto fromDocument = [&map](rapidjson::Document const& doc) -> std::string {
    if (!doc.IsObject()) return {};
    if (map.key.empty() && doc.HasMember("id") && doc["id"].IsString()) map.key = doc["id"].GetString();
    if (!doc.HasMember("versions") || !doc["versions"].IsArray()) return {};
    std::string latest;
    for (auto const& version : doc["versions"].GetArray()) {
      if (!version.IsObject() || !version.HasMember("downloadURL") || !version["downloadURL"].IsString()) continue;
      std::string const url = version["downloadURL"].GetString();
      if (latest.empty()) latest = url;
      if (!map.hash.empty() && version.HasMember("hash") && version["hash"].IsString()) {
        std::string hash = version["hash"].GetString();
        for (auto& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (hash == map.hash) return url;
      }
    }
    return latest;
  };
  auto lookup = [&](std::string const& url) -> std::string {
    auto response = WebUtils::Get<WebUtils::StringResponse>(WebUtils::URLOptions(url));
    if (!response.IsSuccessful() || !response.responseData.has_value()) {
      error = "BeatSaver lookup failed (http " + std::to_string(response.get_HttpCode()) + ")";
      return {};
    }
    rapidjson::Document doc;
    doc.Parse(response.responseData->c_str());
    if (doc.HasParseError()) {
      error = "BeatSaver sent an unreadable answer";
      return {};
    }
    std::string const download = fromDocument(doc);
    if (download.empty()) error = "BeatSaver lists no download";
    return download;
  };
  std::string download;
  if (!map.hash.empty()) download = lookup("https://api.beatsaver.com/maps/hash/" + map.hash);
  if (download.empty() && !map.key.empty()) download = lookup("https://api.beatsaver.com/maps/id/" + map.key);
  if (download.empty() && map.key.empty() && map.hash.empty()) error = "no BeatSaver key or hash";
  return download;
}

bool RedownloadOne(VivifyMapEntry& map, std::string& error) {
  std::string const url = ResolveBeatSaverDownload(map, error);
  if (url.empty()) return false;
  PaperLogger.info("Vivify redownload: '{}' from '{}'", map.folder.string(), url);
  // Vivify maps run to hundreds of MB: well past WebUtils' 10 s default.
  auto response = WebUtils::Get<WebUtils::DataResponse>(WebUtils::URLOptions(url, std::nullopt, 900));
  if (!response.IsSuccessful() || !response.responseData.has_value()) {
    error = "download failed (http " + std::to_string(response.get_HttpCode()) + ", curl " +
            std::to_string(response.get_CurlStatus()) + ")";
    return false;
  }

  std::error_code ec;
  std::filesystem::path const staging = map.folder.parent_path() / (map.folder.filename().string() + ".vivify-new");
  std::filesystem::remove_all(staging, ec);
  if (!ExtractZip(*response.responseData, staging, error)) {
    std::filesystem::remove_all(staging, ec);
    error = "unzip failed: " + error;
    return false;
  }
  // BeatSaver zips are flat; one packed with its folder is taken from inside.
  std::filesystem::path root = staging;
  if (!FileExists((staging / "Info.dat").string()) && !FileExists((staging / "info.dat").string())) {
    std::vector<std::filesystem::path> children;
    for (auto const& entry : std::filesystem::directory_iterator(staging, ec)) children.push_back(entry.path());
    if (children.size() == 1 && std::filesystem::is_directory(children[0], ec)) root = children[0];
  }
  if (!FileExists((root / "Info.dat").string()) && !FileExists((root / "info.dat").string())) {
    std::filesystem::remove_all(staging, ec);
    error = "download has no Info.dat";
    return false;
  }

  std::filesystem::remove_all(map.folder, ec);
  if (ec) {
    std::filesystem::remove_all(staging, ec);
    error = "could not delete the old folder: " + ec.message();
    return false;
  }
  std::filesystem::rename(root, map.folder, ec);
  if (ec) {
    // Rename fails across filesystems; copy instead.
    std::error_code copyError;
    std::filesystem::copy(root, map.folder, std::filesystem::copy_options::recursive, copyError);
    if (copyError) {
      error = "could not move the new copy into place: " + copyError.message() + " (it is in '" +
              root.string() + "')";
      return false;
    }
  }
  std::filesystem::remove_all(staging, ec);
  return true;
}
}

std::string VivifyMapsListPath() {
  return "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyMaps.txt";
}

void RedownloadVivifyMaps(std::function<void(std::string const&)> onStatus) {
  auto status = [onStatus](std::string const& text) {
    if (!onStatus) return;
    BSML::MainThreadScheduler::Schedule([onStatus, text]() { onStatus(text); });
  };
  if (gBulkConversionRunning.load() || gCleanSlateRunning.load()) {
    if (onStatus) onStatus("A conversion or reset is running; try again after it finishes");
    return;
  }
  if (gRedownloadRunning.exchange(true)) {
    if (onStatus) onStatus("Already redownloading");
    return;
  }

  // Everything that touches SongCore or Unity happens here, on the main thread.
  Runtime::Instance().ReleaseBundlesForReset();
  std::vector<VivifyMapEntry> maps;
  for (auto const& folder : CollectCustomLevelDirectories()) {
    if (folder.filename().string().ends_with(".vivify-new")) continue;
    if (!IsVivifyLevelFolder(folder)) continue;
    VivifyMapEntry map;
    map.folder = folder;
    map.key = KeyFromFolderName(folder.filename().string());
    map.songName = folder.filename().string();
    if (auto* level = SongCore::API::Loading::GetLevelByPath(folder)) {
      map.hash = HashFromLevelId(static_cast<std::string>(level->levelID));
      std::string const song = static_cast<std::string>(level->songName);
      if (!song.empty()) map.songName = song;
    }
    for (auto& c : map.songName) {
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    maps.push_back(std::move(map));
  }
  if (maps.empty()) {
    gRedownloadRunning.store(false);
    if (onStatus) onStatus("No Vivify maps found");
    return;
  }

  std::thread([maps = std::move(maps), status]() mutable {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(VivifyMapsListPath()).parent_path(), ec);
    WriteVivifyMapsList(maps);
    PaperLogger.info("Vivify redownload: {} Vivify map(s) listed in '{}'", maps.size(), VivifyMapsListPath());
    // Converted copies of the old downloads are dead weight once the maps are
    // replaced; every map converts fresh from what BeatSaver serves.
    std::filesystem::remove_all(ConvertedBundleCacheDir(), ec);

    int done = 0;
    int failed = 0;
    for (size_t i = 0; i < maps.size(); i++) {
      auto& map = maps[i];
      status("Redownloading " + std::to_string(i + 1) + "/" + std::to_string(maps.size()) + ": " +
             map.songName);
      std::string error;
      bool ok = false;
      try {
        ok = RedownloadOne(map, error);
      } catch (std::exception const& e) {
        error = e.what();
      }
      if (ok) {
        done++;
        map.result = "redownloaded";
      } else {
        failed++;
        map.result = "failed: " + error + " (old copy kept)";
        PaperLogger.warn("Vivify redownload: '{}' failed: {}", map.folder.string(), error);
      }
      WriteVivifyMapsList(maps);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::string summary = "Redownloaded " + std::to_string(done) + " of " + std::to_string(maps.size()) +
                          " Vivify map(s)";
    if (failed > 0) summary += "; " + std::to_string(failed) + " failed (kept as they were, see VivifyMaps.txt)";
    PaperLogger.info("Vivify redownload: {}", summary);
    BSML::MainThreadScheduler::Schedule([status, summary]() {
      gRedownloadRunning.store(false);
      SongCore::API::Loading::RefreshSongs(false);
      status(summary + ". Songs refreshing.");
    });
  }).detach();
}

bool IsBulkPcBundleConversionRunning() {
  return gBulkConversionRunning.load();
}

void StartBulkPcBundleConversion(std::function<void(BulkConversionProgress const&)> onProgress, bool force) {
  if (gRedownloadRunning.load()) {
    if (onProgress) {
      BulkConversionProgress progress;
      progress.finished = true;
      progress.status = "Maps are redownloading; convert after that finishes";
      onProgress(progress);
    }
    return;
  }
  bool expected = false;
  if (!gBulkConversionRunning.compare_exchange_strong(expected, true)) {
    return;
  }

  DetectMultiviewKeyword();
  // SongCore's level roots are enumerated here, on the caller's (main) thread,
  // rather than inside the worker: a song refresh can rewrite them, and the
  // worker only needs the snapshot.
  auto levels = CollectCustomLevelDirectories();

  std::thread([onProgress = std::move(onProgress), levels = std::move(levels), force]() {
    // Background work: below the game's own threads.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
    auto report = [&onProgress](BulkConversionProgress progress) {
      if (!onProgress) return;
      BSML::MainThreadScheduler::Schedule([onProgress, progress]() { onProgress(progress); });
    };

    BulkConversionProgress progress;
    try {
      progress.levelsTotal = static_cast<int>(levels.size());
      progress.status = std::string(force ? "Reconverting " : "Scanning ") +
                        std::to_string(progress.levelsTotal) + " level(s)...";
      report(progress);

      for (auto const& level : levels) {
        progress.levelsScanned++;
        std::string const levelPath = level.string();

        // Maps that already have an Android bundle need nothing.
        if (std::filesystem::exists(JoinPath(levelPath, std::string(kBundleFile)))) continue;

        std::string const source = ResolvePcBundlePath(levelPath);
        if (source.empty()) continue;

        std::string const dest = ConvertedBundlePath(source);
        // An explicit reconvert gives translation another go.
        if (force) {
          std::error_code ec;
          std::filesystem::remove(CrashedMarkerPath(dest), ec);
        }
        RecordInterruptedLoad(dest);
        if (std::filesystem::exists(dest)) {
          if (!force && CachedConversionIsCurrent(dest)) {
            progress.alreadyDone++;
            PaperLogger.info("Vivify bulk convert: '{}' already cached at '{}'", source, dest);
            continue;
          }
          if (!force) {
            PaperLogger.info("Vivify bulk convert: cached '{}' predates this converter, redoing it",
                             dest);
          }
          // Forced, or cached by an older converter: drop the file so the
          // conversion actually re-runs.
          // ConvertToAndroid writes through a .part file and renames, so a
          // failure after this point leaves no cached bundle rather than a
          // truncated one -- the level falls back to being unconverted, which
          // is the state it would have been in anyway.
          std::error_code ec;
          std::filesystem::remove(dest, ec);
          if (ec) {
            progress.failed++;
            PaperLogger.warn("Vivify bulk convert: could not remove cached '{}' to reconvert: {}",
                             dest, ec.message());
            continue;
          }
          PaperLogger.info("Vivify bulk convert: discarded cached '{}' to reconvert", dest);
        }

        if (gSongPlaying.load()) {
          progress.status = "Paused while a song plays...";
          report(progress);
          while (gSongPlaying.load()) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        progress.status = level.filename().string();
        report(progress);

        auto const result = RunBundleConversion(source, dest);
        if (result.status == BundleConvert::Status::Success) {
          progress.converted++;
          PaperLogger.info("Vivify bulk convert: '{}' -> '{}' ({})", source, dest, result.message);
        } else if (result.status == BundleConvert::Status::AlreadyAndroid) {
          progress.alreadyDone++;
        } else {
          progress.failed++;
          PaperLogger.warn("Vivify bulk convert failed for '{}' ({}): {}", source,
                           std::string(BundleConvert::StatusText(result.status)), result.message);
        }
        report(progress);
      }

      progress.status = "Converted " + std::to_string(progress.converted) + ", already done " +
                        std::to_string(progress.alreadyDone) + ", failed " + std::to_string(progress.failed);
      PaperLogger.info("Vivify bulk convert finished: scanned={} converted={} alreadyDone={} failed={}",
                       progress.levelsScanned, progress.converted, progress.alreadyDone, progress.failed);
    } catch (std::exception const& ex) {
      progress.status = std::string("Conversion pass failed: ") + ex.what();
      PaperLogger.error("Vivify bulk convert threw: {}", ex.what());
    } catch (...) {
      progress.status = "Conversion pass failed";
      PaperLogger.error("Vivify bulk convert threw a non-std exception");
    }

    progress.finished = true;
    report(progress);
    gBulkConversionRunning.store(false);
  }).detach();
}

}
