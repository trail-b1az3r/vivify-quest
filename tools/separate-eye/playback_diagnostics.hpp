#pragma once
#include "VivifyComponents.hpp"
// Explicitly authorized local regression checks. Never compiled into releases.
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/HealthWarningFlowCoordinator.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataFileModel.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/RecordingToolManager.hpp"
#include "System/Nullable_1.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/VRTextureUsage.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "GlobalNamespace/OVRPlugin.hpp"
#include "VivifyCameraBindings.hpp"
#include "UnityEngine/SubsystemManager.hpp"
#include "UnityEngine/IntegratedSubsystem.hpp"
#include "UnityEngine/XR/XRSettings.hpp"
#include <unistd.h>

namespace Vivify {
struct PlaybackDiagnostics {
  inline static SafePtr<GlobalNamespace::BeatmapLevel> selected;
  inline static GlobalNamespace::BeatmapKey key;
  inline static bool running = false;
  inline static bool sawGameplay = false;
  inline static float stopAt = 0, deadline = 0;

  template <class T> static T* Find() {
    for (auto* item : UnityEngine::Resources::FindObjectsOfTypeAll<T*>())
      if (IsManagedAlive(item)) return item;
    return nullptr;
  }
  static void ContinueHealth() {
    auto* health = Find<GlobalNamespace::HealthWarningFlowCoordinator>();
    if (!IsManagedAlive(health) || !health->get_isActivated())
      throw std::runtime_error("Health and Safety screen is not active");
    // Only the already-observed Health and Safety Continue action. Never
    // accept EULA, privacy, region or account dialogs through this harness.
    health->HandleHealthWarningViewControllerDidFinish();
  }
  static void Stop() {
    if (!running) throw std::runtime_error("No diagnostic practice run is owned by this harness");
    auto* helper = Find<GlobalNamespace::MenuTransitionsHelper>();
    if (!helper) throw std::runtime_error("Menu transition helper unavailable");
    helper->StopStandardLevel();
    running = false;
  }
  static void CaptureTextures(std::string const& directory) {
    if (!running || !Runtime::Instance().GetCurrentBeatmapData())
      throw std::runtime_error("Texture readback requires an owned diagnostic practice run");
    auto path = std::filesystem::path(directory) / "captures";
    std::filesystem::create_directories(path);
    struct Reference {
      SafePtr<UnityEngine::GameObject> object;
      SafePtr<UnityEngine::RenderTexture> color;
      CameraBindings bindings;
      ~Reference() {
        if (object && IsManagedAlive(object.ptr())) UnityEngine::Object::Destroy(object.ptr());
        if (color && IsManagedAlive(color.ptr())) { color->Release(); UnityEngine::Object::Destroy(color.ptr()); }
      }
    } reference;
    auto* main = UnityEngine::Camera::get_main().unsafePtr();
    if (IsManagedAlive(main)) {
      reference.bindings.Capture(main);
      reference.object = UnityEngine::GameObject::New_ctor(u"Vivify Diagnostic Reference Camera");
      reference.object->set_hideFlags(UnityEngine::HideFlags::HideAndDontSave);
      auto* camera = reference.object->AddComponent<UnityEngine::Camera*>();
      camera->CopyFrom(main); camera->set_enabled(false); camera->RemoveAllCommandBuffers();
      camera->set_stereoTargetEye(UnityEngine::StereoTargetEyeMask::None);
      camera->ResetStereoViewMatrices(); camera->ResetWorldToCameraMatrix(); camera->ResetProjectionMatrix(); camera->ResetCullingMatrix();
      camera->get_transform()->SetParent(main->get_transform()->get_parent(), false);
      camera->get_transform()->set_localPosition(UnityEngine::Vector3(0, 1.6f, 0));
      camera->get_transform()->set_localRotation(UnityEngine::Quaternion::get_identity());
      camera->set_fieldOfView(70); camera->set_aspect(1); camera->set_allowMSAA(false);
      reference.color = UnityEngine::RenderTexture::New_ctor(1024, 1024, 24, UnityEngine::RenderTextureFormat::ARGBHalf);
      reference.color->set_name(u"Vivify Reference Color"); reference.color->Create();
      camera->set_targetTexture(reference.color.ptr());
      SyncCameraBloom(main, camera, nullptr);
      reference.object->AddComponent<SingleViewCameraShaders*>();
      auto* effects = reference.object->AddComponent<CameraApplier*>();
      effects->cameraName = "_DiagnosticReference";
      auto& runtime = Runtime::Instance();
      if (runtime._cameraApplier && IsManagedAlive(runtime._cameraApplier)) {
        effects->hasMainEffect = runtime._cameraApplier->hasMainEffect;
        effects->mainEffectController = runtime._cameraApplier->mainEffectController;
      }
      camera->Render();
    }
    auto frame = std::to_string(getpid()) + "-" + std::to_string(UnityEngine::Time::get_frameCount());
    std::ofstream catalog(path / (frame + "-catalog.txt"));
    std::unordered_set<UnityEngine::RenderTexture*> targets;
    for (auto* camera : UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Camera*>()) {
      if (!IsManagedAlive(camera) || !camera->get_enabled() || !camera->get_gameObject()->get_activeInHierarchy()) continue;
      auto pos = camera->get_transform()->get_position();
      auto rot = camera->get_transform()->get_rotation();
      auto* target = camera->get_targetTexture().unsafePtr();
      catalog << "camera\t" << std::string(camera->get_name()) << "\tposition=" << pos.x << ',' << pos.y << ',' << pos.z
              << "\trotation=" << rot.x << ',' << rot.y << ',' << rot.z << ',' << rot.w
              << "\tmask=" << camera->get_cullingMask() << "\tortho=" << camera->get_orthographic() << '\n';
      if (IsManagedAlive(target)) targets.insert(target);
    }
    for (auto* rt : UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::RenderTexture*>())
      if (IsManagedAlive(rt) && std::string(rt->get_name()).starts_with("Vivify ")) targets.insert(rt);
    for (auto* renderer : UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Renderer*>()) {
      if (!IsManagedAlive(renderer) || !renderer->get_gameObject()->get_activeInHierarchy()) continue;
      for (auto material : renderer->get_sharedMaterials()) {
        auto* mat = material.unsafePtr();
        if (!IsManagedAlive(mat) || !IsManagedAlive(mat->get_shader().unsafePtr())) continue;
        std::string shader(mat->get_shader()->get_name());
        if (shader.find("Saber") == std::string::npos && shader.find("AreaLit") == std::string::npos) continue;
        auto* go = renderer->get_gameObject().unsafePtr(); auto pos = renderer->get_transform()->get_position();
        catalog << "renderer\t" << std::string(go->get_name()) << "\tshader=" << shader << "\tlayer=" << go->get_layer()
                << "\tenabled=" << renderer->get_enabled() << "\tposition=" << pos.x << ',' << pos.y << ',' << pos.z << '\n';
      }
    }
    struct Active {
      SafePtr<UnityEngine::RenderTexture> previous = UnityEngine::RenderTexture::get_active().unsafePtr();
      ~Active() { UnityEngine::RenderTexture::set_active(previous && IsManagedAlive(previous.ptr()) ? previous.ptr() : nullptr); }
    } restore;
    for (auto* rt : targets) {
      if (!rt->IsCreated() || rt->get_width() > 2048 || rt->get_height() > 2048) continue;
      auto descriptor = rt->get_descriptor();
      auto dim = descriptor.get_dimension();
      if (dim != UnityEngine::Rendering::TextureDimension::Tex2D && dim != UnityEngine::Rendering::TextureDimension::Tex2DArray) continue;
      int const count = dim == UnityEngine::Rendering::TextureDimension::Tex2DArray ? descriptor.get_volumeDepth() : 1;
      if (count > 2) continue;
      std::string name(rt->get_name());
      for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
      catalog << "texture\t" << rt->GetInstanceID() << '\t' << name << '\t' << rt->get_width() << 'x' << rt->get_height() << "\tlayers=" << count << '\n';
      for (int slice = 0; slice != count; ++slice) {
        SafePtr<UnityEngine::RenderTexture> temporary;
        SafePtr<UnityEngine::RenderTexture> readable;
        SafePtr<UnityEngine::Texture2D> image;
        try {
          auto* source = rt;
          if (count > 1) {
            descriptor.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D); descriptor.set_volumeDepth(1);
            descriptor.set_vrUsage(UnityEngine::VRTextureUsage::None); descriptor.set_depthBufferBits(0);
            temporary = UnityEngine::RenderTexture::GetTemporary(descriptor).unsafePtr();
            UnityEngine::Graphics::CopyTexture(rt, slice, 0, temporary.ptr(), 0, 0); source = temporary.ptr();
          }
          // GLES ReadPixels cannot convert every authored HDR/RFloat buffer
          // directly into RGBA32. Resolve/convert into a known color framebuffer
          // first; otherwise EncodeToPNG can expose uninitialized CPU pixels.
          readable = UnityEngine::RenderTexture::GetTemporary(source->get_width(), source->get_height(), 0,
                                                              UnityEngine::RenderTextureFormat::ARGB32).unsafePtr();
          UnityEngine::Graphics::Blit(source, readable.ptr());
          UnityEngine::RenderTexture::set_active(readable.ptr());
          image = UnityEngine::Texture2D::New_ctor(source->get_width(), source->get_height(), UnityEngine::TextureFormat::RGBA32, false);
          image->ReadPixels(UnityEngine::Rect(0, 0, source->get_width(), source->get_height()), 0, 0, false);
          auto bytes = UnityEngine::ImageConversion::EncodeToPNG(image.ptr());
          auto file = path / (frame + "-" + std::to_string(rt->GetInstanceID()) + "-" + name + "-" + std::to_string(slice) + ".png");
          std::ofstream output(file, std::ios::binary); output.write(reinterpret_cast<char const*>(bytes.begin()), bytes.size());
        } catch (...) { catalog << "readback-failed\t" << rt->GetInstanceID() << '\t' << slice << '\n'; }
        UnityEngine::RenderTexture::set_active(restore.previous && IsManagedAlive(restore.previous.ptr()) ? restore.previous.ptr() : nullptr);
        if (image && IsManagedAlive(image.ptr())) UnityEngine::Object::Destroy(image.ptr());
        if (readable && IsManagedAlive(readable.ptr())) UnityEngine::RenderTexture::ReleaseTemporary(readable.ptr());
        if (temporary && IsManagedAlive(temporary.ptr())) UnityEngine::RenderTexture::ReleaseTemporary(temporary.ptr());
      }
    }
  }
  static void Start(float until) {
    auto& runtime = Runtime::Instance();
    if (running || runtime.GetCurrentBeatmapData()) throw std::runtime_error("Refuse to replace active gameplay");
    if (GetSubmitScoresOnVivifyMaps()) throw std::runtime_error("Disable Vivify score submission before diagnostic playback");
    if (!selected || !std::isfinite(until) || until < 10 || until > 600)
      throw std::runtime_error("Select a map and a bounded 10-600 second practice duration");
    if (GetExperimentalSeparateEye() && SeparateEye::PreparationState() != "ready")
      throw std::runtime_error("Renderer is not ready");
    for (auto const& gate : SongCore::API::PlayButton::GetPlayButtonDisablingModInfos())
      throw std::runtime_error("Play is disabled by " + gate.modID + ": " + gate.reason);
    auto* helper = Find<GlobalNamespace::MenuTransitionsHelper>();
    auto* model = Find<GlobalNamespace::PlayerDataModel>();
    if (!helper || !model || !model->get_playerData() || !model->get_playerDataFileModel())
      throw std::runtime_error("Initialized menu/player settings unavailable");
    auto* player = model->get_playerData();
    // ScoreSaber 2.5.0 processes replay output before checking score disablers
    // and assumes the normal practice/menu flow initialized its replay state.
    // This harness enters gameplay directly. Use its supported process-only
    // escape hatch so automated, score-disabled practice never enters that
    // unrelated finish path. The normal build excludes this entire header;
    // restarting into it discards the environment override.
    if (::setenv("disable_ss_upload", "1", 1) != 0)
      throw std::runtime_error("Could not isolate diagnostic practice from ScoreSaber uploads");
    auto* modifiers = GlobalNamespace::GameplayModifiers::New_ctor();
    modifiers->____noFailOn0Energy = true;
    auto* practice = GlobalNamespace::PracticeSettings::New_ctor(0.0f, 1.0f);
    // Separate settings instances; the player's saved modifiers are untouched.
    helper->StartStandardLevel(u"Solo", byref(key), selected.ptr(), player->get_overrideEnvironmentSettings(), nullptr,
      false, nullptr, modifiers, player->get_playerSpecificSettings(), practice,
      model->get_playerDataFileModel()->____environmentsListModel, u"Diagnostic", false, false,
      nullptr, nullptr, nullptr, nullptr, System::Nullable_1<GlobalNamespace::RecordingToolManager_SetupData>{});
    stopAt = until; deadline = UnityEngine::Time::get_realtimeSinceStartup() + until + 60;
    running = true; sawGameplay = false;
    PaperLogger.warn("Vivify DIAGNOSTIC PRACTICE: No Fail, practice mode, score submission disabled, bounded at {} seconds", until);
  }
  static void Poll() {
    if (!running) return;
    auto& runtime = Runtime::Instance();
    sawGameplay |= runtime.GetCurrentBeatmapData() != nullptr;
    if (sawGameplay && !runtime.GetCurrentBeatmapData()) { running = false; return; }
    if ((runtime.GetCurrentBeatmapData() && runtime.CurrentSongTime() >= stopAt) ||
        UnityEngine::Time::get_realtimeSinceStartup() >= deadline) Stop();
  }
};
}
