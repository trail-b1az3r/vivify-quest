#include "VivifyUpdater.hpp"
#include "main.hpp"
#include "VivifyGlobalLog.hpp"
#include <algorithm>
#include <ctime>
#include <cerrno>
#include <cstring>
#include "VivifyRuntime.hpp"
#include "VivifyReport.hpp"
#include "VivifyRecovery.hpp"
#include <string>
#include <string_view>
#include <fstream>
#include <chrono>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "HMUI/ViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "bsml/shared/BSML-Lite/Creation/Buttons.hpp"
#include "bsml/shared/BSML-Lite/Creation/Layout.hpp"
#include "bsml/shared/BSML-Lite/Creation/Misc.hpp"
#include "bsml/shared/BSML-Lite/Creation/Settings.hpp"
#include "bsml/shared/BSML/Components/Settings/DropdownListSetting.hpp"
#include "bsml/shared/BSML-Lite/Creation/Text.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "TMPro/TextOverflowModes.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/RectOffset.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "bsml/shared/BSML/Settings/BSMLSettings.hpp"
#include "custom-types/shared/register.hpp"
#include "scotland2/shared/modloader.h"
#include <dlfcn.h>
#include <vector>

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};
// A rebuild of the same version, shown beside it in the menu and the logs.
constexpr char const* kBuildLabel = "";

// Defined below, outside this namespace; the settings reset button re-applies it.
void EnsureConfigDefaults();

namespace {
constexpr std::string_view kMultipassRenderingConfigKey = "multipassRendering";
constexpr std::string_view kVivifyDebugLoggingConfigKey = "vivifyDebugLogging";
constexpr std::string_view kDisableBeat0FilmgrainBlitConfigKey = "disableBeat0FilmgrainBlit";
constexpr std::string_view kDisableAllBlitsConfigKey = "disableAllBlits";
constexpr std::string_view kDisableCreateCameraDepthConfigKey = "disableCreateCameraDepth";
// Ported from the rbatteries1-design/Lars27110 base.
constexpr std::string_view kDisableCustomNoteVisualsConfigKey = "disableCustomNoteVisuals";
constexpr std::string_view kDisableVisualsInMultiplayerConfigKey = "disableVisualsInMultiplayer";
constexpr std::string_view kDisableVRCenterAdjustConfigKey = "disableVRCenterAdjust";
constexpr std::string_view kConvertPcBundlesOnDeviceConfigKey = "convertPcBundlesOnDevice";
constexpr std::string_view kStandInShadingConfigKey = "standInShading";
constexpr std::string_view kSceneDepthConfigKey = "sceneDepthTexture";
constexpr std::string_view kSubmitScoresConfigKey = "submitScoresOnVivifyMaps";
constexpr std::string_view kTranslateShadersConfigKey = "translateShadersOnConversion";
constexpr std::string_view kAudioLinkConfigKey = "audioLink";
constexpr std::string_view kMapRealtimeShadowsConfigKey = "mapRealtimeShadows";
// On by default again since 0.15.3, reduced (see VivifyObjectPrefabs.cpp's
// ReduceReflectionProbes) unless "Full Reflection Probes" is on. A new key, so
// the saved "off" of 0.15.1-0.15.2 does not stick: Get Your Wish's water
// reflects the map through its probe, and with probes off it reflected only
// the notes (#102).
constexpr std::string_view kMapReflectionProbesConfigKey = "mapReflectionProbesV2";
constexpr std::string_view kFullReflectionProbesConfigKey = "fullReflectionProbes";
constexpr std::string_view kSingleViewMapCamerasConfigKey = "singleViewMapCameras";
constexpr std::string_view kAutoEffectResolutionConfigKey = "autoEffectResolution";
constexpr std::string_view kCrashRecoveryConfigKey = "crashRecovery";
constexpr std::string_view kReplayRenderModeConfigKey = "replayRenderMode";
constexpr std::string_view kBlitFixConfigKey = "blitScreenTextureFix";
// Map sabers (AssignObjectPrefab saber) replace the player's own, custom saber
// mods included. Off keeps the player's sabers on every map (0.15.4).
constexpr std::string_view kMapSabersConfigKey = "mapSabers";
// Field Of View % during Vivify maps: 100 is the headset's own (0.15.12).
constexpr std::string_view kFieldOfViewConfigKey = "fieldOfViewPercent";
// The headset's fixed foveated rendering level during Vivify maps: 0 leaves
// the game's own setting alone, 1-4 are the system's low to high-top (0.15.10).
constexpr std::string_view kFoveationConfigKey = "vivifyFoveationLevel";
// Lets lights another mod puts on the sabers light a Vivify map's objects.
// Off by default: they are switched off while a Vivify map plays (0.15.9).
constexpr std::string_view kSaberLightsConfigKey = "saberLightsOnMapObjects";
// Switches off a screen effect that turns a normal picture into one flat
// colour for three seconds, and logs what went in and out (0.15.7).
constexpr std::string_view kFlatScreenGuardConfigKey = "flatScreenGuard";
// Renamed in 0.14.18 so every install starts with it off: 0.14.15 saved it
// as on, and it may be what froze converted maps in the menu.
constexpr std::string_view kPrepareShadersConfigKey = "prepareShadersInMenu";
constexpr std::string_view kGeometryEffectsConfigKey = "drawGeometryShaderEffects";
// Geometry-shader effects drawn without their geometry stage, so they show in
// both eyes (flat) and not in the left eye only (0.15.9).
// Renamed in 0.15.11 so it starts off for everyone: with the stage dropped,
// effects whose shapes the geometry stage makes (grass blades from points)
// drew nothing. Geometry stages now route their eyes themselves.
constexpr std::string_view kGeometryBothEyesConfigKey = "geometryEffectsFlatV2";
// Geometry-shader effects drawn a second time for the right eye (0.15.12).
constexpr std::string_view kGeometryEyeCopyConfigKey = "geometryRightEyeCopy";
constexpr std::string_view kPcShadersForEmptyConfigKey = "pcShadersForEmptyQuestShaders";
constexpr std::string_view kStandInShaderNameConfigKey = "standInShaderName";
constexpr std::string_view kEffectResolutionConfigKey = "effectResolutionPercent";
// Offscreen Culling is off by default since 0.15.3, under a new key so that
// everyone's saved "on" from earlier builds is reset once: an animator frozen
// while its objects are out of view never brings into view an object it
// animates from scale 0 or from a disabled renderer. Get Your Wish's lyrics
// and other elements only appeared with culling off (#102).
constexpr std::string_view kOffscreenCullingConfigKey = "offscreenCullingV2";
constexpr std::string_view kCrashGuardConfigKey = "crashGuard";
constexpr std::string_view kPcBundleForMissingShadersConfigKey = "pcBundleForMissingShaders";
constexpr std::string_view kFarCullingConfigKey = "farCullingDistance";
bool gMultipassRenderingEnabled = true;
bool gVivifyDebugLogging = false;
bool gDisableBeat0FilmgrainBlit = false;
bool gDisableAllBlits = false;
bool gDisableCreateCameraDepth = false;
bool gDisableCustomNoteVisuals = false;
// Defaults to true, matching the base this was ported from: Vivify's world-space
// note/saber/debris replacements aren't validated for multiplayer lobbies, so they
// stay off there unless the player opts back in.
bool gDisableVisualsInMultiplayer = true;
bool gDisableVRCenterAdjust = false;
// Replaces the old "allowUnsafeWindowsBundleFallback" toggle. That one handed a
// PC-built AssetBundle straight to Unity, which simply reports no assets on
// Android. This one instead retargets the archive to Android on device first
// (see VivifyBundleConvert), so the geometry/prefabs in it actually load.
// Defaults on: it only ever runs when a map has no Android bundle at all and no
// downloadable one, i.e. when the alternative is an unplayable map.
bool gConvertPcBundlesOnDevice = true;
// When a bundle's shader cannot run on this GPU, Vivify swaps in a generic
// stand-in so the mesh is at least visible. That trades "invisible" for
// "visible but wrong", and for a converted PC bundle "wrong" often means flat
// white. Turning this off leaves such meshes undrawn instead, which also means
// notes and sabers keep the game's own visuals rather than a white stand-in.
bool gStandInShading = true;
bool gSceneDepth = true;
// Vivify does not change note timing, scoring, or anything else a leaderboard
// cares about -- it changes how a map looks. Submission was nevertheless being
// turned off for every map carrying the Vivify requirement, and with it off
// BeatLeader and ScoreSaber record no replay, so Vivify maps had no replays to
// watch or render at all.
bool gSubmitScores = true;
// On-device conversion now cross-compiles a PC bundle's DirectX shader programs
// to GLSL ES (VivifyDxbc) rather than only retargeting the archive, so a
// converted map can render its own shading instead of a stand-in. A shader
// using anything outside the translated subset is left exactly as it was, so
// the worst case is the behaviour this replaces. Turning this off falls back to
// the retarget-only conversion, which is the escape hatch if a translated
// bundle turns out worse than an unshaded one -- no new build required, just
// reconvert.
bool gTranslateShaders = true;
bool gAudioLink = true;
bool gMapRealtimeShadows = false;
bool gMapReflectionProbes = true;
bool gMapSabers = true;
bool gSaberLights = false;
bool gFlatScreenGuard = true;
bool gFullReflectionProbes = false;
bool gSingleViewMapCameras = true;
bool gAutoEffectResolution = true;

// Auto Effect Resolution (0.15.3). Final Slaughter (#100) runs a three-pass
// full-screen effect at full resolution on top of a depth pass for most of
// the song; the Quest falls behind, its frame synthesis fills in, and notes
// look like they stop and slide back and forth. When frames stay slow against
// the best this level has managed, map effects step down 10% at a time (to
// 40% at least) and back up once there is room again. Frame times are only
// compared with each other, so 72, 90 and 120 Hz all work.
std::atomic<int> gAutoEffectPercent{100};
struct AutoEffectState {
  double average = 0.0;      // exponential moving average of frame time, s
  double best = 0.0;         // fastest average this level
  double slowFor = 0.0;      // how long the average has been well above best
  double fastFor = 0.0;      // how long it has been close to best
  double elapsed = 0.0;
} gAutoEffect;
bool gCrashRecovery = true;
std::atomic<bool> gRecoveryModeActive{false};
bool gReplayRenderMode = false;
bool gBlitFix = false;
bool gPrepareShaders = false;
bool gGeometryEffects = false;
bool gGeometryBothEyes = false;
bool gGeometryEyeCopy = true;
bool gPcShadersForEmpty = true;
// Which shader to use as the stand-in, by name, overriding the automatic pick.
//
// Empty means "choose automatically". This exists because the right answer
// depends on what a particular Beat Saber build actually ships, and that list
// is only knowable from a headset: several builds shipped with a stand-in that
// turned out to render black on the device. The session log prints every
// runnable shader name, so a name from that list can be dropped in here and
// tried immediately rather than waiting for another build.
std::string gStandInShaderName;
// Percent of the eye resolution map post-process effects render at.
int gEffectResolution = 100;
// Animators in a map's prefabs skip updating while none of their renderers is
// on screen (0.14.23).
bool gOffscreenCulling = false;
// Two loads of a converted bundle that never finish put it into the
// untranslated (grey) fallback; off, that never happens (0.14.24).
bool gCrashGuard = true;
// A Quest bundle missing shaders its PC build has is played from the converted
// PC build instead of being patched (0.14.24).
bool gPcBundleForMissingShaders = false;
// Metres beyond which the main camera draws nothing; 0 is off (0.14.23).
int gFarCullingDistance = 0;
int gFoveationLevel = 0;
int gFieldOfView = 100;

// Both diagnostic files live beside the mod's own data, and both are .txt.
//
// This used to be Logs/Vivify.log. A .log file has no default handler on Android
// or Windows, so tapping it does nothing and it looks like no log exists at all
// -- and it sat in a different directory from the per-level report, so there
// were two places to look. One directory, two .txt files, both openable.
constexpr std::string_view kVivifyLogDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify";
constexpr std::string_view kVivifyLogPath =
    "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt";

// A session log must not fill a headset, and it is truncated at launch anyway,
// so this only has to bound one play session.
constexpr std::streamoff kVivifyLogMaxBytes = 8 * 1024 * 1024;

std::ofstream gVivifyLogFile;
std::mutex gVivifyLogMutex;
// vivify_global.txt: every session appended, never truncated; 9 MB parts
// (vivify_global-p2.txt, ...), the older half deleted past 1 GB in total.
Vivify::GlobalLog gVivifyGlobalLog(std::string(kVivifyLogDir), "vivify_global", 9ull * 1024 * 1024,
                                   1024ull * 1024 * 1024);

std::string WallClockText() {
  std::time_t const now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}
bool gVivifyLogSinkInstalled = false;
bool gVivifyLogCapped = false;
std::chrono::steady_clock::time_point gVivifyLogLastFlush{};
std::atomic<int64_t> gVivifyFlushEveryLineUntil{0};  // steady_clock ticks

void InstallVivifyFileLogSink() {
  if (gVivifyLogSinkInstalled) return;
  gVivifyLogSinkInstalled = true;
  std::error_code ec;
  std::filesystem::create_directories(std::string(kVivifyLogDir), ec);

  // Each of the three files is opened on its own: one that cannot be opened
  // used to return from here before the others were even tried.
  errno = 0;
  gVivifyLogFile.open(std::string(kVivifyLogPath), std::ios::out | std::ios::trunc);
  int const sessionOpenErrno = errno;
  if (gVivifyLogFile.is_open()) {
    gVivifyLogFile << "=== Vivify " << VERSION << kBuildLabel << (kLiteBuild ? " (lite)" : "") << " session log, started "
                   << WallClockText() << " ===\n";
    gVivifyLogFile.flush();
  }
  gVivifyLogLastFlush = std::chrono::steady_clock::now();
  try {
    gVivifyGlobalLog.Open("=== Vivify " + std::string(VERSION) + kBuildLabel + (kLiteBuild ? " (lite)" : "") + " session started " +
                          WallClockText() + " ===");
  } catch (...) {
  }

  Paper::Logger::AddLogSink([](Paper::LogData const& data) {
    if (!data.tag.has_value() || *data.tag != std::string_view(MOD_ID)) return;
    std::lock_guard<std::mutex> lock(gVivifyLogMutex);
    bool const important = data.level >= Paper::LogLevel::WRN;
    auto const now = std::chrono::steady_clock::now();
    bool const everyLine = now.time_since_epoch().count() < gVivifyFlushEveryLineUntil.load();
    if (gVivifyGlobalLog.IsOpen()) {
      gVivifyGlobalLog.Write(WallClockText() + " [" + std::string(Paper::format_as(data.level)) + "] " +
                             std::string(data.message));
      if (important || everyLine) gVivifyGlobalLog.Flush();
    }
    if (!gVivifyLogFile.is_open() || gVivifyLogCapped) return;

    gVivifyLogFile << '[' << Paper::format_as(data.level) << "] " << data.message << '\n';

    if (gVivifyLogFile.tellp() > kVivifyLogMaxBytes) {
      gVivifyLogFile << "=== log capped at " << (kVivifyLogMaxBytes / (1024 * 1024))
                     << "MB; further lines go to logcat only ===\n";
      gVivifyLogFile.flush();
      gVivifyLogCapped = true;
      return;
    }

    // Flushing every line meant an sdcard write per log line, on whichever
    // thread logged -- including the main thread mid-gameplay, where Vivify can
    // be noisy. Warnings and errors still flush immediately, because those are
    // the lines that matter if the game stops before the buffer is written;
    // ordinary lines are flushed at most a few times a second.
    if (important || everyLine || now - gVivifyLogLastFlush > std::chrono::milliseconds(250)) {
      gVivifyLogFile.flush();
      gVivifyGlobalLog.Flush();
      gVivifyLogLastFlush = now;
    }
  });

  // Where each file went, or exactly why it did not, as the first lines every
  // file that did open will carry. A report of "the global log is never
  // created" can then be answered from the session log alone.
  if (gVivifyLogFile.is_open()) {
    PaperLogger.info("Vivify log files: session -> {}", kVivifyLogPath);
  } else {
    PaperLogger.warn("Vivify log files: session log could not be opened at {}: {}", kVivifyLogPath,
                     sessionOpenErrno != 0 ? std::strerror(sessionOpenErrno) : "unknown error");
  }
  if (gVivifyGlobalLog.IsOpen()) {
    PaperLogger.info("Vivify log files: global -> {}", gVivifyGlobalLog.CurrentPath().string());
  } else {
    PaperLogger.warn("Vivify log files: global log could not be opened: {}",
                     gVivifyGlobalLog.LastError().empty() ? "unknown reason" : gVivifyGlobalLog.LastError());
  }
  if (Vivify::Report::Append("SESSION STARTED", "Vivify " + std::string(VERSION) + (kLiteBuild ? " (lite)" : "") +
                                                    "; one block per level follows.\n")) {
    PaperLogger.info("Vivify log files: report -> {}", Vivify::Report::FilePath());
  } else {
    PaperLogger.warn("Vivify log files: report could not be written at {}: {}", Vivify::Report::FilePath(),
                     Vivify::Report::LastError());
  }
}

}  // namespace

// Writes everything logged so far to the session and global logs now. For the
// steps around a level's end (0.15.0): a native crash there loses whatever is
// still buffered, and the last step that reached the file is the clue.
void FlushVivifyLogsEveryLineFor(int seconds) {
  auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  gVivifyFlushEveryLineUntil.store(until.time_since_epoch().count());
  FlushVivifyLogs();
}

void FlushVivifyLogs() {
  std::lock_guard<std::mutex> lock(gVivifyLogMutex);
  if (gVivifyLogFile.is_open()) gVivifyLogFile.flush();
  gVivifyGlobalLog.Flush();
}

namespace {
void EnsureConfigObject() {
  auto& doc = getConfig().config;
  if (!doc.IsObject()) {
    doc.SetObject();
  }
}

// The string equivalent of EnsureBoolConfigValue, for settings whose value is a
// name rather than a switch.
bool EnsureStringConfigValue(std::string_view key, std::string const& defaultValue,
                             std::string& value) {
  auto& doc = getConfig().config;
  auto it = doc.FindMember(key.data());
  if (it != doc.MemberEnd() && it->value.IsString()) {
    value.assign(it->value.GetString(), it->value.GetStringLength());
    return false;
  }

  auto& allocator = doc.GetAllocator();
  value = defaultValue;
  rapidjson::Value stored(defaultValue.c_str(), static_cast<rapidjson::SizeType>(defaultValue.size()),
                          allocator);
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), stored, allocator);
  } else {
    it->value = stored;
  }
  return true;
}

// The integer equivalent of EnsureBoolConfigValue; a stored value outside
// [minValue, maxValue] is replaced by the default.
bool EnsureIntConfigValue(std::string_view key, int defaultValue, int minValue, int maxValue, int& value) {
  auto& doc = getConfig().config;
  auto it = doc.FindMember(key.data());
  if (it != doc.MemberEnd() && it->value.IsInt() && it->value.GetInt() >= minValue &&
      it->value.GetInt() <= maxValue) {
    value = it->value.GetInt();
    return false;
  }

  auto& allocator = doc.GetAllocator();
  value = defaultValue;
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(defaultValue), allocator);
  } else {
    it->value.SetInt(defaultValue);
  }
  return true;
}

bool EnsureBoolConfigValue(std::string_view key, bool defaultValue, bool& value) {
  auto& doc = getConfig().config;
  auto it = doc.FindMember(key.data());
  if (it != doc.MemberEnd() && it->value.IsBool()) {
    value = it->value.GetBool();
    return false;
  }

  auto& allocator = doc.GetAllocator();
  value = defaultValue;
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(defaultValue), allocator);
  } else {
    it->value.SetBool(defaultValue);
  }
  return true;
}

// True only while the settings view controller is being constructed.
//
// BSML toggles are live the moment they exist, and a toggle that fires its
// change callback while it is still being set up writes that transient value
// straight through to the config file -- which is how "Stand-In Shading" went
// from on to off mid-session without anybody touching it, and with it every
// converted map's geometry stopped being repaired. A player cannot tap a
// control that is not on screen yet, so any change arriving in this window is
// construction noise and is dropped.
bool gSettingsMenuBuilding = false;

void SetBoolConfigValue(std::string_view key, bool enabled, bool& value) {
  if (gSettingsMenuBuilding) {
    PaperLogger.info("Vivify settings: ignoring a '{}' change to {} that arrived while the menu was "
                     "still being built",
                     key, enabled ? "on" : "off");
    return;
  }
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(key.data());
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(enabled), allocator);
  } else {
    it->value.SetBool(enabled);
  }
  value = enabled;
  config.Write();
}

// "Use Windows Bundle For This Map" (0.14.29): song folder names whose PC
// bundle is converted and played even though they ship a Quest bundle.
constexpr char const* kWindowsBundleMapsConfigKey = "windowsBundleMaps";

std::string MapKeyOf(std::string const& levelPath) {
  std::string trimmed = levelPath;
  while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\')) trimmed.pop_back();
  return std::filesystem::path(trimmed).filename().string();
}

void SetIntConfigValue(std::string_view key, int newValue, int& value) {
  if (gSettingsMenuBuilding) {
    PaperLogger.info("Vivify settings: ignoring a '{}' change to {} that arrived while the menu was "
                     "still being built",
                     key, newValue);
    return;
  }
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(key.data());
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(newValue), allocator);
  } else {
    it->value.SetInt(newValue);
  }
  value = newValue;
  config.Write();
}

// Label under the bulk-convert button. The settings view controller is
// destroyed and rebuilt as the player navigates and conversion progress
// arrives asynchronously, so SafePtrUnity is used for its destroyed-object
// aware liveness check -- a plain SafePtr is not even permitted for Unity
// types.
SafePtrUnity<HMUI::CurvedTextMeshPro> gConvertStatusText;

// Settings layout (0.15.1, corrected in 0.15.3 build 2). Two things overlapped
// in the Vivify tab:
//  - a setting's label is laid out from the left edge with no width limit,
//    so a long one ran under its toggle or increment buttons. Each label now
//    shrinks to fit the room it has (TMP auto-size, down to 60%);
//  - a text whose content changes after the menu is built (conversion
//    status, the This Map row) kept the height of its first content, and a
//    longer message then drew over the widgets below it.
//
// BSML's settings list does not control its children's heights
// (childControlHeight is off), so a LayoutElement height is ignored there and
// a BSML text keeps the 0 x 0 rect it is created with. 0.15.1 gave those texts
// word wrap and an ellipsis on top of that zero-height rect, and TMP then cut
// every line away: the conversion progress, the This Map hint and the logs
// line showed nothing at all. The rect itself now gets the height, and text
// that still does not fit is drawn rather than hidden.
// One or two sentences per setting, shown when the pointer rests on it
// (0.15.4). Keyed by the control's label; nullptr when there is none.
char const* SettingHint(std::u16string_view label) {
  static std::pair<std::u16string_view, char const*> const kHints[] = {
      {u"Use Windows Bundle",
       "For the selected map only: use its PC (Windows) bundle, converted on the headset, instead of its Quest "
       "bundle. Try it when a map's Quest version looks wrong or is missing things."},
      {u"Effect Resolution %",
       "Draws full-screen map effects at this share of the eye resolution, then scales them up. Lower is "
       "faster and blurrier; 50% is a quarter of the work."},
      {u"Auto Effect Resolution",
       "Lowers the effect resolution by itself while a map runs slowly, and raises it again when there is "
       "room."},
      {u"Offscreen Culling (can hide objects)",
       "Stops animating map objects you are not looking at, to save time. Can leave some objects hidden, so "
       "turn it off if things go missing."},
      {u"Far Culling (m, 0 = off)",
       "Does not draw anything farther away than this many metres. 0 draws everything."},
      {u"Field Of View %",
       "During Vivify maps, shows more of the scene at once (smaller), so effects at the edge of the view come "
       "into it. Above 100 the world no longer moves exactly with your head, which can feel uncomfortable."},
      {u"Foveated Rendering (0 = game's)",
       "During Vivify maps, renders the edges of your view at lower detail to save performance, using the "
       "headset's own foveation: 1 is mild, 4 is strongest. Uses eye tracking where the headset has it. 0 leaves "
       "the game's setting alone."},
      {u"Disable All Blits",
       "Turns off every full-screen effect a map adds (colour grading, distortion, flashes). Use it when an "
       "effect covers the screen or makes a map lag."},
      {u"Flat-Screen Effect Guard",
       "Every 2 seconds while a map's screen effect is on, checks 16 spots of the view. If they are all one or two "
       "colours (and the scene itself is not), turns every screen effect off for the rest of the level."},
      {u"Submit Scores On Vivify Maps",
       "Lets scores from Vivify maps go to leaderboards, and lets replays be recorded. Off stops both."},
      {u"Disable Custom Note Visuals",
       "Keeps the game's normal notes, bombs and debris instead of the ones a map brings."},
      {u"Use Map Sabers",
       "On: sabers a map brings replace yours, including custom sabers from another mod; they stay attached "
       "to the saber, so trick mods still move them. Off: you always keep your own sabers."},
      {u"Saber Lights On Map Objects",
       "Some saber mods put a light on each saber. Off (default): those lights are switched off during Vivify "
       "maps, where they light the map's objects, flicker, and cost a lot of performance. On keeps them."},
      {u"No Vivify Visuals In Multiplayer",
       "In multiplayer, plays Vivify maps without their custom notes and sabers."},
      {u"AudioLink",
       "Feeds the song's sound to map shaders that react to music. Off saves a little time; those effects "
       "then stay still."},
      {u"Convert All PC Bundles",
       "Converts every installed PC-only Vivify map now, so they are ready to play. Progress shows above."},
      {u"Debug Logging",
       "Writes much more detail to the Vivify log files. Turn it on before reproducing a problem you want to "
       "report; it costs some performance."},
      {u"Convert PC Bundles On Device",
       "Lets the mod convert maps made only for PC so they can load on the headset. Off: only maps with a "
       "Quest bundle work."},
      {u"Translate Shaders",
       "Converts a PC map's shaders so its materials look as intended. Off: converted maps use plain stand-in "
       "shading instead."},
      {u"Stand-In Shading",
       "Draws objects whose shader could not be used with a simple substitute, so they are at least visible. "
       "Off hides them."},
      {u"Scene Depth For Map Shaders",
       "Gives map shaders the scene's depth, which fog, soft edges and some screen effects need. Costs an "
       "extra render of the scene on maps that use it."},
      {u"Map Realtime Shadows", "Allows shadows from a map's own lights. They are expensive on a Quest."},
      {u"Map Reflection Probes",
       "Allows a map's live reflections, at reduced quality. Off removes them and saves time and memory."},
      {u"Full Reflection Probes",
       "Runs a map's live reflections at the quality the map asked for, not reduced. Heavy; can crash on "
       "big maps."},
      {u"Single-View Map Cameras",
       "Fixes a map's in-world screens, portals and mirrors (its own cameras drawing into textures) on "
       "converted maps."},
      {u"Crash Recovery Mode",
       "After a map crashes or freezes, plays it next time with less memory use and without the effect that "
       "was starting when it went down."},
      {u"Forget Crash Recovery", "Clears the list of maps that crashed, so they play normally again."},
      {u"Geometry Effects In Right Eye",
       "Grass, sparkles and wireframes of PC maps are drawn by a shader stage the Quest only runs for the left "
       "eye. On: each is drawn again for the right eye. Turn off if those effects look doubled. Changing it "
       "reconverts the map."},
      {u"Flat Geometry Effects",
       "Draws grass, fur and wireframe effects of PC maps without their geometry shader: plain meshes, and "
       "some effects then draw nothing. Only for a map where those effects still go wrong. Changing it "
       "reconverts the map."},
      {u"Geometry-Shader Effects (exp.)",
       "Experimental. Draws effects that need a geometry shader (wireframes, exploding triangles) as plain "
       "meshes when they cannot be converted. Changing it reconverts affected maps."},
      {u"PC Bundle For Missing Shaders",
       "When a map's Quest bundle is missing shaders, uses the converted PC bundle instead."},
      {u"Prepare Shaders Before Playing",
       "Compiles a map's shaders when you select it rather than during play. Fewer hitches in the song, "
       "but a longer wait in the menu; has frozen the menu on some maps."},
      {u"Blit Fix For Screen Textures (exp.)",
       "Experimental. Converts a second, one-eye version of a map's full-screen effects, for effects that "
       "come out black or doubled."},
      {u"Crash Guard",
       "If a converted map fails to load twice, converts it again without shader translation so it at least "
       "loads, with plain shading."},
      {u"Skip Beat-0 Film Grain Blit",
       "Skips a full-screen effect that starts at the very beginning of a map, usually film grain or a "
       "vignette."},
      {u"No Depth On Map Cameras",
       "Stops cameras a map creates from rendering depth. Saves time; effects that need depth can look wrong."},
      {u"Ignore VR Center Adjust",
       "Ignores your room offset and rotation setting during Vivify maps, for maps that look misplaced with "
       "it."},
      {u"Replay Render Mode",
       "Prepares a version of each map that the Replay mod can render to video. Only matters while rendering "
       "a replay."},
      {u"Update Vivify",
       "Downloads the newest release of Vivify from GitHub and installs it for the next time you start the game."},
      {u"Install Nightly",
       "Installs the latest nightly build: the newest changes, untested. Takes effect after restarting the game."},
      {u"Install Dev Build",
       "Installs the most recent build of the development branch, newer than the nightly and less tested."},
      {u"Converter Version",
       "Picks a Vivify release, 0.13.5 or later, with the converter it shipped with. The converter is part of the "
       "mod, so this installs that whole version."},
      {u"Install Chosen Version", "Installs the version picked above. Takes effect after restarting the game."},
      {u"Force Reconvert All",
       "Throws away every converted map and converts them all again. Use after a mod update if maps still "
       "look wrong."},
      {u"Reset Vivify (clean slate)",
       "Deletes converted maps and downloaded PC bundles and resets these settings. Your maps stay. Press "
       "twice to confirm."},
      {u"Redownload All Vivify Maps",
       "Replaces every Vivify map with a fresh download from BeatSaver. Press twice to confirm."},
  };
  for (auto const& [name, hint] : kHints) {
    if (name == label) return hint;
  }
  return nullptr;
}

void FitSettingLabels(UnityEngine::Component* widget) {
  if (widget == nullptr) return;
  auto gameObject = widget->get_gameObject();
  if (!gameObject) return;
  auto texts = gameObject->GetComponentsInChildren<TMPro::TextMeshProUGUI*>(true);
  for (auto* text : texts) {
    if (text == nullptr) continue;
    float const size = text->get_fontSize();
    text->set_enableWordWrapping(false);
    text->set_fontSizeMax(size);
    text->set_fontSizeMin(size * 0.6f);
    text->set_enableAutoSizing(true);
  }
}

void FixTextHeight(HMUI::CurvedTextMeshPro* text, int lines, float lineHeight = 4.5f) {
  if (text == nullptr) return;
  float const height = static_cast<float>(lines) * lineHeight;
  text->set_enableWordWrapping(true);
  text->set_overflowMode(TMPro::TextOverflowModes::Overflow);
  auto* rect = reinterpret_cast<UnityEngine::RectTransform*>(text->get_transform().unsafePtr());
  auto size = rect->get_sizeDelta();
  size.y = height;
  rect->set_sizeDelta(size);
  auto* layout = text->get_gameObject()->GetComponent<UnityEngine::UI::LayoutElement*>();
  if (layout == nullptr) layout = text->get_gameObject()->AddComponent<UnityEngine::UI::LayoutElement*>();
  layout->set_minHeight(height);
  layout->set_preferredHeight(height);
}

// The latest conversion status, kept so the label shows it again whenever the
// menu is opened, not only while it was open when the progress arrived.
std::string gConvertStatus = "Idle";

void SetConvertStatusText(std::string const& text) {
  gConvertStatus = text;
  if (!gConvertStatusText) return;
  gConvertStatusText->set_text(StringW(text));
}

// A bar of 30 ticks, filled to done/total, in plain ASCII so the menu font has it.
std::string ProgressBarText(int done, int total) {
  constexpr int kTicks = 30;
  int filled = total > 0 ? std::clamp(done * kTicks / total, 0, kTicks) : 0;
  int const percent = total > 0 ? std::clamp(done * 100 / total, 0, 100) : 0;
  return "<color=#5fd35f>" + std::string(static_cast<size_t>(filled), '|') + "</color><color=#555555>" +
         std::string(static_cast<size_t>(kTicks - filled), '|') + "</color> " + std::to_string(percent) + "%";
}

// Settings menu state (0.14.29): the advanced widgets the "Show Advanced
// Settings" button hides and shows, and the This Map row.
std::vector<SafePtrUnity<UnityEngine::GameObject>> gAdvancedWidgets;
bool gShowAdvanced = false;
SafePtrUnity<UnityEngine::UI::Button> gAdvancedButton;
SafePtrUnity<HMUI::CurvedTextMeshPro> gThisMapText;
SafePtrUnity<UnityEngine::UI::Button> gThisMapButton;

void RefreshThisMapRow() {
  std::string const path = Vivify::SelectedVivifyMapPath();
  std::string name = path;
  while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
  name = std::filesystem::path(name).filename().string();
  bool const enabled = !path.empty() && GetWindowsBundleForMap(path);
  if (gThisMapText) {
    std::string line;
    if (path.empty()) {
      line = "<size=80%><color=#ffcc66>No Vivify map selected: pick one in the song list (and a difficulty "
             "that needs Vivify), then come back here</color></size>";
    } else {
      line = "<size=80%>" + name + "\n" +
             (enabled ? "<color=#5fd35f>Using its Windows (PC) bundle, converted on the headset</color>"
                      : "Using its Quest bundle when it has one") +
             "</size>";
    }
    gThisMapText->set_text(StringW(line));
  }
  if (gThisMapButton) {
    BSML::Lite::SetButtonText(gThisMapButton.ptr(), StringW(path.empty() ? std::string("Use Windows Bundle: -")
                                                                         : std::string("Use Windows Bundle: ") +
                                                                               (enabled ? "on" : "off")));
  }
}

void RegisterModSettings() {
  BSML::BSMLSettings::get_instance()->TryAddSettingsMenu(
      [](HMUI::ViewController* viewController, bool firstActivation, bool, bool) {
        if (viewController == nullptr) return;
        // The selected map can change between visits, so its row is refreshed
        // on every activation, not only when the menu is built.
        if (!firstActivation) {
          RefreshThisMapRow();
          SetConvertStatusText(gConvertStatus);
          return;
        }
        gSettingsMenuBuilding = true;
        // Cleared however this scope is left, including through the catch below.
        struct BuildGuard {
          ~BuildGuard() { gSettingsMenuBuilding = false; }
        } buildGuard;

        // The whole menu is built inside a try/catch because it is built inside
        // a callback the game invokes: anything that throws here abandons the
        // rest of the construction and leaves the settings tab wedged, with the
        // game still running around it. That is exactly what the version label
        // added in 0.9.2 did -- it was the first widget in the list, so when it
        // threw, every control after it simply never existed and the menu could
        // not be used at all.
        //
        // A failure now costs the widgets after it and says so in the log,
        // instead of costing the menu.
        try {
        auto* container = BSML::Lite::CreateScrollableSettingsContainer(viewController->get_transform());
        if (container == nullptr) return;
        UnityEngine::Transform* parent = container->get_transform().unsafePtr();
        gAdvancedWidgets.clear();
        // The list sat off the left edge of the panel and ran its full width:
        // room on both sides makes it narrower and keeps it inside (0.15.10).
        if (auto* layout = container->GetComponent<UnityEngine::UI::VerticalLayoutGroup*>(); layout != nullptr) {
          layout->set_padding(UnityEngine::RectOffset::New_ctor(9, 9, 0, 0));
        }

        // Which build is actually running, in the headset, without a file.
        // "the new features do not work" and "the new build did not install"
        // look identical from the outside, and a version number here separates
        // them in one glance.
        //
        // Built as a StringW from a std::string, the way every other text in
        // this menu is: the std::u16string this used to assemble by hand is
        // what took the menu down.
        FixTextHeight(
            BSML::Lite::CreateText(parent, StringW(std::string("Vivify ") + VERSION + kBuildLabel + (kLiteBuild ? " (lite)" : ""))),
            1);

        // 0.14.29: the everyday settings come first and everything else sits
        // behind "Show Advanced Settings", which hides or shows those widgets
        // in place (an inactive child drops out of the list's layout). Labels
        // are short enough to fit beside their control. Controls that exist
        // only in the full build are skipped in lite, not shown disabled.
        auto header = [&](std::string const& title) {
          auto* text = BSML::Lite::CreateText(parent, StringW("<size=110%><b>" + title + "</b></size>"));
          FixTextHeight(text, 1, 6.0f);
          return text;
        };
        // Registers a widget as advanced: hidden until the button below shows it.
        auto advanced = [](auto* widget) {
          if (widget == nullptr) return;
          auto gameObject = widget->get_gameObject();
          if (!gameObject) return;
          gAdvancedWidgets.emplace_back(gameObject.unsafePtr());
          gameObject->SetActive(gShowAdvanced);
        };
        // Hover hints (0.15.4): point at a setting or button and a line or
        // two says what it does. The text is looked up by the control's label
        // (SettingHint), so a control without an entry simply has no hint.
        auto hinted = [](auto* widget, std::u16string_view label) {
          if (widget == nullptr) return widget;
          char const* hint = SettingHint(label);
          if (hint == nullptr) return widget;
          auto gameObject = widget->get_gameObject();
          if (gameObject) BSML::Lite::AddHoverHint(gameObject.unsafePtr(), StringW(std::string(hint)));
          return widget;
        };
        auto toggle = [&](std::u16string_view label, bool value, std::function<void(bool)> onChange) {
          auto* created = BSML::Lite::CreateToggle(parent, StringW(label), value, std::move(onChange));
          FitSettingLabels(reinterpret_cast<UnityEngine::Component*>(created));
          return hinted(created, label);
        };
        auto increment = [&](std::u16string_view label, float step, float value, float min, float max,
                             std::function<void(float)> onChange) {
          auto* created = BSML::Lite::CreateIncrementSetting(parent, StringW(label), 0, step, value, true, true, min,
                                                             max, std::move(onChange));
          FitSettingLabels(reinterpret_cast<UnityEngine::Component*>(created));
          return hinted(created, label);
        };
        auto button = [&](std::u16string_view label, std::function<void()> onClick) {
          return hinted(BSML::Lite::CreateUIButton(parent, StringW(label), std::move(onClick)), label);
        };

        if constexpr (!kLiteBuild) {
          header("This Map");
          gThisMapText = BSML::Lite::CreateText(parent, u"");
          FixTextHeight(gThisMapText.ptr(), 2);
          gThisMapButton = BSML::Lite::CreateUIButton(parent, u"Use Windows Bundle: -", []() {
            std::string const path = Vivify::SelectedVivifyMapPath();
            if (path.empty()) {
              RefreshThisMapRow();
              return;
            }
            bool const enabled = !GetWindowsBundleForMap(path);
            SetWindowsBundleForMap(path, enabled);
            PaperLogger.info("Vivify settings: Windows bundle for '{}' turned {}", path, enabled ? "on" : "off");
            Vivify::ReselectSelectedMap();
            RefreshThisMapRow();
          });
          hinted(gThisMapButton.ptr(), u"Use Windows Bundle");
          RefreshThisMapRow();
        }

        header("Performance");
        if constexpr (!kLiteBuild) {
          // Renders map post-process effects below eye resolution and scales
          // them up. Full-screen raymarchers cost per pixel: 50% is a quarter of
          // the work. Steps of 5% (0.14.26).
          increment(u"Effect Resolution %", 5.0f, static_cast<float>(gEffectResolution), 25.0f, 100.0f,
              [](float value) {
                int const percent = std::clamp(static_cast<int>(value + 0.5f), 25, 100);
                SetIntConfigValue(kEffectResolutionConfigKey, percent, gEffectResolution);
              });
          // Steps map effects down while a map runs slow, back up when there
          // is room (0.15.3).
          toggle(u"Auto Effect Resolution", gAutoEffectResolution, [](bool value) {
            SetBoolConfigValue(kAutoEffectResolutionConfigKey, value, gAutoEffectResolution);
          });
          toggle(u"Offscreen Culling (can hide objects)", gOffscreenCulling,
                 [](bool value) { SetBoolConfigValue(kOffscreenCullingConfigKey, value, gOffscreenCulling); });
          // Draws nothing farther than this from the player; 0 is off. Steps of
          // 10 m (0.14.26).
          increment(u"Far Culling (m, 0 = off)", 10.0f, static_cast<float>(gFarCullingDistance), 0.0f, 1000.0f,
              [](float value) {
                int const metres = std::clamp(static_cast<int>(value + 0.5f), 0, 1000);
                SetIntConfigValue(kFarCullingConfigKey, metres, gFarCullingDistance);
              });
        }
        increment(u"Field Of View %", 5.0f, static_cast<float>(gFieldOfView), 100.0f, 150.0f, [](float value) {
          int const percent = std::clamp(static_cast<int>(value + 0.5f), 100, 150);
          SetIntConfigValue(kFieldOfViewConfigKey, percent, gFieldOfView);
        });
        increment(u"Foveated Rendering (0 = game's)", 1.0f, static_cast<float>(gFoveationLevel), 0.0f, 4.0f,
                  [](float value) {
                    int const level = std::clamp(static_cast<int>(value + 0.5f), 0, 4);
                    SetIntConfigValue(kFoveationConfigKey, level, gFoveationLevel);
                  });
        toggle(u"Disable All Blits", GetDisableAllBlits(),
               [](bool value) { SetBoolConfigValue(kDisableAllBlitsConfigKey, value, gDisableAllBlits); });

        toggle(u"Flat-Screen Effect Guard", gFlatScreenGuard,
               [](bool value) { SetBoolConfigValue(kFlatScreenGuardConfigKey, value, gFlatScreenGuard); });

        header("Gameplay");
        toggle(u"Submit Scores On Vivify Maps", GetSubmitScoresOnVivifyMaps(),
               [](bool value) { SetBoolConfigValue(kSubmitScoresConfigKey, value, gSubmitScores); });
        toggle(u"Disable Custom Note Visuals", GetDisableCustomNoteVisuals(), [](bool value) {
          SetBoolConfigValue(kDisableCustomNoteVisualsConfigKey, value, gDisableCustomNoteVisuals);
        });
        toggle(u"Use Map Sabers", gMapSabers,
               [](bool value) { SetBoolConfigValue(kMapSabersConfigKey, value, gMapSabers); });
        toggle(u"Saber Lights On Map Objects", gSaberLights,
               [](bool value) { SetBoolConfigValue(kSaberLightsConfigKey, value, gSaberLights); });
        toggle(u"No Vivify Visuals In Multiplayer", GetDisableVisualsInMultiplayer(), [](bool value) {
          SetBoolConfigValue(kDisableVisualsInMultiplayerConfigKey, value, gDisableVisualsInMultiplayer);
        });
        toggle(u"AudioLink", GetAudioLinkEnabled(),
               [](bool value) { SetBoolConfigValue(kAudioLinkConfigKey, value, gAudioLink); });

        header("PC Maps");
        // A map whose only asset bundle is a PC build has its play button
        // disabled, so it can never be selected into. These convert every
        // installed map in one pass instead.
        gConvertStatusText = BSML::Lite::CreateText(parent, u"Idle");
        FixTextHeight(gConvertStatusText.ptr(), 3);
        SetConvertStatusText(Vivify::IsBulkPcBundleConversionRunning() && gConvertStatus == "Idle" ? "Converting..."
                                                                                                    : gConvertStatus);
        // One progress callback for both buttons; they differ only in whether an
        // already-cached conversion is reused or thrown away first.
        static auto const startConversion = [](bool force) {
          if (Vivify::IsBulkPcBundleConversionRunning()) return;
          SetConvertStatusText(force ? "Reconverting..." : "Scanning...");
          Vivify::StartBulkPcBundleConversion(
              [](Vivify::BulkConversionProgress const& progress) {
                if (progress.finished) {
                  SetConvertStatusText(progress.status);
                  return;
                }
                SetConvertStatusText(ProgressBarText(progress.levelsScanned, progress.levelsTotal) + "\n" +
                                     std::to_string(progress.levelsScanned) + "/" +
                                     std::to_string(progress.levelsTotal) + "  <size=80%>" + progress.status +
                                     "</size>");
              },
              force);
        };
        button(u"Convert All PC Bundles", []() { startConversion(false); });

        header("Updates");
        // Installs the newest numbered release in place of this build, for the
        // next start of the game (0.15.12, VivifyUpdater.cpp).
        button(u"Update Vivify", []() {
          SetConvertStatusText("Checking for an update...");
          Vivify::Updater::Install(Vivify::Updater::Channel::Stable, "",
                                   [](std::string const& text) { SetConvertStatusText(text); });
        });

        header("Diagnostics");
        toggle(u"Debug Logging", GetVivifyDebugLogging(),
               [](bool value) { SetBoolConfigValue(kVivifyDebugLoggingConfigKey, value, gVivifyDebugLogging); });
        // paperlog output is not reachable without adb, so Vivify writes its own
        // plain-text files next to its data, all in this one folder.
        FixTextHeight(BSML::Lite::CreateText(parent, StringW("<size=70%>Logs in " + std::string(kVivifyLogDir) +
                                                             ":\nVivifySession.txt, VivifyReport.txt" +
                                                             (kLiteBuild ? "" : ", vivify_global.txt") + "</size>")),
                      2, 3.5f);

        gAdvancedButton = BSML::Lite::CreateUIButton(parent, u"Show Advanced Settings", []() {
          gShowAdvanced = !gShowAdvanced;
          for (auto& widget : gAdvancedWidgets) {
            if (widget) widget->SetActive(gShowAdvanced);
          }
          if (gAdvancedButton) {
            BSML::Lite::SetButtonText(gAdvancedButton.ptr(),
                                      gShowAdvanced ? u"Hide Advanced Settings" : u"Show Advanced Settings");
          }
        });

        advanced(header("Shaders"));
        advanced(toggle(u"Convert PC Bundles On Device", GetConvertPcBundlesOnDevice(), [](bool value) {
          SetBoolConfigValue(kConvertPcBundlesOnDeviceConfigKey, value, gConvertPcBundlesOnDevice);
        }));
        advanced(toggle(u"Translate Shaders", GetTranslateShadersOnConversion(),
                        [](bool value) { SetBoolConfigValue(kTranslateShadersConfigKey, value, gTranslateShaders); }));
        advanced(toggle(u"Stand-In Shading", GetStandInShading(),
                        [](bool value) { SetBoolConfigValue(kStandInShadingConfigKey, value, gStandInShading); }));
        advanced(toggle(u"Scene Depth For Map Shaders", GetSceneDepthTexture(),
                        [](bool value) { SetBoolConfigValue(kSceneDepthConfigKey, value, gSceneDepth); }));
        advanced(toggle(u"Map Realtime Shadows", GetMapRealtimeShadows(), [](bool value) {
          SetBoolConfigValue(kMapRealtimeShadowsConfigKey, value, gMapRealtimeShadows);
        }));
        // Realtime probes re-render the scene into a cubemap; on by default
        // in a reduced form (128 px, one face a frame, no HDR), full quality
        // only on request. A map that went down plays without them.
        advanced(toggle(u"Map Reflection Probes", GetMapReflectionProbes(), [](bool value) {
          SetBoolConfigValue(kMapReflectionProbesConfigKey, value, gMapReflectionProbes);
        }));
        advanced(toggle(u"Full Reflection Probes", GetFullReflectionProbes(), [](bool value) {
          SetBoolConfigValue(kFullReflectionProbesConfigKey, value, gFullReflectionProbes);
        }));
        if constexpr (!kLiteBuild) {
          // A map's own cameras that render into textures (TV screens,
          // portals, light meshes) draw with single-view shaders (0.15.3).
          advanced(toggle(u"Single-View Map Cameras", gSingleViewMapCameras, [](bool value) {
            SetBoolConfigValue(kSingleViewMapCamerasConfigKey, value, gSingleViewMapCameras);
          }));
        }
        if constexpr (!kLiteBuild) {
          // A map that crashed or froze last time plays in recovery mode
          // next time: less memory, and without the effect that was
          // starting when it went down (0.15.2).
          advanced(toggle(u"Crash Recovery Mode", gCrashRecovery, [](bool value) {
            SetBoolConfigValue(kCrashRecoveryConfigKey, value, gCrashRecovery);
          }));
          advanced(button(u"Forget Crash Recovery", []() {
            size_t const count = Vivify::Recovery::Entries().size();
            Vivify::Recovery::ForgetAll();
            PaperLogger.info("Vivify settings: crash recovery list cleared ({} entr{})", count,
                             count == 1 ? "y" : "ies");
            SetConvertStatusText("Crash recovery cleared (" + std::to_string(count) + " entr" +
                                 (count == 1 ? "y" : "ies") + ")");
          }));
        }
        // Experimental: geometry-stage effects are drawn without that stage
        // instead of as grey stand-ins. Changing it reconverts affected maps.
        advanced(toggle(u"Geometry Effects In Right Eye", gGeometryEyeCopy, [](bool value) {
          SetBoolConfigValue(kGeometryEyeCopyConfigKey, value, gGeometryEyeCopy);
        }));
        advanced(toggle(u"Flat Geometry Effects", gGeometryBothEyes, [](bool value) {
          SetBoolConfigValue(kGeometryBothEyesConfigKey, value, gGeometryBothEyes);
        }));
        advanced(toggle(u"Geometry-Shader Effects (exp.)", GetDrawGeometryShaderEffects(),
                        [](bool value) { SetBoolConfigValue(kGeometryEffectsConfigKey, value, gGeometryEffects); }));
        if constexpr (!kLiteBuild) {
          advanced(toggle(u"PC Bundle For Missing Shaders", gPcBundleForMissingShaders, [](bool value) {
            SetBoolConfigValue(kPcBundleForMissingShadersConfigKey, value, gPcBundleForMissingShaders);
          }));
          advanced(toggle(u"Prepare Shaders Before Playing", GetPrepareShadersBeforePlaying(),
                          [](bool value) { SetBoolConfigValue(kPrepareShadersConfigKey, value, gPrepareShaders); }));
          advanced(toggle(u"Blit Fix For Screen Textures (exp.)", GetBlitScreenTextureFix(),
                          [](bool value) { SetBoolConfigValue(kBlitFixConfigKey, value, gBlitFix); }));
          // After two loads of a converted map that never finish, it is
          // reconverted without shader translation (grey stand-ins).
          advanced(toggle(u"Crash Guard", gCrashGuard,
                          [](bool value) { SetBoolConfigValue(kCrashGuardConfigKey, value, gCrashGuard); }));
        }

        advanced(header("Effects"));
        advanced(toggle(u"Skip Beat-0 Film Grain Blit", GetDisableBeat0FilmgrainBlit(), [](bool value) {
          SetBoolConfigValue(kDisableBeat0FilmgrainBlitConfigKey, value, gDisableBeat0FilmgrainBlit);
        }));
        advanced(toggle(u"No Depth On Map Cameras", GetDisableCreateCameraDepth(), [](bool value) {
          SetBoolConfigValue(kDisableCreateCameraDepthConfigKey, value, gDisableCreateCameraDepth);
        }));
        advanced(toggle(u"Ignore VR Center Adjust", GetDisableVRCenterAdjust(), [](bool value) {
          SetBoolConfigValue(kDisableVRCenterAdjustConfigKey, value, gDisableVRCenterAdjust);
        }));

        if constexpr (!kLiteBuild) {
          advanced(header("Replays"));
          // Only applies while the Replay mod renders; with it on, maps get a
          // single-view build prepared at selection for that (0.14.29).
          advanced(toggle(u"Replay Render Mode", gReplayRenderMode, [](bool value) {
            SetBoolConfigValue(kReplayRenderModeConfigKey, value, gReplayRenderMode);
          }));
        }

        advanced(header("Maintenance"));
        // A cached conversion is keyed on the source bundle, so it is reused even
        // after the converter itself has been fixed. This picks fixes up.
        advanced(button(u"Force Reconvert All", []() { startConversion(true); }));
        advanced(button(u"Install Nightly", []() {
          SetConvertStatusText("Checking the nightly build...");
          Vivify::Updater::Install(Vivify::Updater::Channel::Nightly, "",
                                   [](std::string const& text) { SetConvertStatusText(text); });
        }));
        advanced(button(u"Install Dev Build", []() {
          SetConvertStatusText("Checking the latest build of main...");
          Vivify::Updater::Install(Vivify::Updater::Channel::Dev, "",
                                   [](std::string const& text) { SetConvertStatusText(text); });
        }));
        // The converter is part of the mod, so an older converter means an older
        // build of Vivify: this lists the releases from 0.13.5 on, filled in
        // once GitHub answers.
        {
          static std::string chosenVersion = "Latest";
          std::vector<std::string_view> initial = {"Latest"};
          auto* versions = BSML::Lite::CreateDropdown(parent, u"Converter Version", StringW("Latest"),
                                                      std::span<std::string_view>(initial),
                                                      [](StringW value) { chosenVersion = std::string(value); });
          hinted(versions, u"Converter Version");
          advanced(versions);
          SafePtrUnity<BSML::DropdownListSetting> versionsPtr(versions);
          Vivify::Updater::ListVersions([versionsPtr](std::vector<std::string> const& tags, std::string const& error) {
            if (!versionsPtr) return;
            if (!error.empty()) PaperLogger.warn("Vivify updater: release list: {}", error);
            auto list = versionsPtr->values;
            if (!list) return;
            list->Clear();
            list->Add(reinterpret_cast<System::Object*>(static_cast<Il2CppString*>(StringW("Latest"))));
            for (auto const& tag : tags) list->Add(reinterpret_cast<System::Object*>(static_cast<Il2CppString*>(StringW(tag))));
            versionsPtr->UpdateChoices();
          });
          advanced(button(u"Install Chosen Version", []() {
            bool const latest = chosenVersion == "Latest";
            SetConvertStatusText("Installing " + chosenVersion + "...");
            Vivify::Updater::Install(latest ? Vivify::Updater::Channel::Stable : Vivify::Updater::Channel::Version,
                                     latest ? std::string() : chosenVersion,
                                     [](std::string const& text) { SetConvertStatusText(text); });
          }));
        }
        if constexpr (!kLiteBuild) {
          // Puts Vivify back to a fresh install's state, without touching the
          // maps. Two presses within five seconds, since it cannot be undone.
          advanced(button(u"Reset Vivify (clean slate)", []() {
            static std::chrono::steady_clock::time_point armedUntil{};
            auto const now = std::chrono::steady_clock::now();
            if (now > armedUntil) {
              armedUntil = now + std::chrono::seconds(5);
              SetConvertStatusText("Press Reset again within 5 s: deletes converted maps and downloaded PC "
                                   "bundles, resets settings");
              return;
            }
            armedUntil = {};
            if (Vivify::IsBulkPcBundleConversionRunning()) {
              SetConvertStatusText("A conversion pass is running; reset after it finishes");
              return;
            }
            EnsureConfigObject();
            getConfig().config.RemoveAllMembers();
            EnsureConfigDefaults();
            SetConvertStatusText("Resetting...");
            Vivify::ResetToCleanSlate([](std::string const& summary) {
              SetConvertStatusText(summary + ". Reopen this menu to see the settings.");
            });
          }));
          // Lists every Vivify map in VivifyMaps.txt, then replaces each with a
          // fresh BeatSaver download. Two presses within five seconds.
          advanced(button(u"Redownload All Vivify Maps", []() {
            static std::chrono::steady_clock::time_point armedUntil{};
            auto const now = std::chrono::steady_clock::now();
            if (now > armedUntil) {
              armedUntil = now + std::chrono::seconds(5);
              SetConvertStatusText("Press Redownload again within 5 s: lists Vivify maps in VivifyMaps.txt, "
                                   "then replaces each with a fresh BeatSaver download");
              return;
            }
            armedUntil = {};
            SetConvertStatusText("Finding Vivify maps...");
            Vivify::RedownloadVivifyMaps([](std::string const& text) { SetConvertStatusText(text); });
          }));
        }
        // Spacers, so the last line of the menu is not clipped by the scroll view.
        // The scroll view clips its last rows, which cut off the bottom of the
        // menu (#81); three empty lines keep the real rows clear of that.
        FixTextHeight(BSML::Lite::CreateText(parent, u" "), 3);
        } catch (std::exception const& e) {
          PaperLogger.error("Vivify settings menu: construction threw ({}); the controls after the "
                            "failure are missing", e.what());
        } catch (...) {
          PaperLogger.error("Vivify settings menu: construction threw; the controls after the "
                            "failure are missing");
        }
      },
      "Vivify", false);
}
}

Configuration &getConfig() {
  static Configuration config(modInfo);
  return config;
}

bool GetMultipassRenderingEnabled() {
  // Hard-disabled regardless of gMultipassRenderingEnabled / the stored config
  // value. When enabled, MultipassKeywordController::OnPreRender() (see
  // VivifyComponents.cpp) calls Shader::SetGlobalInt("_StereoActiveEye", eye)
  // every frame a level is active, using Camera::get_stereoActiveEye(). That
  // value is only meaningful under legacy multi-pass XR rendering; Quest
  // renders single-pass-instanced, so this writes a near-arbitrary value into
  // a GLOBAL shader property that isn't scoped to Vivify's own materials --
  // any other shader reading it (note materials, UI/menu shaders, saber and
  // arc effects) gets corrupted for as long as a level is active, including
  // while paused. That's what was causing notes to go invisible after
  // entering a level and arcs/saber effects/menus to go invisible in some
  // levels. Confirmed by removing the initializer/EnsureConfigDefaults default
  // mismatch that let a stale or manually-enabled config value take effect;
  // the previous ref1 base avoided this entirely by hardcoding it off. Leave
  // this hardcoded false until MultipassKeywordController is actually fixed
  // for single-pass-instanced rendering and verified on-device.
  return false;
}

bool GetVivifyDebugLogging() {
  return gVivifyDebugLogging;
}

bool GetDisableBeat0FilmgrainBlit() {
  return gDisableBeat0FilmgrainBlit;
}

bool GetDisableAllBlits() {
  return gDisableAllBlits;
}

bool GetDisableCreateCameraDepth() {
  return gDisableCreateCameraDepth;
}

bool GetFlatScreenGuard() {
  return gFlatScreenGuard;
}

bool GetSaberLightsOnMapObjects() {
  return gSaberLights;
}

bool GetGeometryEffectsBothEyes() {
  return gGeometryBothEyes;
}

bool GetGeometryRightEyeCopy() {
  return gGeometryEyeCopy;
}

bool GetMapSabers() {
  return gMapSabers;
}

bool GetDisableCustomNoteVisuals() {
  return gDisableCustomNoteVisuals;
}

bool GetDisableVisualsInMultiplayer() {
  return gDisableVisualsInMultiplayer;
}

bool GetDisableVRCenterAdjust() {
  return gDisableVRCenterAdjust;
}

bool GetConvertPcBundlesOnDevice() {
  return gConvertPcBundlesOnDevice;
}

bool GetStandInShading() {
  return gStandInShading;
}

bool GetSceneDepthTexture() {
  return gSceneDepth;
}

bool GetSubmitScoresOnVivifyMaps() {
  return gSubmitScores;
}

bool GetTranslateShadersOnConversion() {
  return gTranslateShaders;
}

bool GetAudioLinkEnabled() {
  return gAudioLink;
}

bool GetMapReflectionProbes() {
  return gMapReflectionProbes;
}

bool GetFullReflectionProbes() {
  return gFullReflectionProbes;
}

bool GetSingleViewMapCameras() {
  return !kLiteBuild && gSingleViewMapCameras;
}

bool GetCrashRecovery() {
  return !kLiteBuild && gCrashRecovery;
}

bool GetRecoveryModeActive() {
  return gRecoveryModeActive.load();
}

void SetRecoveryModeActive(bool active) {
  gRecoveryModeActive.store(active);
}

bool GetMapRealtimeShadows() {
  return gMapRealtimeShadows;
}

namespace {
// What conditional-dependencies' EXPOSE_API returns, laid out as in its
// utils.hpp (0.3.0). Its headers are not used: they include the old
// QuestLoader modloader header, which this scotland2 build does not have.
struct ExposedTypeInfo {
  std::string name;
  size_t size;
};
struct ExposedApi {
  ExposedTypeInfo ret;
  std::vector<ExposedTypeInfo> params;
  void* func;
};

using IsInRenderFunction = bool (*)();

// Replay's IsInRender, found through the loader rather than by file path, and
// type checked the way conditional-dependencies does before using it.
IsInRenderFunction FindReplayIsInRender() {
  CModInfo info{.id = "replay", .version = nullptr, .version_long = 0};
  CModResult mod = modloader_get_mod(&info, MatchType_IdOnly);
  if (mod.handle == nullptr) return nullptr;
  auto* exported = reinterpret_cast<ExposedApi (*)()>(dlsym(mod.handle, "__IsInRender"));
  if (exported == nullptr) return nullptr;
  ExposedApi const api = exported();
  if (api.ret.name != "bool" || api.ret.size != sizeof(bool) || !api.params.empty()) return nullptr;
  return reinterpret_cast<IsInRenderFunction>(api.func);
}

bool ReplayModInstalled() {
  CModInfo info{.id = "replay", .version = nullptr, .version_long = 0};
  return modloader_get_mod(&info, MatchType_IdOnly).handle != nullptr;
}

// Whether the Replay mod is rendering a replay right now. The lookup is
// retried until it succeeds, since one made before Replay has loaded would
// otherwise stay empty for the session.
bool ReplayModIsRendering() {
  static std::atomic<IsInRenderFunction> isInRender{nullptr};
  auto function = isInRender.load();
  if (function == nullptr) {
    function = FindReplayIsInRender();
    if (function == nullptr) return false;
    isInRender.store(function);
  }
  return function();
}
}

// Single-view programs are what replay renderers draw, but the headset draws
// none of them: a player who left this on got black maps (#92, #93). So it
// only applies while the Replay mod is actually rendering (0.14.28).
bool GetReplayRenderMode() {
  return !kLiteBuild && gReplayRenderMode && ReplayModIsRendering();
}

// The setting itself, rendering or not: while it is on and Replay is
// installed, maps get their single-view conversion prepared ahead of a render.
bool GetWindowsBundleForMap(std::string const& levelPath) {
  if (kLiteBuild || levelPath.empty()) return false;
  auto const& doc = getConfig().config;
  if (!doc.IsObject()) return false;
  auto it = doc.FindMember(kWindowsBundleMapsConfigKey);
  if (it == doc.MemberEnd() || !it->value.IsArray()) return false;
  std::string const key = MapKeyOf(levelPath);
  for (auto const& entry : it->value.GetArray()) {
    if (entry.IsString() && key == entry.GetString()) return true;
  }
  return false;
}

void SetWindowsBundleForMap(std::string const& levelPath, bool enabled) {
  if (kLiteBuild || levelPath.empty()) return;
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(kWindowsBundleMapsConfigKey);
  if (it == doc.MemberEnd() || !it->value.IsArray()) {
    if (it != doc.MemberEnd()) doc.RemoveMember(it);
    doc.AddMember(rapidjson::Value(kWindowsBundleMapsConfigKey, allocator), rapidjson::Value(rapidjson::kArrayType),
                  allocator);
    it = doc.FindMember(kWindowsBundleMapsConfigKey);
  }
  auto& list = it->value;
  std::string const key = MapKeyOf(levelPath);
  for (auto entry = list.Begin(); entry != list.End();) {
    entry = entry->IsString() && key == entry->GetString() ? list.Erase(entry) : entry + 1;
  }
  if (enabled) list.PushBack(rapidjson::Value(key.c_str(), allocator), allocator);
  config.Write();
}

bool GetReplayRenderModeSetting() {
  return !kLiteBuild && gReplayRenderMode;
}

bool GetReplayModInstalled() {
  return ReplayModInstalled();
}

bool GetBlitScreenTextureFix() {
  return !kLiteBuild && gBlitFix;
}

bool GetPrepareShadersBeforePlaying() {
  return !kLiteBuild && gPrepareShaders;
}

bool GetDrawGeometryShaderEffects() {
  return gGeometryEffects;
}

bool GetUsePcShadersForEmptyShaders() {
  return gPcShadersForEmpty;
}

bool GetCrashGuard() {
  return gCrashGuard;
}

bool GetUsePcBundleForMissingShaders() {
  return !kLiteBuild && gPcBundleForMissingShaders;
}

// Behind each post-0.14.7 getter below, the lite build answers with 0.14.7's
// behaviour whatever the saved value says, so a lite install never reads a
// setting it does not show.
bool GetOffscreenCulling() {
  // Off while the field of view is widened: it would freeze what the wider
  // view now shows (0.15.12).
  return !kLiteBuild && gOffscreenCulling && gFieldOfView <= 100;
}

int GetVivifyFoveationLevel() {
  return gFoveationLevel;
}

int GetFieldOfViewPercent() {
  return gFieldOfView;
}

int GetFarCullingDistance() {
  return kLiteBuild ? 0 : std::clamp(gFarCullingDistance, 0, 1000);
}

void ResetAutoEffectResolution() {
  gAutoEffect = AutoEffectState{};
  gAutoEffectPercent.store(100);
}

void ReportGameplayFrame(float seconds) {
  if (kLiteBuild || !gAutoEffectResolution) return;
  if (!(seconds > 0.0f) || seconds > 0.5f) return;  // a pause or a stall, not pacing
  auto& s = gAutoEffect;
  s.elapsed += seconds;
  s.average = s.average == 0.0 ? seconds : s.average * 0.95 + seconds * 0.05;
  if (s.elapsed < 2.0) return;  // let the average settle after the level starts
  if (s.best == 0.0 || s.average < s.best) s.best = s.average;
  int percent = gAutoEffectPercent.load();
  if (s.average > s.best * 1.35) {
    s.slowFor += seconds;
    s.fastFor = 0.0;
    if (s.slowFor >= 1.5 && percent > 40) {
      percent = std::max(40, percent - 10);
      gAutoEffectPercent.store(percent);
      s.slowFor = 0.0;
      PaperLogger.info("Vivify Auto Effect Resolution: frames at {:.1f} ms against a best of {:.1f} ms; map "
                       "effects down to {}%", s.average * 1000.0, s.best * 1000.0, percent);
    }
  } else if (s.average < s.best * 1.15) {
    s.fastFor += seconds;
    s.slowFor = 0.0;
    if (s.fastFor >= 6.0 && percent < 100) {
      percent = std::min(100, percent + 10);
      gAutoEffectPercent.store(percent);
      s.fastFor = 0.0;
      PaperLogger.info("Vivify Auto Effect Resolution: frames back to {:.1f} ms; map effects up to {}%",
                       s.average * 1000.0, percent);
    }
  } else {
    s.slowFor = 0.0;
    s.fastFor = 0.0;
  }
}

int GetEffectResolutionPercent() {
  if (kLiteBuild) return 100;
  int percent = std::clamp(gEffectResolution, 25, 100);
  if (gAutoEffectResolution) percent = std::min(percent, gAutoEffectPercent.load());
  // Crash recovery mode: half resolution or less (0.15.2).
  return GetRecoveryModeActive() ? std::min(percent, 50) : percent;
}

float GetEffectResolutionScale() {
  return static_cast<float>(GetEffectResolutionPercent()) / 100.0f;
}

std::string GetStandInShaderName() {
  return gStandInShaderName;
}

void EnsureConfigDefaults() {
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  bool needsWrite = false;

  needsWrite |= EnsureBoolConfigValue(kMultipassRenderingConfigKey, false, gMultipassRenderingEnabled);

  needsWrite |= EnsureBoolConfigValue(kVivifyDebugLoggingConfigKey, false, gVivifyDebugLogging);
  needsWrite |= EnsureBoolConfigValue(kDisableBeat0FilmgrainBlitConfigKey, false, gDisableBeat0FilmgrainBlit);
  needsWrite |= EnsureBoolConfigValue(kDisableAllBlitsConfigKey, false, gDisableAllBlits);
  needsWrite |= EnsureBoolConfigValue(kDisableCreateCameraDepthConfigKey, false, gDisableCreateCameraDepth);
  needsWrite |= EnsureBoolConfigValue(kDisableCustomNoteVisualsConfigKey, false, gDisableCustomNoteVisuals);
  needsWrite |= EnsureBoolConfigValue(kDisableVisualsInMultiplayerConfigKey, true, gDisableVisualsInMultiplayer);
  needsWrite |= EnsureBoolConfigValue(kDisableVRCenterAdjustConfigKey, false, gDisableVRCenterAdjust);
  needsWrite |= EnsureBoolConfigValue(kConvertPcBundlesOnDeviceConfigKey, true, gConvertPcBundlesOnDevice);
  needsWrite |= EnsureBoolConfigValue(kStandInShadingConfigKey, true, gStandInShading);
  needsWrite |= EnsureBoolConfigValue(kSceneDepthConfigKey, true, gSceneDepth);
  needsWrite |= EnsureBoolConfigValue(kSubmitScoresConfigKey, true, gSubmitScores);
  needsWrite |= EnsureBoolConfigValue(kTranslateShadersConfigKey, true, gTranslateShaders);
  needsWrite |= EnsureBoolConfigValue(kAudioLinkConfigKey, true, gAudioLink);
  needsWrite |= EnsureBoolConfigValue(kMapRealtimeShadowsConfigKey, false, gMapRealtimeShadows);
  needsWrite |= EnsureBoolConfigValue(kMapReflectionProbesConfigKey, true, gMapReflectionProbes);
  needsWrite |= EnsureBoolConfigValue(kMapSabersConfigKey, true, gMapSabers);
  needsWrite |= EnsureBoolConfigValue(kSaberLightsConfigKey, false, gSaberLights);
  needsWrite |= EnsureBoolConfigValue(kFlatScreenGuardConfigKey, true, gFlatScreenGuard);
  needsWrite |= EnsureBoolConfigValue(kFullReflectionProbesConfigKey, false, gFullReflectionProbes);
  needsWrite |= EnsureBoolConfigValue(kSingleViewMapCamerasConfigKey, true, gSingleViewMapCameras);
  needsWrite |= EnsureBoolConfigValue(kAutoEffectResolutionConfigKey, true, gAutoEffectResolution);
  needsWrite |= EnsureBoolConfigValue(kCrashRecoveryConfigKey, true, gCrashRecovery);
  needsWrite |= EnsureBoolConfigValue(kReplayRenderModeConfigKey, false, gReplayRenderMode);
  needsWrite |= EnsureBoolConfigValue(kBlitFixConfigKey, false, gBlitFix);
  needsWrite |= EnsureBoolConfigValue(kPrepareShadersConfigKey, false, gPrepareShaders);
  needsWrite |= EnsureBoolConfigValue(kGeometryEffectsConfigKey, false, gGeometryEffects);
  needsWrite |= EnsureBoolConfigValue(kGeometryBothEyesConfigKey, false, gGeometryBothEyes);
  needsWrite |= EnsureBoolConfigValue(kGeometryEyeCopyConfigKey, true, gGeometryEyeCopy);
  needsWrite |= EnsureBoolConfigValue(kPcShadersForEmptyConfigKey, true, gPcShadersForEmpty);
  needsWrite |= EnsureStringConfigValue(kStandInShaderNameConfigKey, std::string(),
                                        gStandInShaderName);
  needsWrite |= EnsureIntConfigValue(kEffectResolutionConfigKey, 100, 25, 100, gEffectResolution);
  needsWrite |= EnsureBoolConfigValue(kOffscreenCullingConfigKey, false, gOffscreenCulling);
  needsWrite |= EnsureBoolConfigValue(kCrashGuardConfigKey, true, gCrashGuard);
  needsWrite |= EnsureBoolConfigValue(kPcBundleForMissingShadersConfigKey, false, gPcBundleForMissingShaders);
  needsWrite |= EnsureIntConfigValue(kFarCullingConfigKey, 0, 0, 1000, gFarCullingDistance);
  needsWrite |= EnsureIntConfigValue(kFoveationConfigKey, 0, 0, 4, gFoveationLevel);
  needsWrite |= EnsureIntConfigValue(kFieldOfViewConfigKey, 100, 100, 150, gFieldOfView);
  if (needsWrite) {
    config.Write();
  }
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();
  try {
    InstallVivifyFileLogSink();
  } catch (...) {
  }
  // What the previous run left behind: a Vivify map that crashed, or froze and
  // was backed out of, goes on the crash recovery list (0.15.2).
  try {
    Vivify::Recovery::SetDirectory(std::string(kVivifyLogDir));
    std::string const recovery = Vivify::Recovery::ProcessPreviousRun(static_cast<int64_t>(std::time(nullptr)));
    if (!recovery.empty()) PaperLogger.warn("{}", recovery);
  } catch (...) {
  }
  // A config file another tool or a crash left unreadable must not take the
  // game down at launch (setup is noexcept): Vivify starts from its defaults.
  try {
    getConfig().Load();
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify: config could not be read ({}); using defaults", ex.what());
    getConfig().config.SetObject();
  } catch (...) {
    getConfig().config.SetObject();
  }
  try {
    EnsureConfigDefaults();
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify: config defaults could not be written ({})", ex.what());
  } catch (...) {
  }
  // Note: gMultipassRenderingEnabled / gVivifyDebugLogging are intentionally NOT
  // reset here. EnsureConfigDefaults() above already loaded the saved values (or
  // wrote the defaults on first run); hardcoding them back to a fixed value here
  // would silently discard the player's saved settings-menu choice on every launch.
  PaperLogger.info("Vivify file logging active -> {}", kVivifyLogPath);
}
namespace {
// Every other mod the loader started, in one line of the session log: the
// first thing to know when Vivify misbehaves alongside something else (a
// freeze in 0.14.22 turned out to be Adaptive Audio Latency and Custom
// Models, found by turning mods off one at a time).
void LogLoadedMods() {
  CModResults results = modloader_get_loaded();
  std::string list;
  for (size_t i = 0; i < results.size; i++) {
    auto const& info = results.array[i].info;
    if (info.id == nullptr) continue;
    if (!list.empty()) list += ", ";
    list += info.id;
    if (info.version != nullptr) list += std::string(" ") + info.version;
  }
  PaperLogger.info("Vivify: loaded mods ({}): {}", results.size, list.empty() ? "none reported" : list);
  modloader_free_results(&results);
}
}

MOD_EXTERN_FUNC void late_load() noexcept {
  // noexcept: anything escaping from here terminates the game at launch. A
  // failure in Vivify's own start-up costs Vivify, not the game and every
  // other mod with it.
  try {
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    RegisterModSettings();
    Vivify::LateLoad();
  } catch (std::exception const& ex) {
    PaperLogger.error("Vivify late_load failed ({}); Vivify is not active this session", ex.what());
  } catch (...) {
    PaperLogger.error("Vivify late_load failed with a non-std exception; Vivify is not active this session");
  }
  try {
    LogLoadedMods();
  } catch (...) {
  }
}
