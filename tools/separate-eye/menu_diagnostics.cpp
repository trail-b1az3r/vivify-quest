// Opt-in diagnostic build only. File commands select/load bundles IN THE MENU.
// Gameplay actions additionally require VIVIFY_GAMEPLAY_DIAGNOSTICS, explicitly
// authorized for controlled regression runs and excluded from releases.
#include "VivifyRuntimeInternal.hpp"
#include "VivifySeparateEye.hpp"
#include "songcore/shared/SongCore.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "VivifyProjectors.hpp"
#include "VivifyDepthCopy.hpp"
#include "VivifyCameraBindings.hpp"
#include "VivifyComponents.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/stringbuffer.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/writer.h"
#include <iterator>
#include <bit>
#include "System/GC.hpp"
#ifdef VIVIFY_GAMEPLAY_DIAGNOSTICS
#include "playback_diagnostics.hpp"
#endif

namespace Vivify {
struct MenuDiagnostics {
  // Diagnostic IPC stays in app-private storage. ADB-created files on the
  // emulated shared volume can be unreadable from the app's scoped namespace.
  static constexpr char const* directory = "/data/user/0/com.beatgames.beatsaber/files/vivify-menu-diagnostics";
  inline static std::string id, folder, message;
  inline static float lastPoll = -1;
  inline static bool active = false;
  inline static bool nativeWarmupQueued = false;
  inline static bool donorWarmupQueued = false;
  inline static bool fixedGuardProbeChecked = false, fixedGuardProbePassed = false;
  inline static bool cameraBindingsChecked = false, cameraBindingsPassed = false;
  inline static bool nestedCameraBindingsPassed = false;
  inline static bool secondaryCameraRefreshChecked = false, secondaryCameraRefreshPassed = false;
  inline static std::string secondaryCameraRefreshError;
  inline static bool blitMaterialRefreshChecked = false, blitMaterialRefreshPassed = false;
  inline static std::string blitMaterialRefreshError;
  inline static bool textureOwnershipChecked = false, textureOwnershipPassed = false;
  inline static std::string textureOwnershipError;
  inline static bool lifetimeRequested = false, lifetimeDone = false;
  inline static SafePtr<UnityEngine::AsyncOperation> lifetimeCollection;
  inline static std::vector<SafePtr<UnityEngine::Object>> lifetimeAssets;
  inline static size_t lifetimeLost = 0;
  inline static size_t donorIndex = 0;
  inline static int donorPassAttempts = 0, donorPassFailures = 0;
  inline static int expectedDonorNonDraw = 0;
  inline static std::vector<SafePtr<UnityEngine::Material>> donorMaterials;
  inline static rapidjson::Document passTypes;

  static bool ExpectedNonDraw(std::string name, int pass, int passCount) {
    if (pass < 0 || pass >= passCount || !passTypes.IsObject() || !passTypes.HasMember(folder.c_str())) return false;
    if (name.starts_with("__Vivify")) { auto slash = name.find('/'); if (slash != std::string::npos) name.erase(0, slash + 1); }
    auto const& map = passTypes[folder.c_str()];
    if (!map.IsObject() || !map.HasMember(name.c_str()) || !map[name.c_str()].IsArray()) return false;
    bool matched = false;
    for (auto const& layout : map[name.c_str()].GetArray()) {
      if (!layout.IsArray() || layout.Size() != static_cast<unsigned>(passCount)) continue;
      matched = true;
      // SerializedPass type 2 is GrabPass. Require agreement across every
      // possible source subshader of this size; ambiguous metadata stays a
      // genuine unresolved diagnostic, never an assumed success.
      if (!layout[pass].IsInt() || layout[pass].GetInt() != 2) return false;
    }
    return matched;
  }

  static void ClearDonorWarmup() {
    lifetimeRequested = lifetimeDone = false; lifetimeCollection = nullptr; lifetimeAssets.clear(); lifetimeLost = 0;
    fixedGuardProbeChecked = fixedGuardProbePassed = false;
    blitMaterialRefreshChecked = blitMaterialRefreshPassed = false; blitMaterialRefreshError.clear();
    textureOwnershipChecked = textureOwnershipPassed = false; textureOwnershipError.clear();
    for (auto& material : donorMaterials) if (material && IsManagedAlive(material.ptr())) UnityEngine::Object::Destroy(material.ptr());
    donorMaterials.clear(); donorWarmupQueued = false; donorIndex = 0; donorPassAttempts = donorPassFailures = expectedDonorNonDraw = 0;
  }
  static void WarmDonors() {
    if (!active || !message.empty()) return;
    if (!donorWarmupQueued && SeparateEye::PreparationState() == "ready") {
      for (auto* shader : SeparateEye::PreparedShadersForDiagnostics()) donorMaterials.emplace_back(UnityEngine::Material::New_ctor(shader));
      donorWarmupQueued = true;
    }
    for (int budget = 0; budget < 2 && donorIndex < donorMaterials.size(); ++budget) {
      auto* material = donorMaterials[donorIndex++].ptr();
      if (!IsManagedAlive(material)) continue;
      for (int pass = 0; pass < material->get_passCount(); ++pass) {
        ++donorPassAttempts;
        try {
          if (!material->SetPass(pass)) {
            ++donorPassFailures;
            std::string name(material->get_shader()->get_name());
            if (ExpectedNonDraw(name, pass, material->get_passCount())) ++expectedDonorNonDraw;
            else PaperLogger.warn("Vivify MENU DIAGNOSTICS: drawable/unknown donor '{}' pass {} did not bind", name, pass);
          }
        } catch (...) { ++donorPassFailures; }
      }
    }
  }

  static void WarmNative(Runtime& runtime) {
    if (nativeWarmupQueued || !active || !IsManagedAlive(runtime._mainBundle)) return;
    nativeWarmupQueued = true;
    // Normal menu warmup is limited to converted PC bundles. Exercise the
    // installed Quest materials too for this diagnostic run, without drawing.
    if (!runtime._companionPcBundlePath.empty()) return;
    std::unordered_set<UnityEngine::Material*> seen;
    auto add = [&](UnityEngine::Material* material) {
      if (IsManagedAlive(material) && seen.insert(material).second) runtime._warmupQueue.push_back(material);
    };
    for (auto const& [_, asset] : runtime._assets) {
      if (!IsManagedAlive(asset)) continue;
      if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr)) add(material);
      if (auto* object = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr)) {
        for (auto* renderer : object->GetComponentsInChildren<UnityEngine::Renderer*>(true))
          if (IsManagedAlive(renderer)) for (auto material : renderer->get_sharedMaterials()) add(material.unsafePtr());
        for (auto* projector : Projectors::InChildren(object))
          if (IsManagedAlive(projector)) add(Projectors::GetMaterial(projector));
      }
    }
    if (runtime._warmupQueue.empty()) return;
    runtime._warmupIndex = 0; runtime._warmupLevel = runtime._selectedLevelPath;
    runtime._warmupPassAttempts = runtime._warmupPassFailures = 0;
    runtime._warmupUnboundPasses.clear();
    runtime._warmupBundle = runtime._preloadedBundlePath;
    runtime._warmupStarted = UnityEngine::Time::get_realtimeSinceStartup();
    SongCore::API::PlayButton::DisablePlayButton("Vivify", "Preparing Quest shaders for menu diagnostics...");
    PaperLogger.info("Vivify MENU DIAGNOSTICS: preparing {} Quest material(s), no draw/play", runtime._warmupQueue.size());
  }

  static void CheckFixedGuard(Runtime& runtime) {
    if (fixedGuardProbeChecked || runtime._fixedFeatureMaterials.empty()) return;
    auto* source = runtime._fixedFeatureMaterials.begin()->second.material.ptr();
    if (!IsManagedAlive(source)) return;
    fixedGuardProbeChecked = true;
    SafePtr<UnityEngine::Material> copy;
    try {
      copy = UnityEngine::Material::New_ctor(source);
      bool initial = Runtime::FixedMaterialFeaturesMatch(copy.ptr());
      copy->SetFloat(u"_EmissionEnable", 1.0f);
      bool rejected = !Runtime::FixedMaterialFeaturesMatch(copy.ptr());
      copy->SetFloat(u"_EmissionEnable", 0.0f);
      fixedGuardProbePassed = initial && rejected && Runtime::FixedMaterialFeaturesMatch(copy.ptr()) && Runtime::FixedMaterialFeaturesMatch(source);
    } catch (...) { fixedGuardProbePassed = false; }
    if (copy && IsManagedAlive(copy.ptr())) UnityEngine::Object::Destroy(copy.ptr());
    PaperLogger.info("Vivify MENU DIAGNOSTICS: private-copy material change rejection passed={}; source material untouched", fixedGuardProbePassed);
    if (!fixedGuardProbePassed) message = "Fixed material guard self-check failed";
  }

  static void CheckSecondaryCameraRefresh(Runtime& runtime) {
    // This fixture never renders, activates its GameObject, or changes the
    // real main camera. It catches changes the old four-field gate missed.
    if (secondaryCameraRefreshChecked || runtime.GetCurrentBeatmapData() != nullptr) return;
    auto* main = UnityEngine::Camera::get_main().unsafePtr();
    if (!IsManagedAlive(main)) return;
    secondaryCameraRefreshChecked = true;
    struct Fixture {
      SafePtr<UnityEngine::GameObject> object;
      SafePtr<SecondaryCameraController> controller;
      SafePtr<UnityEngine::RenderTexture> target;
      bool Close() noexcept {
        bool ok = true;
        auto attempt = [&](auto action) noexcept { try { action(); } catch (...) { ok = false; } };
        attempt([&] { if (controller && IsManagedAlive(controller.ptr())) controller->OnPostRender(); });
        controller = nullptr;
        attempt([&] {
          if (object && IsManagedAlive(object.ptr())) {
            object->SetActive(false);
            UnityEngine::Object::Destroy(object.ptr());
          }
        });
        object = nullptr;
        attempt([&] {
          if (target && IsManagedAlive(target.ptr())) {
            if (target->IsCreated()) target->Release();
            UnityEngine::Object::Destroy(target.ptr());
          }
        });
        target = nullptr;
        return ok;
      }
      ~Fixture() noexcept { Close(); }
    };
    try {
      CameraBindings bindings;
      bindings.Capture(main);
      Fixture fixture; // destroyed before bindings on every exception path
      int const mask = main->get_cullingMask();
      float const fov = main->get_fieldOfView(), nearClip = main->get_nearClipPlane(), farClip = main->get_farClipPlane();
      float const aspect = main->get_aspect(), depth = main->get_depth();
      auto const eye = main->get_stereoTargetEye();
      auto* originalTarget = main->get_targetTexture().unsafePtr();
      std::vector<float> distances;
      for (float value : main->get_layerCullDistances()) distances.push_back(value);
      if (distances.empty()) throw std::runtime_error("main camera has no culling-distance array");

      fixture.object = UnityEngine::GameObject::New_ctor(u"Vivify Menu Camera Refresh Check");
      if (!fixture.object || !IsManagedAlive(fixture.object.ptr())) throw std::runtime_error("auxiliary fixture allocation failed");
      fixture.object->SetActive(false);
      fixture.object->set_hideFlags(UnityEngine::HideFlags::HideAndDontSave);
      auto* camera = fixture.object->AddComponent<UnityEngine::Camera*>();
      if (!IsManagedAlive(camera)) throw std::runtime_error("auxiliary fixture camera allocation failed");
      camera->set_enabled(false);
      fixture.controller = fixture.object->AddComponent<SecondaryCameraController*>();
      fixture.target = UnityEngine::RenderTexture::New_ctor(4, 4, 0, UnityEngine::RenderTextureFormat::ARGB32);
      if (!fixture.controller || !IsManagedAlive(fixture.controller.ptr()) || !fixture.target || !IsManagedAlive(fixture.target.ptr()))
        throw std::runtime_error("auxiliary fixture controller/target allocation failed");
      // No Create/Render/Blit: the non-created target only tests that the
      // controller removes a stale targetTexture reference.
      camera->set_targetTexture(fixture.target.ptr());
      camera->set_cullingMask(mask); camera->set_fieldOfView(fov);
      camera->set_nearClipPlane(nearClip); camera->set_farClipPlane(farClip);
      camera->set_aspect(aspect + 0.25f); camera->set_depth(depth + 3.0f);
      camera->set_stereoTargetEye(eye == UnityEngine::StereoTargetEyeMask::None
                                    ? UnityEngine::StereoTargetEyeMask::Both : UnityEngine::StereoTargetEyeMask::None);
      auto stale = ArrayW<float>(static_cast<il2cpp_array_size_t>(distances.size()));
      for (size_t i = 0; i < distances.size(); ++i) stale[i] = distances[i] == 0.0f ? 1.0f : 0.0f;
      camera->set_layerCullDistances(stale);
      if (camera->get_cullingMask() != mask || camera->get_fieldOfView() != fov ||
          camera->get_nearClipPlane() != nearClip || camera->get_farClipPlane() != farClip)
        throw std::runtime_error("could not isolate auxiliary refresh from the old gate fields");

      fixture.controller->OnPreCull();
      auto matchesDistances = [&](UnityEngine::Camera* candidate) {
        auto values = candidate->get_layerCullDistances();
        if (values.size() != distances.size()) return false;
        for (size_t i = 0; i < distances.size(); ++i) if (values[i] != distances[i]) return false;
        return true;
      };
      bool const copied = camera->get_cullingMask() == mask && camera->get_fieldOfView() == fov &&
          camera->get_nearClipPlane() == nearClip && camera->get_farClipPlane() == farClip &&
          camera->get_aspect() == aspect && camera->get_depth() == depth - 1.0f &&
          camera->get_stereoTargetEye() == eye && camera->get_targetTexture().unsafePtr() == nullptr &&
          matchesDistances(camera);
      bool const mainUnchanged = main->get_cullingMask() == mask && main->get_fieldOfView() == fov &&
          main->get_nearClipPlane() == nearClip && main->get_farClipPlane() == farClip &&
          main->get_aspect() == aspect && main->get_depth() == depth && main->get_stereoTargetEye() == eye &&
          main->get_targetTexture().unsafePtr() == originalTarget && matchesDistances(main);
      bool const cleaned = fixture.Close();
      bool const restored = bindings.Restore();
      secondaryCameraRefreshPassed = copied && mainUnchanged && cleaned && restored;
      if (!secondaryCameraRefreshPassed) secondaryCameraRefreshError = "auxiliary properties, source preservation or cleanup did not match";
    } catch (std::exception const& ex) { secondaryCameraRefreshError = ex.what(); }
      catch (...) { secondaryCameraRefreshError = "native/managed exception during auxiliary-camera fixture"; }
    PaperLogger.info("Vivify MENU DIAGNOSTICS: auxiliary camera refresh={} (no render); {}",
                     secondaryCameraRefreshPassed, secondaryCameraRefreshError);
  }

  static void CheckBlitMaterialRefresh(Runtime& runtime) {
    if (blitMaterialRefreshChecked || runtime.GetCurrentBeatmapData() != nullptr || SeparateEye::IsRenderingEye() ||
        runtime._singleViewShaders.empty() || runtime._blitCompanionLevel != runtime._selectedLevelPath) return;
    try {
      UnityEngine::Material* asset = nullptr;
      UnityEngine::Shader* companion = nullptr;
      auto consider = [&](UnityEngine::Material* material) {
        if (asset || !IsManagedAlive(material) || !material->HasProperty(u"_MainTex")) return;
        auto* shader = material->get_shader().unsafePtr();
        if (!IsManagedAlive(shader)) return;
        auto found = runtime._singleViewShaders.find(ShaderNameForLog(shader));
        if (found != runtime._singleViewShaders.end() && IsManagedAlive(found->second)) {
          asset = material; companion = found->second;
        }
      };
      // Only inspect already loaded assets. All property writes below belong
      // to a disposable copy, never the selected map's own material.
      for (auto const& [_, loaded] : runtime._assets) {
        if (!IsManagedAlive(loaded)) continue;
        consider(il2cpp_utils::try_cast<UnityEngine::Material>(loaded).value_or(nullptr));
        if (auto* object = il2cpp_utils::try_cast<UnityEngine::GameObject>(loaded).value_or(nullptr)) {
          for (auto* renderer : object->GetComponentsInChildren<UnityEngine::Renderer*>(true))
            if (IsManagedAlive(renderer)) for (auto material : renderer->get_sharedMaterials()) consider(material.unsafePtr());
          for (auto* projector : Projectors::InChildren(object)) consider(Projectors::GetMaterial(projector));
        }
        if (asset) break;
      }
      if (!asset) return; // No eligible asset yet; a later menu status can retry.
      blitMaterialRefreshChecked = true;
      SafePtr<UnityEngine::Material> source;
      SafePtr<UnityEngine::RenderTexture> target;
      auto cleanup = [&]() noexcept {
        bool ok = true;
        auto attempt = [&](auto action) noexcept { try { action(); } catch (...) { ok = false; } };
        attempt([&] {
          // The source is a new private object, so this key can only belong
          // to this fixture. Never clear another material's helper entry.
          if (!source) return;
          auto found = runtime._singleViewBlitMaterials.find(source.ptr());
          if (found != runtime._singleViewBlitMaterials.end()) {
            auto* copy = found->second;
            runtime._singleViewBlitMaterials.erase(found);
            if (copy != source.ptr() && IsManagedAlive(copy)) UnityEngine::Object::Destroy(copy);
          }
        });
        attempt([&] {
          if (target && IsManagedAlive(target.ptr())) {
            if (target->IsCreated()) target->Release();
            UnityEngine::Object::Destroy(target.ptr());
          }
        });
        target = nullptr;
        attempt([&] { if (source && IsManagedAlive(source.ptr())) UnityEngine::Object::Destroy(source.ptr()); });
        source = nullptr;
        return ok;
      };
      struct Cleanup { decltype(cleanup)& close; ~Cleanup() noexcept { close(); } } guard{cleanup};
      auto* originalTexture = asset->GetTexture(u"_MainTex").unsafePtr();
      auto* originalShader = asset->get_shader().unsafePtr();
      source = UnityEngine::Material::New_ctor(asset);
      target = UnityEngine::RenderTexture::New_ctor(4, 4, 0, UnityEngine::RenderTextureFormat::ARGB32);
      if (!source || !IsManagedAlive(source.ptr()) || !target || !IsManagedAlive(target.ptr()))
        throw std::runtime_error("blit refresh fixture allocation failed");
      auto* black = UnityEngine::Texture2D::get_blackTexture().unsafePtr();
      auto* white = UnityEngine::Texture2D::get_whiteTexture().unsafePtr();
      if (!IsManagedAlive(black) || !IsManagedAlive(white) || black == white)
        throw std::runtime_error("blit refresh fixture textures unavailable");
      // No target Create/Blit/SetPass: exercise helper selection and its
      // cached material state without drawing or compiling a shader.
      source->SetTexture(u"_MainTex", black);
      auto* first = runtime.BlitMaterialFor(source.ptr(), target.ptr());
      bool const initial = IsManagedAlive(first) && first != source.ptr() &&
          first->get_shader().unsafePtr() == companion && first->GetTexture(u"_MainTex").unsafePtr() == black;
      source->SetTexture(u"_MainTex", white);
      auto* second = runtime.BlitMaterialFor(source.ptr(), target.ptr());
      bool const refreshed = initial && second == first && second->get_shader().unsafePtr() == companion &&
          second->GetTexture(u"_MainTex").unsafePtr() == white &&
          second->get_hideFlags() == UnityEngine::HideFlags::DontUnloadUnusedAsset;
      // Only perturb this fixture's private helper. The current companion
      // must be re-established even when a cached copy has another shader.
      if (IsManagedAlive(second)) second->set_shader(originalShader);
      auto* third = runtime.BlitMaterialFor(source.ptr(), target.ptr());
      bool const retargeted = refreshed && third == first && third->get_shader().unsafePtr() == companion &&
          third->GetTexture(u"_MainTex").unsafePtr() == white &&
          third->get_hideFlags() == UnityEngine::HideFlags::DontUnloadUnusedAsset;
      bool const assetUnchanged = asset->GetTexture(u"_MainTex").unsafePtr() == originalTexture &&
          asset->get_shader().unsafePtr() == originalShader;
      bool const cleaned = cleanup();
      blitMaterialRefreshPassed = retargeted && assetUnchanged && cleaned;
      if (!blitMaterialRefreshPassed) blitMaterialRefreshError = "helper identity/properties/shader, asset preservation or cleanup did not match";
    } catch (std::exception const& ex) { blitMaterialRefreshError = ex.what(); }
      catch (...) { blitMaterialRefreshError = "native/managed exception during blit-material fixture"; }
    PaperLogger.info("Vivify MENU DIAGNOSTICS: blit material refresh={} (no draw); {}",
                     blitMaterialRefreshPassed, blitMaterialRefreshError);
  }

  static void CheckTextureOwnership(Runtime& runtime) {
    if (textureOwnershipChecked || runtime.GetCurrentBeatmapData() || SeparateEye::IsRenderingEye()) return;
    textureOwnershipChecked = true;
    int const key = UnityEngine::Shader::PropertyToID(u"__VivifyTextureOwnershipProbe");
    SafePtr<UnityEngine::Texture> previous;
    SafePtr<UnityEngine::RenderTexture> oldTexture;
    SafePtr<UnityEngine::RenderTexture> replacement;
    DeclaredTextureData old;
    old.propertyId = key;
    bool captured = false;
    auto cleanup = [&]() noexcept {
      bool ok = true;
      auto attempt = [&](auto action) noexcept { try { action(); } catch (...) { ok = false; } };
      auto* pending = old.texture;
      attempt([&] { if (old.texture) runtime.ReleaseDeclaredTextureData(old); });
      if (!old.texture) oldTexture = nullptr;
      if (replacement.ptr() == pending && !old.texture) replacement = nullptr;
      attempt([&] { if (captured) UnityEngine::Shader::SetGlobalTexture(key,
          previous && IsManagedAlive(previous.ptr()) ? previous.ptr() : nullptr); });
      attempt([&] {
        if (replacement && IsManagedAlive(replacement.ptr())) {
          if (replacement->IsCreated()) replacement->Release();
          UnityEngine::Object::Destroy(replacement.ptr());
        }
      });
      replacement = nullptr;
      return ok;
    };
    struct Cleanup { decltype(cleanup)& close; ~Cleanup() noexcept { close(); } } guard{cleanup};
    try {
      previous = UnityEngine::Shader::GetGlobalTexture(key).unsafePtr(); captured = true;
      oldTexture = UnityEngine::RenderTexture::New_ctor(4, 4, 0, UnityEngine::RenderTextureFormat::ARGB32);
      old.texture = oldTexture.ptr();
      replacement = UnityEngine::RenderTexture::New_ctor(4, 4, 0, UnityEngine::RenderTextureFormat::ARGB32);
      if (!IsManagedAlive(old.texture) || !replacement || !IsManagedAlive(replacement.ptr()))
        throw std::runtime_error("texture ownership fixture allocation failed");
      // Unity returns no global binding for an uncreated RenderTexture.
      // Give these two private 4x4 fixtures storage, as real captures have.
      // No Render/Blit/SetPass and no map asset or camera is used.
      if (!oldTexture->Create() || !replacement->Create())
        throw std::runtime_error("texture ownership fixture storage creation failed");
      auto* control = UnityEngine::Texture2D::get_whiteTexture().unsafePtr();
      UnityEngine::Shader::SetGlobalTexture(key, control);
      bool const controlBound = UnityEngine::Shader::GetGlobalTexture(key).unsafePtr() == control;
      UnityEngine::Shader::SetGlobalTexture(key, replacement.ptr());
      auto* initialBinding = UnityEngine::Shader::GetGlobalTexture(key).unsafePtr();
      auto* expectedBinding = static_cast<void*>(replacement.ptr());
      bool const initiallyBound = initialBinding == replacement.ptr();
      bool const replacementCreated = replacement->IsCreated();
      runtime.ReleaseDeclaredTextureData(old);
      oldTexture = nullptr;
      bool const preserved = !old.texture && !old.allocation &&
          UnityEngine::Shader::GetGlobalTexture(key).unsafePtr() == replacement.ptr();
      runtime.ReleaseDeclaredTextureData(old);
      bool const repeated = UnityEngine::Shader::GetGlobalTexture(key).unsafePtr() == replacement.ptr();
      old.texture = replacement.ptr();
      runtime.ReleaseDeclaredTextureData(old); replacement = nullptr;
      bool const cleared = !old.texture && UnityEngine::Shader::GetGlobalTexture(key).unsafePtr() == nullptr;
      bool const restored = cleanup();
      textureOwnershipPassed = preserved && repeated && cleared && restored;
      if (!textureOwnershipPassed) textureOwnershipError = fmt::format(
          "controlBound={} initiallyBound={} replacementCreated={} preserved={} repeated={} cleared={} restored={} initial={} expected={}",
          controlBound, initiallyBound, replacementCreated, preserved, repeated, cleared, restored,
          static_cast<void*>(initialBinding), expectedBinding);
    } catch (std::exception const& ex) { textureOwnershipError = ex.what(); }
      catch (...) { textureOwnershipError = "native/managed exception during texture ownership fixture"; }
    PaperLogger.info("Vivify MENU DIAGNOSTICS: texture ownership={} (no draw); {}",
                     textureOwnershipPassed, textureOwnershipError);
  }

  static void Status() {
    auto& runtime = Runtime::Instance();
    WarmNative(runtime);
    CheckFixedGuard(runtime);
    if (lifetimeRequested && !lifetimeDone && IsManagedAlive(runtime._mainBundle) &&
        runtime._warmupQueue.empty() && donorIndex == donorMaterials.size() &&
        (SeparateEye::PreparationState() == "ready" || SeparateEye::PreparationState() == "not-requested")) {
      if (!lifetimeCollection) {
        std::unordered_set<UnityEngine::Object*> seen;
        for (auto const& [_, asset] : runtime._assets)
          if (IsManagedAlive(asset) && seen.insert(asset).second) lifetimeAssets.emplace_back(asset);
        lifetimeCollection = UnityEngine::Resources::UnloadUnusedAssets();
      } else if (lifetimeCollection->get_isDone()) {
        System::GC::Collect();
        for (auto const& asset : lifetimeAssets) if (!asset || !IsManagedAlive(asset.ptr())) ++lifetimeLost;
        lifetimeDone = true;
        PaperLogger.info("Vivify asset lifetime check: {} selected asset(s), {} lost after unused-resource cleanup", lifetimeAssets.size(), lifetimeLost);
        if (lifetimeLost) message = "Selected bundle assets were destroyed by unused-resource cleanup";
      }
    }
    // Resolve the actual engine bindings and load the embedded PC resource in
    // Unity. These are preparation checks only; no camera or level is started.
    bool const depthCopyReady = DepthCopy::Prepare();
    CheckSecondaryCameraRefresh(runtime);
    CheckBlitMaterialRefresh(runtime);
    CheckTextureOwnership(runtime);
    if (!cameraBindingsChecked) {
      auto* camera = UnityEngine::Camera::get_main().unsafePtr();
      if (IsManagedAlive(camera)) {
        cameraBindingsChecked = true;
        try {
          CameraBindings original;
          original.Capture(camera);
          constexpr std::array<char const*, 3> names = {"_ScreenParams", "_ZBufferParams", "_ProjectionParams"};
          std::array<UnityEngine::Vector4, 3> initial;
          for (size_t i = 0; i < names.size(); ++i)
            initial[i] = UnityEngine::Shader::GetGlobalVector(StringW(std::string(names[i])));
          auto set = [&](float seed) {
            for (size_t i = 0; i < names.size(); ++i)
              UnityEngine::Shader::SetGlobalVector(StringW(std::string(names[i])), UnityEngine::Vector4(seed + i, seed + i + 1, seed + i + 2, seed + i + 3));
          };
          auto equal = [](UnityEngine::Vector4 a, UnityEngine::Vector4 b) {
            return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) &&
                   std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
                   std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z) &&
                   std::bit_cast<uint32_t>(a.w) == std::bit_cast<uint32_t>(b.w);
          };
          auto matches = [&](float seed) {
            bool ok = true;
            for (size_t i = 0; i < names.size(); ++i)
              ok &= equal(UnityEngine::Shader::GetGlobalVector(StringW(std::string(names[i]))), UnityEngine::Vector4(seed + i, seed + i + 1, seed + i + 2, seed + i + 3));
            return ok;
          };
          auto* black = UnityEngine::Texture2D::get_blackTexture().unsafePtr();
          auto* white = UnityEngine::Texture2D::get_whiteTexture().unsafePtr();
          bool childRestored = false, childCorrect = false, parentRestored = false, parentCorrect = false;
          set(10); UnityEngine::Shader::SetGlobalTexture(u"_CameraDepthTexture", black);
          {
            CameraBindings parent; parent.Capture(camera);
            set(20); UnityEngine::Shader::SetGlobalTexture(u"_CameraDepthTexture", white);
            {
              CameraBindings child; child.Capture(camera);
              set(30); UnityEngine::Shader::SetGlobalTexture(u"_CameraDepthTexture", black);
              childRestored = child.Restore();
              childCorrect = matches(20) && UnityEngine::Shader::GetGlobalTexture(u"_CameraDepthTexture").unsafePtr() == white;
            }
            parentRestored = parent.Restore();
            parentCorrect = matches(10) && UnityEngine::Shader::GetGlobalTexture(u"_CameraDepthTexture").unsafePtr() == black;
          }
          bool const originalRestored = original.Restore();
          bool originalCorrect = true;
          for (size_t i = 0; i < names.size(); ++i)
            originalCorrect &= equal(UnityEngine::Shader::GetGlobalVector(StringW(std::string(names[i]))), initial[i]);
          nestedCameraBindingsPassed = childRestored && childCorrect && parentRestored && parentCorrect && originalCorrect;
          cameraBindingsPassed = originalRestored && nestedCameraBindingsPassed;
          PaperLogger.info("Vivify MENU DIAGNOSTICS: nested camera depth/screen/projection restoration={}", nestedCameraBindingsPassed);
        } catch (...) { cameraBindingsPassed = false; }
      }
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    w.Key("diagnostic_protocol"); w.Int(2);
    auto text = [&](char const* key, std::string const& value) { w.Key(key); w.String(value.c_str(), value.size()); };
    text("id", id); text("folder", folder); text("message", message);
    w.Key("projector_bindings_available"); w.Bool(Projectors::Available());
    w.Key("depth_copy_resource_ready"); w.Bool(depthCopyReady);
    w.Key("camera_bindings_checked"); w.Bool(cameraBindingsChecked);
    w.Key("camera_bindings_passed"); w.Bool(cameraBindingsPassed);
    w.Key("nested_camera_bindings_passed"); w.Bool(nestedCameraBindingsPassed);
    w.Key("secondary_camera_refresh_checked"); w.Bool(secondaryCameraRefreshChecked);
    w.Key("secondary_camera_refresh_passed"); w.Bool(secondaryCameraRefreshPassed);
    text("secondary_camera_refresh_error", secondaryCameraRefreshError);
    w.Key("blit_material_refresh_checked"); w.Bool(blitMaterialRefreshChecked);
    w.Key("blit_material_refresh_passed"); w.Bool(blitMaterialRefreshPassed);
    text("blit_material_refresh_error", blitMaterialRefreshError);
    w.Key("texture_ownership_checked"); w.Bool(textureOwnershipChecked);
    w.Key("texture_ownership_passed"); w.Bool(textureOwnershipPassed);
    text("texture_ownership_error", textureOwnershipError);
    w.Key("asset_lifetime_check_complete"); w.Bool(lifetimeDone);
    w.Key("asset_lifetime_objects"); w.Uint64(lifetimeAssets.size());
    w.Key("asset_lifetime_lost"); w.Uint64(lifetimeLost);
#ifdef VIVIFY_GAMEPLAY_DIAGNOSTICS
    w.Key("diagnostic_practice_running"); w.Bool(PlaybackDiagnostics::running);
    w.Key("legacy_stereo_mode"); w.Int(UnityEngine::XR::XRSettings::get_stereoRenderingMode().value__);
    w.Key("song_time"); w.Double(runtime.GetCurrentBeatmapData() ? runtime.CurrentSongTime() : 0);
    w.Key("song_duration"); w.Double(PlaybackDiagnostics::selected ? PlaybackDiagnostics::selected->songDuration : 0);
    w.Key("unity_frame_seconds"); w.Double(UnityEngine::Time::get_unscaledDeltaTime());
    w.Key("device_performance"); w.StartObject();
    try {
      auto stats = GlobalNamespace::OVRPlugin::GetAppPerfStats();
      bool const valid = stats.FrameStats && stats.FrameStatsCount > 0 && stats.FrameStatsCount <= stats.FrameStats.size();
      w.Key("valid"); w.Bool(valid);
      if (valid) {
        auto frame = stats.FrameStats[0];
        for (int i = 1; i < stats.FrameStatsCount; ++i)
          if (stats.FrameStats[i].AppFrameIndex > frame.AppFrameIndex) frame = stats.FrameStats[i];
        w.Key("frame_index"); w.Int(frame.AppFrameIndex);
        w.Key("dropped_frames"); w.Int(frame.AppDroppedFrameCount);
        w.Key("cpu_seconds"); w.Double(frame.AppCpuElapsedTime);
        w.Key("gpu_seconds"); w.Double(frame.AppGpuElapsedTime);
        w.Key("app_framerate"); w.Double(GlobalNamespace::OVRPlugin::GetAppFramerate());
      }
    } catch (...) { w.Key("unavailable"); w.Bool(true); }
    w.EndObject();
    w.Key("xr_performance"); w.StartObject();
    try {
      using FloatStat = bool (*)(UnityEngine::IntegratedSubsystem*, float*);
      using CountStat = bool (*)(UnityEngine::IntegratedSubsystem*, int*);
      using IntProperty = int (*)(UnityEngine::IntegratedSubsystem*);
      static auto gpu = reinterpret_cast<FloatStat>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::TryGetAppGPUTimeLastFrame"));
      static auto rate = reinterpret_cast<FloatStat>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::TryGetDisplayRefreshRate"));
      static auto dropped = reinterpret_cast<CountStat>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::TryGetDroppedFrameCount"));
      static auto presented = reinterpret_cast<CountStat>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::TryGetFramePresentCount"));
      static auto layout = reinterpret_cast<IntProperty>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::get_textureLayout"));
      static auto layouts = reinterpret_cast<IntProperty>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::get_supportedTextureLayouts"));
      static auto passes = reinterpret_cast<IntProperty>(il2cpp_functions::resolve_icall("UnityEngine.XR.XRDisplaySubsystem::GetRenderPassCount"));
      auto* subsystems = UnityEngine::SubsystemManager::getStaticF_s_IntegratedSubsystems();
      if (subsystems) for (int i = 0; i < subsystems->get_Count(); ++i) {
        auto* subsystem = subsystems->get_Item(i);
        if (!subsystem) continue;
        auto* klass = il2cpp_functions::object_get_class(reinterpret_cast<Il2CppObject*>(subsystem));
        if (std::string_view(il2cpp_functions::class_get_name(klass)) != "XRDisplaySubsystem") continue;
        float seconds = 0, hz = 0; int lost = 0, frames = 0;
        if (gpu && gpu(subsystem, &seconds) && std::isfinite(seconds) && seconds > 0) { w.Key("gpu_seconds"); w.Double(seconds); }
        if (rate && rate(subsystem, &hz) && std::isfinite(hz) && hz > 0) { w.Key("refresh_hz"); w.Double(hz); }
        if (dropped && dropped(subsystem, &lost)) { w.Key("dropped_frames"); w.Int(lost); }
        if (presented && presented(subsystem, &frames)) { w.Key("presented_frames"); w.Int(frames); }
        if (layout) { w.Key("texture_layout"); w.Int(layout(subsystem)); }
        if (layouts) { w.Key("supported_texture_layouts"); w.Int(layouts(subsystem)); }
        if (passes) { w.Key("render_pass_count"); w.Int(passes(subsystem)); }
        break;
      }
    } catch (...) { w.Key("unavailable"); w.Bool(true); }
    w.EndObject();
#endif
    w.Key("fixed_materials_validated"); w.Uint64(runtime._fixedFeatureMaterials.size());
    w.Key("fixed_material_violation"); w.Bool(runtime._fixedFeatureViolation);
    w.Key("fixed_material_change_probe_checked"); w.Bool(fixedGuardProbeChecked);
    w.Key("fixed_material_change_probe_passed"); w.Bool(fixedGuardProbePassed);
    w.Key("songs_loaded"); w.Bool(SongCore::API::Loading::AreSongsLoaded());
    text("selected_path", runtime._selectedLevelPath); text("bundle_path", runtime._selectedBundlePath);
    text("renderer", SeparateEye::PreparationState());
    w.Key("donors"); w.Int(SeparateEye::PreparedDonorCount());
    w.Key("untranslated_tessellation_shaders"); w.Int(SeparateEye::UntranslatedTessellationShaderCount());
    text("untranslated_tessellation_scope", "main scene eyes only; authored cameras and blits are separate");
    w.Key("source_tessellation_shaders"); w.Int(SeparateEye::SourceTessellationShaderCount());
    w.Key("active_scene_eye_tessellation_shaders"); w.Int(SeparateEye::ActiveSceneEyeTessellationShaderCount());
    w.Key("authored_camera_tessellation_shaders_untranslated"); w.Int(SeparateEye::SourceTessellationShaderCount());
    w.Key("blit_tessellation_shaders_untranslated"); w.Int(SeparateEye::SourceTessellationShaderCount());
    // Read-only engine capability. Stage-parser presence in libunity does not
    // prove that this graphics device advertises tessellation to Unity.
    using SupportsTessellation = bool (*)();
    static auto supportsTessellation = reinterpret_cast<SupportsTessellation>(
        il2cpp_functions::resolve_icall("UnityEngine.SystemInfo::SupportsTessellationShaders"));
    w.Key("engine_supports_tessellation_shaders");
    if (supportsTessellation) w.Bool(supportsTessellation()); else w.Null();
    bool const loaded = IsManagedAlive(runtime._mainBundle);
    w.Key("main_bundle_loaded"); w.Bool(loaded);
    w.Key("recovery_active"); w.Bool(GetRecoveryModeActive());
    w.Key("single_view_shader_count"); w.Uint64(runtime._singleViewShaders.size());
    w.Key("single_view_material_count"); w.Uint64(runtime._singleViewSwaps.size());
    w.Key("single_view_prefab_preload"); w.Bool(runtime._blitCompanionWantsPrefabs);
    w.Key("pre_effects"); w.Uint64(runtime._preEffects.size());
    w.Key("post_effects"); w.Uint64(runtime._postEffects.size());
    w.Key("fallback_materials"); w.Uint64(runtime._fallbackShadedMaterials.size());
    w.Key("shader_repair_failed"); w.Int(runtime._shaderRepairFailed);
    w.Key("material_pass_attempts"); w.Int(runtime._warmupPassAttempts);
    w.Key("material_pass_failures"); w.Int(runtime._warmupPassFailures);
    w.Key("donor_pass_attempts"); w.Int(donorPassAttempts);
    w.Key("donor_pass_failures"); w.Int(donorPassFailures);
    w.Key("expected_donor_non_draw_passes"); w.Int(expectedDonorNonDraw);
    w.Key("unexpected_donor_pass_failures"); w.Int(donorPassFailures - expectedDonorNonDraw);
    int expectedMaterialNonDraw = 0;
    w.Key("unresolved_material_passes"); w.StartArray();
    for (auto const& pass : runtime._warmupUnboundPasses) {
      if (ExpectedNonDraw(pass.shader, pass.pass, pass.passCount)) ++expectedMaterialNonDraw;
      else { w.StartObject(); text("shader", pass.shader); w.Key("pass"); w.Int(pass.pass); w.Key("pass_count"); w.Int(pass.passCount); w.EndObject(); }
    }
    w.EndArray();
    w.Key("expected_material_non_draw_passes"); w.Int(expectedMaterialNonDraw);
    w.Key("unexpected_material_pass_failures"); w.Int(runtime._warmupPassFailures - expectedMaterialNonDraw);
    w.Key("donor_materials_remaining"); w.Uint64(donorMaterials.size() - donorIndex);
    w.Key("warmup_remaining"); w.Uint64(runtime._warmupQueue.size() - std::min(runtime._warmupIndex, runtime._warmupQueue.size()));
    w.Key("gameplay_active"); w.Bool(runtime._currentBeatmapData != nullptr);
    w.Key("gates"); w.StartArray();
    bool blocked = false;
    for (auto const& gate : SongCore::API::PlayButton::GetPlayButtonDisablingModInfos()) {
      if (gate.modID != "Vivify" && gate.modID != "VivifyRenderer" && gate.modID != "VivifyFixedMaterial") continue;
      blocked = true;
      w.StartObject(); text("id", gate.modID); text("reason", gate.reason); w.EndObject();
    }
    w.EndArray();
    bool const rendererPending = !runtime._companionPcBundlePath.empty() && GetExperimentalSeparateEye() &&
                                 SeparateEye::PreparationState() != "ready";
    text("state", !message.empty() ? "error" : !active ? "idle" :
         SeparateEye::PreparationState() == "failed" ? "renderer-failed" :
         loaded && !blocked && runtime._warmupQueue.empty() && !rendererPending && donorIndex == donorMaterials.size() &&
         (!lifetimeRequested || lifetimeDone) ? "ready" : "preparing");
    w.EndObject();
    auto const tmp = std::string(directory) + "/status.tmp";
    { std::ofstream out(tmp); out << buffer.GetString() << '\n'; }
    std::filesystem::rename(tmp, std::string(directory) + "/status.json");
  }

  static void Select(rapidjson::Document const& command) {
    auto& runtime = Runtime::Instance();
    if (runtime._currentBeatmapData != nullptr) throw std::runtime_error("menu-only diagnostics refuse active gameplay");
    if (!SongCore::API::Loading::AreSongsLoaded()) throw std::runtime_error("SongCore has not loaded songs yet");
    if (!command.HasMember("folder") || !command["folder"].IsString()) throw std::runtime_error("folder missing");
    folder = command["folder"].GetString();
    if (folder.find('/') != std::string::npos || folder.find("..") != std::string::npos) throw std::runtime_error("invalid folder");
    if (!passTypes.IsObject()) {
      std::ifstream catalog(std::string(directory) + "/pass-types.json");
      std::string data((std::istreambuf_iterator<char>(catalog)), std::istreambuf_iterator<char>());
      passTypes.Parse(data.c_str());
      if (passTypes.HasParseError() || !passTypes.IsObject()) throw std::runtime_error("pass-kind catalog missing or invalid");
    }
    auto path = std::filesystem::path("/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/CustomLevels") / folder;
    auto* level = SongCore::API::Loading::GetLevelByPath(path);
    if (!level) throw std::runtime_error("installed level not found");
    std::string characteristic = command.HasMember("characteristic") && command["characteristic"].IsString() ? command["characteristic"].GetString() : "Standard";
    int difficulty = command.HasMember("difficulty") && command["difficulty"].IsInt() ? command["difficulty"].GetInt() : 4;
    if (difficulty < 0 || difficulty > 4) throw std::runtime_error("invalid difficulty");
    auto save = level->get_CustomSaveDataInfo();
    if (!save) throw std::runtime_error("custom metadata missing");
    auto details = save->get().TryGetBasicLevelDetails();
    if (!details) throw std::runtime_error("custom level details missing");
    auto group = details->get().TryGetCharacteristic(characteristic);
    if (!group) throw std::runtime_error("characteristic metadata missing");
    auto diff = group->get().TryGetDifficulty(GlobalNamespace::BeatmapDifficulty(difficulty));
    if (!diff) throw std::runtime_error("difficulty metadata missing");
    GlobalNamespace::BeatmapCharacteristicSO* characteristicObject = nullptr;
    for (auto* value : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::BeatmapCharacteristicSO*>())
      if (IsManagedAlive(value) && std::string(value->get_serializedName()) == characteristic) { characteristicObject = value; break; }
    if (!characteristicObject) throw std::runtime_error("characteristic object missing");
    SongCore::API::LevelSelect::LevelWasSelectedEventArgs event;
    event.isCustom = true; event.customBeatmapLevel = level;
    event.levelID = std::string(level->levelID);
    if (event.levelID.starts_with("custom_level_")) event.hash = event.levelID.substr(13);
    event.customLevelInfoSaveDataV2 = level->standardLevelInfoSaveDataV2;
    event.customBeatmapLevelSaveDataV4 = level->beatmapLevelSaveDataV4;
    event.customLevelDetails.emplace(SongCore::API::LevelSelect::LevelWasSelectedEventArgs::BasicCustomLevelDetailsGroup{details->get(), group->get(), diff->get()});
    event.beatmapKey.beatmapCharacteristic = characteristicObject;
    event.beatmapKey.difficulty = GlobalNamespace::BeatmapDifficulty(difficulty);
    event.beatmapKey.levelId = level->levelID;
    ClearDonorWarmup(); active = true; nativeWarmupQueued = false;
    lifetimeRequested = command.HasMember("collect_unused") && command["collect_unused"].IsBool() && command["collect_unused"].GetBool();
#ifdef VIVIFY_GAMEPLAY_DIAGNOSTICS
    PlaybackDiagnostics::selected = level; PlaybackDiagnostics::key = event.beatmapKey;
    SongCore::API::LevelSelect::GetLevelWasSelectedEvent().invoke(event);
#else
    runtime.HandleLevelSelected(event);
#endif
    if (!runtime._selectedMapHasVivifyRequirement) throw std::runtime_error("selected difficulty does not require Vivify");
    PaperLogger.info("Vivify MENU DIAGNOSTICS: preparing '{}' only; no song will be started", folder);
  }

  static void Poll() {
#ifdef VIVIFY_GAMEPLAY_DIAGNOSTICS
    PlaybackDiagnostics::Poll();
#endif
    static bool logged = false;
    if (!logged) {
      std::filesystem::create_directories(directory);
      PaperLogger.info("Vivify MENU DIAGNOSTICS: app-private command polling active");
      logged = true;
    }
    WarmDonors();
    float now = UnityEngine::Time::get_realtimeSinceStartup();
    if (lastPoll >= 0 && now - lastPoll < 0.5f) return;
    lastPoll = now;
    std::string const path = std::string(directory) + "/command.json";
    if (!std::filesystem::exists(path)) { if (active) Status(); return; }
    std::ifstream in(path);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    rapidjson::Document command;
    command.Parse(data.c_str());
    if (command.HasParseError() || !command.IsObject() || !command.HasMember("id") || !command["id"].IsString()) return;
    std::filesystem::remove(path);
    id = command["id"].GetString(); message.clear();
    try {
      if (!command.HasMember("action") || !command["action"].IsString()) throw std::runtime_error("action missing");
      std::string action = command["action"].GetString();
      if (action == "prepare") Select(command);
      else if (action == "unload") {
        if (Runtime::Instance()._currentBeatmapData) throw std::runtime_error("refuse to unload during gameplay");
        ClearDonorWarmup(); Runtime::Instance().HandleLevelSelected({});
        Runtime::Instance().ReleaseBundlesForReset(); active = false; folder.clear();
        UnityEngine::Resources::UnloadUnusedAssets();
      }
#ifdef VIVIFY_GAMEPLAY_DIAGNOSTICS
      else if (action == "continue-health") PlaybackDiagnostics::ContinueHealth();
      else if (action == "stop-practice") PlaybackDiagnostics::Stop();
      else if (action == "capture-textures") PlaybackDiagnostics::CaptureTextures(directory);
      else if (action == "start-practice") {
        float const until = command.HasMember("until") && command["until"].IsNumber() ? command["until"].GetFloat() : 105.0f;
        PlaybackDiagnostics::Start(until);
      }
#endif
      else if (action != "status") throw std::runtime_error("unknown diagnostic action");
    } catch (std::exception const& ex) { message = ex.what(); PaperLogger.error("Vivify MENU DIAGNOSTICS: {}", message); }
    Status();
  }
};
void PollMenuDiagnostics() {
  try { MenuDiagnostics::Poll(); }
  catch (std::exception const& ex) { PaperLogger.error("Vivify menu diagnostics failed: {}", ex.what()); }
  catch (...) { PaperLogger.error("Vivify menu diagnostics failed"); }
}
}
