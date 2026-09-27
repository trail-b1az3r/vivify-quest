#include "VivifyAudioLink.hpp"

#include "main.hpp"

#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "UnityEngine/Resources.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "UnityEngine/AssetBundle.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "UnityEngine/AudioSource.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/RenderTexture.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Texture.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Vector4.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

// The converted AudioLink bundle, linked into the library (VivifyAudioLinkBundle.cpp).
extern "C" unsigned char const vivify_audiolink_bundle_start[];
extern "C" unsigned char const vivify_audiolink_bundle_end[];

namespace Vivify::AudioLink {
namespace {

constexpr char const* kBundlePath = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/AudioLink.bundle";
constexpr char const* kMaterialAsset = "assets/com.llealloo.audiolink/runtime/materials/mat_audiolink.mat";
constexpr char const* kTextureAsset = "assets/com.llealloo.audiolink/runtime/rendertextures/rt_audiolink.asset";

// AudioLink.cs: AudioLinkVersionNumberMajor / Minor.
constexpr float kVersionMajor = 3.00f;
constexpr float kVersionMinor = 1.02f;

constexpr int kSamplesPerArray = 1023;
constexpr int kFrames = kSamplesPerArray * 4;
constexpr int kRightChannelTestDelay = 300;
constexpr int kGlobalStringMaxLength = 32;

// AudioLink.cs defaults (its serialized fields), which BSAudioLink keeps.
struct Settings {
  float gain = 1.0f;
  float bass = 1.0f;
  float treble = 1.0f;
  float x0 = 0.0f, x1 = 0.25f, x2 = 0.5f, x3 = 0.75f;
  float threshold0 = 0.45f, threshold1 = 0.45f, threshold2 = 0.45f, threshold3 = 0.45f;
  float fadeLength = 0.25f;
  float fadeExpFalloff = 0.75f;
  bool autogain = true;
  float autogainDerate = 0.1f;
};

struct PropertyIds {
  int audioTexture, fadeLength, fadeExpFalloff, gain, bass, treble, x0, x1, x2, x3;
  int threshold0, threshold1, threshold2, threshold3, autogain, autogainDerate;
  int sourceVolume, sourceSpatialBlend, sourcePosition;
  int themeColorMode, customThemeColor0, customThemeColor1, customThemeColor2, customThemeColor3;
  int stringLocalPlayer, stringMasterPlayer, stringCustom1, stringCustom2;
  int advancedTimeProps0, advancedTimeProps1, playerCountAndData, versionNumberAndFps;
  int mediaVolume, mediaTime, mediaPlaying, mediaLoop;
  std::array<int, 4> samplesL, samplesR;
};

struct State {
  bool attempted = false;     // tried to load (whether or not it worked)
  bool active = false;        // _AudioTexture is published
  UnityEngine::Material* material = nullptr;
  UnityEngine::RenderTexture* texture = nullptr;
  PropertyIds ids{};

  // Managed arrays reused every frame instead of allocating eight per frame.
  std::optional<SafePtr<Array<float>>> samples;
  std::optional<SafePtr<Array<UnityEngine::Vector4>>> stringVectors;
  std::array<float, kFrames> framesL{};
  std::array<float, kFrames> framesR{};
  std::optional<SafePtr<Array<float>>> framesArray;

  UnityEngine::AudioSource* source = nullptr;
  int lastSourceSearchFrame = -1000;
  GlobalNamespace::ColorScheme* appliedScheme = nullptr;

  double elapsedTime = 0.0;
  double elapsedTimeMSW = 0.0;
  int networkTimeMS = 0;
  double networkTimeMSAccumulatedError = 0.0;
  double fpsTime = 0.0;
  int fpsCount = 0;
  int rightChannelTestCounter = kRightChannelTestDelay;
  bool ignoreRightChannel = false;
};

State& S() {
  static State state;
  return state;
}

bool Alive(UnityEngine::Object* object) {
  return object != nullptr && object->m_CachedPtr.m_value != nullptr;
}

int Id(char const* name) {
  return UnityEngine::Shader::PropertyToID(StringW(name));
}

void InitIds(PropertyIds& ids) {
  ids.audioTexture = Id("_AudioTexture");
  ids.fadeLength = Id("_FadeLength");
  ids.fadeExpFalloff = Id("_FadeExpFalloff");
  ids.gain = Id("_Gain");
  ids.bass = Id("_Bass");
  ids.treble = Id("_Treble");
  ids.x0 = Id("_X0");
  ids.x1 = Id("_X1");
  ids.x2 = Id("_X2");
  ids.x3 = Id("_X3");
  ids.threshold0 = Id("_Threshold0");
  ids.threshold1 = Id("_Threshold1");
  ids.threshold2 = Id("_Threshold2");
  ids.threshold3 = Id("_Threshold3");
  ids.autogain = Id("_Autogain");
  ids.autogainDerate = Id("_AutogainDerate");
  ids.sourceVolume = Id("_SourceVolume");
  ids.sourceSpatialBlend = Id("_SourceSpatialBlend");
  ids.sourcePosition = Id("_SourcePosition");
  ids.themeColorMode = Id("_ThemeColorMode");
  ids.customThemeColor0 = Id("_CustomThemeColor0");
  ids.customThemeColor1 = Id("_CustomThemeColor1");
  ids.customThemeColor2 = Id("_CustomThemeColor2");
  ids.customThemeColor3 = Id("_CustomThemeColor3");
  ids.stringLocalPlayer = Id("_StringLocalPlayer");
  ids.stringMasterPlayer = Id("_StringMasterPlayer");
  ids.stringCustom1 = Id("_StringCustom1");
  ids.stringCustom2 = Id("_StringCustom2");
  ids.advancedTimeProps0 = Id("_AdvancedTimeProps0");
  ids.advancedTimeProps1 = Id("_AdvancedTimeProps1");
  ids.playerCountAndData = Id("_PlayerCountAndData");
  ids.versionNumberAndFps = Id("_VersionNumberAndFPSProperty");
  ids.mediaVolume = Id("_MediaVolume");
  ids.mediaTime = Id("_MediaTime");
  ids.mediaPlaying = Id("_MediaPlaying");
  ids.mediaLoop = Id("_MediaLoop");
  char const* left[] = {"_Samples0L", "_Samples1L", "_Samples2L", "_Samples3L"};
  char const* right[] = {"_Samples0R", "_Samples1R", "_Samples2R", "_Samples3R"};
  for (int i = 0; i < 4; i++) {
    ids.samplesL[static_cast<size_t>(i)] = Id(left[i]);
    ids.samplesR[static_cast<size_t>(i)] = Id(right[i]);
  }
}

// The embedded bundle is written out once (again only if it changed) and
// loaded from disk: this game build does not expose AssetBundle.LoadFromMemory.
bool WriteBundleFile() {
  size_t const size = static_cast<size_t>(vivify_audiolink_bundle_end - vivify_audiolink_bundle_start);
  if (size == 0) return false;
  std::error_code ec;
  if (std::filesystem::exists(kBundlePath, ec) && std::filesystem::file_size(kBundlePath, ec) == size && !ec) {
    std::ifstream existing(kBundlePath, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(existing)), {});
    if (bytes.size() == size && std::equal(bytes.begin(), bytes.end(), vivify_audiolink_bundle_start)) return true;
  }
  std::filesystem::create_directories(std::filesystem::path(kBundlePath).parent_path(), ec);
  std::ofstream out(kBundlePath, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<char const*>(vivify_audiolink_bundle_start), static_cast<std::streamsize>(size));
  return static_cast<bool>(out);
}

// AudioLink.cs UpdateSettings.
void ApplySettings(State& s) {
  Settings const settings;
  auto* m = s.material;
  m->SetFloat(s.ids.gain, settings.gain);
  m->SetFloat(s.ids.fadeLength, settings.fadeLength);
  m->SetFloat(s.ids.fadeExpFalloff, settings.fadeExpFalloff);
  m->SetFloat(s.ids.bass, settings.bass);
  m->SetFloat(s.ids.treble, settings.treble);
  m->SetFloat(s.ids.x0, settings.x0);
  m->SetFloat(s.ids.x1, settings.x1);
  m->SetFloat(s.ids.x2, settings.x2);
  m->SetFloat(s.ids.x3, settings.x3);
  m->SetFloat(s.ids.threshold0, settings.threshold0);
  m->SetFloat(s.ids.threshold1, settings.threshold1);
  m->SetFloat(s.ids.threshold2, settings.threshold2);
  m->SetFloat(s.ids.threshold3, settings.threshold3);
  m->SetFloat(s.ids.autogain, settings.autogain ? 1.0f : 0.0f);
  m->SetFloat(s.ids.autogainDerate, settings.autogainDerate);
}

// AudioLink.cs UpdateThemeColors, fed the way BSAudioLink's SetColorScheme
// does: the scheme's environment colours, and their boost colours when it has
// them. Before any scheme is known AudioLink's own defaults stand.
void ApplyThemeColors(State& s, GlobalNamespace::ColorScheme* scheme) {
  auto* m = s.material;
  UnityEngine::Color c0{1.0f, 1.0f, 0.0f, 1.0f}, c1{0.0f, 0.0f, 1.0f, 1.0f};
  UnityEngine::Color c2{1.0f, 0.0f, 0.0f, 1.0f}, c3{0.0f, 1.0f, 0.0f, 1.0f};
  int mode = 0;
  if (scheme != nullptr) {
    mode = 1;
    c0 = scheme->get_environmentColor0();
    c1 = scheme->get_environmentColor1();
    bool const boost = scheme->get_supportsEnvironmentColorBoost();
    c2 = boost ? scheme->get_environmentColor0Boost() : c0;
    c3 = boost ? scheme->get_environmentColor1Boost() : c1;
  }
  m->SetInt(s.ids.themeColorMode, mode);
  m->SetColor(s.ids.customThemeColor0, c0);
  m->SetColor(s.ids.customThemeColor1, c1);
  m->SetColor(s.ids.customThemeColor2, c2);
  m->SetColor(s.ids.customThemeColor3, c3);
}

// AudioLink.cs UpdateGlobalString: up to 32 code points, 24 bits each, packed
// as denormal floats into eight float4s.
void ApplyGlobalString(State& s, int id, std::u32string const& text) {
  ArrayW<UnityEngine::Vector4> vectors(static_cast<Array<UnityEngine::Vector4>*>(s.stringVectors->ptr()));
  for (size_t i = 0; i < vectors.size(); i++) vectors[i] = UnityEngine::Vector4{0, 0, 0, 0};
  auto pack = [](char32_t codePoint) {
    uint32_t const fraction = static_cast<uint32_t>(codePoint) & 0x007FFFFFu;
    return (static_cast<float>(fraction) / 8388608.0f) * 1.1754944e-38f;
  };
  size_t const length = std::min<size_t>(text.size(), kGlobalStringMaxLength);
  for (size_t j = 0; j < length; j++) {
    auto& v = vectors[j / 4];
    float const packed = pack(text[j]);
    switch (j % 4) {
      case 0: v.x = packed; break;
      case 1: v.y = packed; break;
      case 2: v.z = packed; break;
      default: v.w = packed; break;
    }
  }
  s.material->SetVectorArray(id, vectors);
}

bool Load(State& s) {
  s.attempted = true;
  if (!WriteBundleFile()) {
    PaperLogger.warn("Vivify AudioLink: could not write the AudioLink bundle to '{}'", kBundlePath);
    return false;
  }
  auto bundle = UnityEngine::AssetBundle::LoadFromFile(StringW(kBundlePath));
  if (!Alive(bundle.unsafePtr())) {
    PaperLogger.warn("Vivify AudioLink: the AudioLink bundle did not load");
    return false;
  }
  auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(bundle->LoadAsset(StringW(kMaterialAsset)).unsafePtr())
                       .value_or(nullptr);
  auto* texture =
      il2cpp_utils::try_cast<UnityEngine::RenderTexture>(bundle->LoadAsset(StringW(kTextureAsset)).unsafePtr())
          .value_or(nullptr);
  bundle->Unload(false);
  if (!Alive(material) || !Alive(texture)) {
    PaperLogger.warn("Vivify AudioLink: the bundle has no AudioLink material or texture");
    return false;
  }
  // Nothing managed references these once the bundle is gone, and a scene
  // change's Resources.UnloadUnusedAssets would otherwise take them.
  material->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
  texture->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
  auto* shader = material->get_shader().unsafePtr();
  if (!Alive(shader) || !shader->get_isSupported()) {
    PaperLogger.warn("Vivify AudioLink: this GPU rejects the AudioLink shader; AudioLink stays off");
    return false;
  }
  s.material = material;
  s.texture = texture;
  InitIds(s.ids);
  s.samples.emplace(static_cast<Array<float>*>(ArrayW<float>(static_cast<il2cpp_array_size_t>(kSamplesPerArray))));
  s.stringVectors.emplace(static_cast<Array<UnityEngine::Vector4>*>(
      ArrayW<UnityEngine::Vector4>(static_cast<il2cpp_array_size_t>(kGlobalStringMaxLength / 4))));
  s.framesArray.emplace(static_cast<Array<float>*>(ArrayW<float>(static_cast<il2cpp_array_size_t>(kFrames))));
  ApplySettings(s);
  ApplyThemeColors(s, nullptr);
  ApplyGlobalString(s, s.ids.stringLocalPlayer, U"");
  ApplyGlobalString(s, s.ids.stringMasterPlayer, U"");
  ApplyGlobalString(s, s.ids.stringCustom1, U"");
  ApplyGlobalString(s, s.ids.stringCustom2, U"");
  // AudioLink.cs SetAudioLinkGlobalTexture. The texture's update mode is
  // Realtime in the bundle itself: this build strips CustomRenderTexture's
  // scripting API, so it cannot be switched from here the way AudioLink.cs does.
  UnityEngine::Shader::SetGlobalTexture(s.ids.audioTexture, static_cast<UnityEngine::Texture*>(texture));
  s.active = true;
  PaperLogger.info("Vivify AudioLink: running (AudioLink 3.1.2 shader), publishing _AudioTexture");
  return true;
}

void Unpublish(State& s) {
  if (!s.active) return;
  UnityEngine::Shader::SetGlobalTexture(s.ids.audioTexture, static_cast<UnityEngine::Texture*>(nullptr));
  s.active = false;
  PaperLogger.info("Vivify AudioLink: turned off");
}

// The song while playing a level, the menu's preview otherwise -- what
// BSAudioLink's GameProvider and MenuProvider hand AudioLink.
void FindSource(State& s) {
  int const frame = UnityEngine::Time::get_frameCount();
  if (Alive(s.source) && s.source->get_isPlaying()) return;
  if (frame - s.lastSourceSearchFrame < 30) return;
  s.lastSourceSearchFrame = frame;
  s.source = nullptr;
  GlobalNamespace::ColorScheme* scheme = nullptr;
  auto* sync = UnityEngine::Object::FindObjectOfType<GlobalNamespace::AudioTimeSyncController*>();
  if (Alive(sync) && Alive(sync->____audioSource.unsafePtr())) {
    s.source = sync->____audioSource.unsafePtr();
    // The level's colour scheme (overrides and map colours already applied),
    // from the installer that set the gameplay scene up. ColorManager itself
    // is a plain Zenject-bound class that FindObjectOfType cannot see.
    auto installers = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::GameplayCoreInstaller*>();
    for (auto* installer : installers) {
      if (!Alive(installer) || installer->____sceneSetupData == nullptr) continue;
      scheme = installer->____sceneSetupData->colorScheme;
      if (scheme != nullptr) break;
    }
  } else {
    auto* preview = UnityEngine::Object::FindObjectOfType<GlobalNamespace::SongPreviewPlayer*>();
    if (Alive(preview) && preview->____audioSourceControllers) {
      auto controllers = preview->____audioSourceControllers;
      int const channel = preview->____activeChannel;
      if (channel >= 0 && static_cast<size_t>(channel) < controllers.size() && controllers[channel] != nullptr) {
        auto* audio = controllers[channel]->audioSource.unsafePtr();
        if (Alive(audio)) s.source = audio;
      }
    }
  }
  if (scheme != s.appliedScheme) {
    s.appliedScheme = scheme;
    ApplyThemeColors(s, scheme);
  }
}

// AudioLink.cs FPSUpdate.
void FpsUpdate(State& s) {
  s.material->SetVector(s.ids.versionNumberAndFps,
                        UnityEngine::Vector4{3.02f, kVersionMajor, static_cast<float>(s.fpsCount), kVersionMinor});
  s.material->SetVector(s.ids.playerCountAndData, UnityEngine::Vector4{0, 0, 0, 0});
  s.fpsCount = 0;
  s.fpsTime++;
  constexpr double kElapsedTimeMSWBoundary = 1024;
  if (s.elapsedTime >= kElapsedTimeMSWBoundary) {
    s.fpsTime = 0;
    s.elapsedTime -= kElapsedTimeMSWBoundary;
    s.elapsedTimeMSW++;
  }
  int const now = static_cast<int>(UnityEngine::Time::get_time() * 1000.0f);
  int const delta = now - s.networkTimeMS;
  if (delta > 3000 || delta < -3000) {
    s.networkTimeMS = now;
  } else {
    s.networkTimeMS += delta / 20;
  }
}

// AudioLink.cs SendAudioOutputData, with its right-channel test for sources
// whose right channel stays silent.
void SendAudio(State& s) {
  auto* source = s.source;
  ArrayW<float> frames(static_cast<Array<float>*>(s.framesArray->ptr()));
  source->GetOutputData(frames, 0);
  std::copy(frames.begin(), frames.end(), s.framesL.begin());
  if (s.rightChannelTestCounter > 0) {
    if (s.ignoreRightChannel) {
      s.framesR = s.framesL;
    } else {
      source->GetOutputData(frames, 1);
      std::copy(frames.begin(), frames.end(), s.framesR.begin());
    }
    s.rightChannelTestCounter--;
  } else {
    s.rightChannelTestCounter = kRightChannelTestDelay;
    frames[0] = 0.0f;
    source->GetOutputData(frames, 1);
    std::copy(frames.begin(), frames.end(), s.framesR.begin());
    s.ignoreRightChannel = s.framesR[0] == 0.0f;
    if (s.ignoreRightChannel) s.framesR = s.framesL;
  }
  ArrayW<float> samples(static_cast<Array<float>*>(s.samples->ptr()));
  for (int i = 0; i < 4; i++) {
    auto const offset = static_cast<size_t>(i * kSamplesPerArray);
    std::copy_n(s.framesL.begin() + offset, kSamplesPerArray, samples.begin());
    s.material->SetFloatArray(s.ids.samplesL[static_cast<size_t>(i)], samples);
    std::copy_n(s.framesR.begin() + offset, kSamplesPerArray, samples.begin());
    s.material->SetFloatArray(s.ids.samplesR[static_cast<size_t>(i)], samples);
  }
}

// AudioLink.cs Update.
void Update(State& s) {
  double const delta = UnityEngine::Time::get_deltaTime();
  s.elapsedTime += delta;
  {
    double const deltaMS = delta * 1000.0;
    int advance = static_cast<int>(deltaMS);
    s.networkTimeMSAccumulatedError += deltaMS - advance;
    if (s.networkTimeMSAccumulatedError > 1) {
      s.networkTimeMSAccumulatedError--;
      advance++;
    }
    s.networkTimeMS += advance;
  }
  s.fpsCount++;
  if (s.elapsedTime >= s.fpsTime) FpsUpdate(s);

  auto const now = std::chrono::system_clock::now();
  std::time_t const wall = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&wall, &local);
  double const subSecond =
      std::chrono::duration<double>(now.time_since_epoch()).count() - static_cast<double>(wall);
  double const localSeconds = local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec + subSecond;
  s.material->SetVector(s.ids.advancedTimeProps0,
                        UnityEngine::Vector4{static_cast<float>(s.elapsedTime), static_cast<float>(s.elapsedTimeMSW),
                                             static_cast<float>(localSeconds), 0.0f});
  double const utcSecondsUnix = std::chrono::duration<double>(now.time_since_epoch()).count();
  s.material->SetVector(s.ids.advancedTimeProps1,
                        UnityEngine::Vector4{static_cast<float>(s.networkTimeMS & 65535),
                                             static_cast<float>(s.networkTimeMS >> 16),
                                             static_cast<float>(std::floor(utcSecondsUnix / 86400.0)),
                                             static_cast<float>(std::fmod(utcSecondsUnix, 86400.0))});

  FindSource(s);
  if (!Alive(s.source)) return;
  SendAudio(s);
  // Beat Saber's music sources are 2D: no spatial falloff to evaluate, so the
  // volume goes through unchanged and the spatial blend is zero.
  float const volume = s.source->get_volume();
  s.material->SetFloat(s.ids.sourceVolume, volume);
  s.material->SetFloat(s.ids.sourceSpatialBlend, 0.0f);
  auto const position = s.source->get_transform()->get_position();
  s.material->SetVector(s.ids.sourcePosition, UnityEngine::Vector4{position.x, position.y, position.z, 0.0f});
  // AudioLink.cs autoSetMediaState.
  s.material->SetFloat(s.ids.mediaVolume, volume);
  float time = 0.0f;
  auto* clip = s.source->get_clip().unsafePtr();
  if (Alive(clip) && clip->get_length() > 0.0f) time = s.source->get_time() / clip->get_length();
  s.material->SetFloat(s.ids.mediaTime, time);
  s.material->SetFloat(s.ids.mediaPlaying, s.source->get_isPlaying() ? 1.0f : 3.0f);  // Playing / Stopped
  s.material->SetFloat(s.ids.mediaLoop, s.source->get_loop() ? 1.0f : 0.0f);
}

}  // namespace

bool IsActive() {
  return S().active;
}

void Tick() {
  auto& s = S();
  if (!GetAudioLinkEnabled()) {
    Unpublish(s);
    return;
  }
  if (!s.active) {
    if (s.attempted && Alive(s.material) && Alive(s.texture)) {
      // Turned back on in the settings.
      UnityEngine::Shader::SetGlobalTexture(s.ids.audioTexture, static_cast<UnityEngine::Texture*>(s.texture));
      s.active = true;
    } else if (!s.attempted) {
      if (!Load(s)) return;
    } else {
      return;
    }
  }
  if (!Alive(s.material) || !Alive(s.texture)) {
    s.active = false;
    return;
  }
  Update(s);
}

}  // namespace Vivify::AudioLink
