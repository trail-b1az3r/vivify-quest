#include "main.hpp"
#include "VivifyGlobalLog.hpp"
#include <ctime>
#include "VivifyRuntime.hpp"
#include "VivifyReport.hpp"
#include <string>
#include <string_view>
#include <fstream>
#include <chrono>
#include <mutex>
#include <filesystem>
#include "HMUI/ViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "bsml/shared/BSML-Lite/Creation/Buttons.hpp"
#include "bsml/shared/BSML-Lite/Creation/Layout.hpp"
#include "bsml/shared/BSML-Lite/Creation/Settings.hpp"
#include "bsml/shared/BSML-Lite/Creation/Text.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "bsml/shared/BSML/Settings/BSMLSettings.hpp"
#include "custom-types/shared/register.hpp"
#include "scotland2/shared/modloader.h"

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

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
constexpr std::string_view kReplayRenderModeConfigKey = "replayRenderMode";
constexpr std::string_view kBlitFixConfigKey = "blitScreenTextureFix";
// Renamed in 0.14.18 so every install starts with it off: 0.14.15 saved it
// as on, and it may be what froze converted maps in the menu.
constexpr std::string_view kPrepareShadersConfigKey = "prepareShadersInMenu";
constexpr std::string_view kGeometryEffectsConfigKey = "drawGeometryShaderEffects";
constexpr std::string_view kPcShadersForEmptyConfigKey = "pcShadersForEmptyQuestShaders";
constexpr std::string_view kStandInShaderNameConfigKey = "standInShaderName";
constexpr std::string_view kEffectResolutionConfigKey = "effectResolutionPercent";
constexpr std::string_view kOffscreenCullingConfigKey = "offscreenCulling";
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
bool gReplayRenderMode = false;
bool gBlitFix = false;
bool gPrepareShaders = false;
bool gGeometryEffects = false;
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
bool gOffscreenCulling = true;
// Two loads of a converted bundle that never finish put it into the
// untranslated (grey) fallback; off, that never happens (0.14.24).
bool gCrashGuard = true;
// A Quest bundle missing shaders its PC build has is played from the converted
// PC build instead of being patched (0.14.24).
bool gPcBundleForMissingShaders = false;
// Metres beyond which the main camera draws nothing; 0 is off (0.14.23).
int gFarCullingDistance = 0;

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

void InstallVivifyFileLogSink() {
  if (gVivifyLogSinkInstalled) return;
  gVivifyLogSinkInstalled = true;
  std::error_code ec;
  std::filesystem::create_directories(std::string(kVivifyLogDir), ec);

  gVivifyLogFile.open(std::string(kVivifyLogPath), std::ios::out | std::ios::trunc);
  if (!gVivifyLogFile.is_open()) {
    PaperLogger.warn("Vivify: could not open log file at {} (logging to logcat only)", kVivifyLogPath);
    return;
  }
  gVivifyLogFile << "=== Vivify " << VERSION << " session log ===\n";
  gVivifyLogFile.flush();
  gVivifyLogLastFlush = std::chrono::steady_clock::now();
  gVivifyGlobalLog.Open("=== Vivify " + std::string(VERSION) + " session started " + WallClockText() + " ===");

  Paper::Logger::AddLogSink([](Paper::LogData const& data) {
    if (!data.tag.has_value() || *data.tag != std::string_view(MOD_ID)) return;
    std::lock_guard<std::mutex> lock(gVivifyLogMutex);
    bool const important = data.level >= Paper::LogLevel::WRN;
    if (gVivifyGlobalLog.IsOpen()) {
      gVivifyGlobalLog.Write(WallClockText() + " [" + std::string(Paper::format_as(data.level)) + "] " +
                             std::string(data.message));
      if (important) gVivifyGlobalLog.Flush();
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
    auto const now = std::chrono::steady_clock::now();
    if (important || now - gVivifyLogLastFlush > std::chrono::milliseconds(250)) {
      gVivifyLogFile.flush();
      gVivifyGlobalLog.Flush();
      gVivifyLogLastFlush = now;
    }
  });
}

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

void SetConvertStatusText(std::string const& text) {
  if (!gConvertStatusText) return;
  gConvertStatusText->set_text(StringW(text));
}

void RegisterModSettings() {
  BSML::BSMLSettings::get_instance()->TryAddSettingsMenu(
      [](HMUI::ViewController* viewController, bool firstActivation, bool, bool) {
        if (!firstActivation || viewController == nullptr) return;
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

        // Which build is actually running, in the headset, without a file.
        // "the new features do not work" and "the new build did not install"
        // look identical from the outside, and a version number here separates
        // them in one glance.
        //
        // Built as a StringW from a std::string, the way every other text in
        // this menu is: the std::u16string this used to assemble by hand is
        // what took the menu down.
        BSML::Lite::CreateText(container->get_transform(),
                               StringW(std::string("Vivify ") + VERSION));

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Debug logging", GetVivifyDebugLogging(),
            [](bool value) { SetBoolConfigValue(kVivifyDebugLoggingConfigKey, value, gVivifyDebugLogging); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Beat 0 Filmgrain Blit", GetDisableBeat0FilmgrainBlit(),
            [](bool value) { SetBoolConfigValue(kDisableBeat0FilmgrainBlitConfigKey, value, gDisableBeat0FilmgrainBlit); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable All Blits", GetDisableAllBlits(),
            [](bool value) { SetBoolConfigValue(kDisableAllBlitsConfigKey, value, gDisableAllBlits); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable CreateCamera Depth", GetDisableCreateCameraDepth(),
            [](bool value) { SetBoolConfigValue(kDisableCreateCameraDepthConfigKey, value, gDisableCreateCameraDepth); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Custom Note Visuals", GetDisableCustomNoteVisuals(),
            [](bool value) { SetBoolConfigValue(kDisableCustomNoteVisualsConfigKey, value, gDisableCustomNoteVisuals); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Vivify Visuals In Multiplayer", GetDisableVisualsInMultiplayer(),
            [](bool value) { SetBoolConfigValue(kDisableVisualsInMultiplayerConfigKey, value, gDisableVisualsInMultiplayer); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable VR Center Adjust Handling", GetDisableVRCenterAdjust(),
            [](bool value) { SetBoolConfigValue(kDisableVRCenterAdjustConfigKey, value, gDisableVRCenterAdjust); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Convert PC Bundles On Device",
            GetConvertPcBundlesOnDevice(),
            [](bool value) {
              SetBoolConfigValue(kConvertPcBundlesOnDeviceConfigKey, value, gConvertPcBundlesOnDevice);
            });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Stand-In Shading For Unsupported Shaders",
            GetStandInShading(),
            [](bool value) { SetBoolConfigValue(kStandInShadingConfigKey, value, gStandInShading); });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Scene Depth For Map Shaders",
            GetSceneDepthTexture(),
            [](bool value) { SetBoolConfigValue(kSceneDepthConfigKey, value, gSceneDepth); });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Translate Shaders On Conversion",
            GetTranslateShadersOnConversion(),
            [](bool value) { SetBoolConfigValue(kTranslateShadersConfigKey, value, gTranslateShaders); });

        // "PC Shaders For Broken Quest Shaders" is no longer optional (0.14.10):
        // empty Quest shaders always get their PC build.

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Map Realtime Shadows",
            GetMapRealtimeShadows(),
            [](bool value) { SetBoolConfigValue(kMapRealtimeShadowsConfigKey, value, gMapRealtimeShadows); });

        // Converted maps with single-screen programs only, for replay and
        // recording renderers. Kept in a cache of its own, so switching back
        // and forth converts each map only once per mode.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Replay Render Mode (converted maps)",
            GetReplayRenderMode(),
            [](bool value) { SetBoolConfigValue(kReplayRenderModeConfigKey, value, gReplayRenderMode); });

        // Experimental: a second, single-view build of a converted map's
        // shaders for blits into screen textures. Off until seen working.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Blit Fix For Screen Textures (experimental)",
            GetBlitScreenTextureFix(),
            [](bool value) { SetBoolConfigValue(kBlitFixConfigKey, value, gBlitFix); });

        // Compiles a converted map's shaders in the menu after selecting it,
        // a little each frame, instead of all at once when the song starts.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Prepare Shaders Before Playing",
            GetPrepareShadersBeforePlaying(),
            [](bool value) { SetBoolConfigValue(kPrepareShadersConfigKey, value, gPrepareShaders); });

        // Experimental: shaders with a geometry stage (wireframes, exploding
        // triangles) are drawn without it instead of as grey stand-ins.
        // Changing it reconverts the affected maps.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Draw Geometry-Shader Effects (experimental)",
            GetDrawGeometryShaderEffects(),
            [](bool value) { SetBoolConfigValue(kGeometryEffectsConfigKey, value, gGeometryEffects); });

        // Renders map post-process effects (blits, screen textures) below eye
        // resolution and scales the result up. Full-screen raymarchers cost
        // per pixel: 50% is a quarter of the work.
        BSML::Lite::CreateIncrementSetting(
            container->get_transform(), u"Effect Resolution % (lower = less lag)", 0, 25.0f,
            static_cast<float>(gEffectResolution), true, true, 25.0f, 100.0f, [](float value) {
              int const percent = std::clamp(static_cast<int>(value + 0.5f), 25, 100);
              SetIntConfigValue(kEffectResolutionConfigKey, percent, gEffectResolution);
            });

        // Plays the whole map from its converted PC build when the Quest
        // bundle shipped shaders empty, instead of patching those shaders in.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Use Only PC Bundle For Missing Shaders", gPcBundleForMissingShaders,
            [](bool value) {
              SetBoolConfigValue(kPcBundleForMissingShadersConfigKey, value, gPcBundleForMissingShaders);
            });

        // After two loads of a converted map that never finish, it is
        // reconverted without shader translation (grey stand-ins) so it can
        // be played. Off: maps always keep their translated shaders.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Crash Guard (grey fallback after 2 freezes)", gCrashGuard,
            [](bool value) { SetBoolConfigValue(kCrashGuardConfigKey, value, gCrashGuard); });

        // Animators in the map's prefabs stop updating while nothing they
        // draw is on screen; their animation resumes at the right time.
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Offscreen Culling (less lag)", gOffscreenCulling,
            [](bool value) { SetBoolConfigValue(kOffscreenCullingConfigKey, value, gOffscreenCulling); });

        // Draws nothing farther than this from the player. 0 is off: far
        // scenery is part of many maps' look.
        BSML::Lite::CreateIncrementSetting(
            container->get_transform(), u"Far Culling Distance m (0 = off)", 0, 50.0f,
            static_cast<float>(gFarCullingDistance), true, true, 0.0f, 1000.0f, [](float value) {
              int const metres = std::clamp(static_cast<int>(value + 0.5f), 0, 1000);
              SetIntConfigValue(kFarCullingConfigKey, metres, gFarCullingDistance);
            });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"AudioLink",
            GetAudioLinkEnabled(),
            [](bool value) { SetBoolConfigValue(kAudioLinkConfigKey, value, gAudioLink); });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Submit Scores On Vivify Maps",
            GetSubmitScoresOnVivifyMaps(),
            [](bool value) { SetBoolConfigValue(kSubmitScoresConfigKey, value, gSubmitScores); });

        // A map whose only asset bundle is a PC build has its play button
        // disabled, so it can never be selected into -- which also means the
        // per-level conversion that runs on level select can never fire for it.
        // This button converts every installed map in one pass instead, so
        // those levels become playable without having to be playable first.
        gConvertStatusText = BSML::Lite::CreateText(container->get_transform(), u"Idle");
        if (Vivify::IsBulkPcBundleConversionRunning()) SetConvertStatusText("Converting...");
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
                SetConvertStatusText(std::to_string(progress.levelsScanned) + "/" +
                                     std::to_string(progress.levelsTotal) + "  " + progress.status);
              },
              force);
        };

        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Convert All PC Bundles Now",
            []() { startConversion(false); });

        // A cached conversion is keyed on the source bundle, so it is reused
        // even after the converter itself has been fixed. This is the way to
        // pick those fixes up without deleting the cache directory by hand.
        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Force Reconvert All (ignore cache)",
            []() { startConversion(true); });

        // Puts Vivify back to a fresh install's state, without touching the
        // maps: every converted bundle and crash-guard note, the PC bundles it
        // downloaded into Quest maps' folders, and its settings. Two presses
        // within five seconds, since it cannot be undone.
        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Reset Vivify (clean slate)",
            []() {
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
            });

        // Lists every Vivify map in VivifyMaps.txt, then replaces each with a
        // fresh BeatSaver download, for maps whose folders were changed by an
        // earlier version. Two presses within five seconds.
        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Redownload All Vivify Maps",
            []() {
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
            });

        // paperlog output is not reachable without adb, so Vivify writes its own
        // plain-text report next to its data. Showing the path here means the
        // file can be found without being told where to look.
        BSML::Lite::CreateText(container->get_transform(),
                               u"Diagnostics (both plain .txt, same folder):");
        BSML::Lite::CreateText(
            container->get_transform(),
            StringW("<size=70%>" + Vivify::Report::FilePath() + "</size>"));
        BSML::Lite::CreateText(
            container->get_transform(),
            StringW("<size=70%>" + std::string(kVivifyLogPath) + "</size>"));
        BSML::Lite::CreateText(
            container->get_transform(),
            StringW("<size=70%>" + std::string(kVivifyLogDir) + "/vivify_global.txt (every session)</size>"));
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

bool GetMapRealtimeShadows() {
  return gMapRealtimeShadows;
}

bool GetReplayRenderMode() {
  return gReplayRenderMode;
}

bool GetBlitScreenTextureFix() {
  return gBlitFix;
}

bool GetPrepareShadersBeforePlaying() {
  return gPrepareShaders;
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
  return gPcBundleForMissingShaders;
}

bool GetOffscreenCulling() {
  return gOffscreenCulling;
}

int GetFarCullingDistance() {
  return std::clamp(gFarCullingDistance, 0, 1000);
}

int GetEffectResolutionPercent() {
  return std::clamp(gEffectResolution, 25, 100);
}

float GetEffectResolutionScale() {
  return static_cast<float>(std::clamp(gEffectResolution, 25, 100)) / 100.0f;
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
  needsWrite |= EnsureBoolConfigValue(kReplayRenderModeConfigKey, false, gReplayRenderMode);
  needsWrite |= EnsureBoolConfigValue(kBlitFixConfigKey, false, gBlitFix);
  needsWrite |= EnsureBoolConfigValue(kPrepareShadersConfigKey, false, gPrepareShaders);
  needsWrite |= EnsureBoolConfigValue(kGeometryEffectsConfigKey, false, gGeometryEffects);
  needsWrite |= EnsureBoolConfigValue(kPcShadersForEmptyConfigKey, true, gPcShadersForEmpty);
  needsWrite |= EnsureStringConfigValue(kStandInShaderNameConfigKey, std::string(),
                                        gStandInShaderName);
  needsWrite |= EnsureIntConfigValue(kEffectResolutionConfigKey, 100, 25, 100, gEffectResolution);
  needsWrite |= EnsureBoolConfigValue(kOffscreenCullingConfigKey, true, gOffscreenCulling);
  needsWrite |= EnsureBoolConfigValue(kCrashGuardConfigKey, true, gCrashGuard);
  needsWrite |= EnsureBoolConfigValue(kPcBundleForMissingShadersConfigKey, false, gPcBundleForMissingShaders);
  needsWrite |= EnsureIntConfigValue(kFarCullingConfigKey, 0, 0, 1000, gFarCullingDistance);
  if (needsWrite) {
    config.Write();
  }
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();
  InstallVivifyFileLogSink();
  getConfig().Load();
  EnsureConfigDefaults();
  // Note: gMultipassRenderingEnabled / gVivifyDebugLogging are intentionally NOT
  // reset here. EnsureConfigDefaults() above already loaded the saved values (or
  // wrote the defaults on first run); hardcoding them back to a fixed value here
  // would silently discard the player's saved settings-menu choice on every launch.
  PaperLogger.info("Vivify file logging active -> {}", kVivifyLogPath);
}
MOD_EXTERN_FUNC void late_load() noexcept {
  il2cpp_functions::Init();
  custom_types::Register::AutoRegister();
  RegisterModSettings();
  Vivify::LateLoad();
}
