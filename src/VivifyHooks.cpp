#include "VivifyRuntimeInternal.hpp"
#include <set>
#include "VivifyComponents.hpp"
#include "VivifySeparateEye.hpp"
#include "VivifySeparateEyePolicy.hpp"
#include "VivifyCameraBindings.hpp"
#include "GlobalNamespace/MirrorRendererSO.hpp"
#include "GlobalNamespace/UserInfo.hpp"
#ifdef VIVIFY_GAME_1_45_3
#include "GlobalNamespace/MainEffectPostRenderPass.hpp"
#include "GlobalNamespace/MainEffectRendererFeature.hpp"
#include "UnityEngine/Rendering/CommandBufferHelpers.hpp"
#include "UnityEngine/Rendering/RenderGraphModule/RasterGraphContext.hpp"
#include "UnityEngine/Rendering/RenderGraphModule/TextureHandle.hpp"
#include "UnityEngine/Rendering/RenderGraphModule/UnsafeGraphContext.hpp"
#include "UnityEngine/Rendering/Universal/CameraData.hpp"
#include "UnityEngine/Rendering/Universal/RenderingData.hpp"
#include "UnityEngine/Rendering/Universal/ScriptableRenderer.hpp"
#include "UnityEngine/Rendering/UnsafeCommandBuffer.hpp"
#endif

using namespace Vivify;
using namespace std::string_view_literals;

namespace {
thread_local SeparateEye::MirrorRenderStack mirrorRenders;
struct MirrorRenderScope {
  MirrorRenderScope(GlobalNamespace::MirrorRendererSO* source, GlobalNamespace::MirrorRendererSO* target) {
    mirrorRenders.Push(reinterpret_cast<uintptr_t>(source), reinterpret_cast<uintptr_t>(target));
  }
  ~MirrorRenderScope() { mirrorRenders.Pop(); }
};

// Vivify's part of a hook, run so that nothing it throws leaves the hook
// (0.14.25). A hook sits in a chain shared with other mods (custom sabers and
// notes, Qosmetics, Chroma, Noodle): an exception from Vivify's code unwinding
// out of it skips whatever the other mods and the game still had to do there,
// and on Quest usually takes the game down. The game's own function -- the
// call into the rest of the chain -- is never inside this, so its behaviour,
// exceptions included, is exactly what it would be without Vivify.
template <typename Body>
void VivifyPart(std::string_view hook, Body&& body) {
  try {
    body();
  } catch (std::exception const& ex) {
    static std::set<std::string_view> sReported;
    if (sReported.insert(hook).second) {
      PaperLogger.error("Vivify: {} threw ({}); skipped Vivify's part of it, the game and other mods "
                        "carry on. Repeats are not logged", hook, ex.what());
    }
  } catch (...) {
    static std::set<std::string_view> sReported;
    if (sReported.insert(hook).second) {
      PaperLogger.error("Vivify: {} threw a non-std exception; skipped Vivify's part of it. Repeats are "
                        "not logged", hook);
    }
  }
}

#ifdef VIVIFY_GAME_1_45_3
// Screen effects on Beat Saber 1.45.3, which renders with URP's render graph:
// there is no OnRenderImage. The game's own post-processing pass
// (MainEffectPostRenderPass, bloom and tone mapping) runs once per camera
// with a command buffer and that camera's colour texture; Vivify's Blit
// effects are recorded into the same buffer just before the game's, so they
// run at that point of the frame.
namespace {
// The camera URP is setting up passes for. URP records and executes one
// camera's render graph before setting up the next, so the passes that run
// next are this camera's. Only compared, never dereferenced.
UnityEngine::Camera* gUrpCamera = nullptr;
bool gReportedUrpEffects = false;
bool gReportedNoPostProcess = false;
bool IsUrpMainCamera() {
  auto* main = VivifyMainCamera();
  return gUrpCamera != nullptr && main != nullptr && gUrpCamera == main;
}
}  // namespace

MAKE_HOOK_MATCH(MainEffectRendererFeature_AddRenderPasses, &GlobalNamespace::MainEffectRendererFeature::AddRenderPasses, void,
                GlobalNamespace::MainEffectRendererFeature* self, UnityEngine::Rendering::Universal::ScriptableRenderer* renderer,
                by_ref<UnityEngine::Rendering::Universal::RenderingData> renderingData) {
  VivifyPart("MainEffectRendererFeature.AddRenderPasses", [&] {
    auto camera = renderingData->cameraData.get_camera();
    gUrpCamera = (*camera).unsafe_ptr();
  });
  MainEffectRendererFeature_AddRenderPasses(self, renderer, renderingData);
}

MAKE_HOOK_MATCH(MainEffectPostRenderPass_ExecutePyramidBloomPass, &GlobalNamespace::MainEffectPostRenderPass::ExecutePyramidBloomPass,
                void, GlobalNamespace::MainEffectPostRenderPass_PassData* data,
                UnityEngine::Rendering::RenderGraphModule::UnsafeGraphContext* context) {
  VivifyPart("MainEffectPostRenderPass.ExecutePyramidBloomPass", [&] {
    auto& runtime = Runtime::Instance();
    if (!IsUrpMainCamera() || data == nullptr || context == nullptr) {
      runtime.RecordScreenEffects(nullptr, nullptr, 0);  // still frees last frame's temporaries
      return;
    }
    auto* commands = UnityEngine::Rendering::CommandBufferHelpers::GetNativeCommandBuffer(context->cmd);
    auto* color = UnityEngine::Rendering::RenderGraphModule::TextureHandle::op_Implicit___UnityW___UnityEngine__RenderTexture_(
                      data->activeColorTexture).unsafe_ptr();
    // The Separate-Eye Renderer's two eye images, drawn before the frame
    // (UrpPrepareFrame), go into the main image first.
    SeparateEye::NoteUrpPostPass(gUrpCamera, color);
    SeparateEye::UrpPresent(commands, color);
    // All of the map's effects go before the game's bloom: where the game
    // writes its result is not known here, and effects drawn into a texture
    // it no longer reads would be lost. A post-bloom effect gets bloom over
    // it instead.
    if (!runtime.HasScreenEffects(0)) {
      runtime.RecordScreenEffects(nullptr, nullptr, 0);
      return;
    }
    runtime.RecordScreenEffects(commands, color, 0);
    if (!gReportedUrpEffects) {
      gReportedUrpEffects = true;
      PaperLogger.info("Vivify screen effects (URP): recorded into the game's post-processing pass on '{}'",
                       IsManagedAlive(color) ? std::string(color->get_name()) : std::string("<no colour texture>"));
    }
  });
  MainEffectPostRenderPass_ExecutePyramidBloomPass(data, context);
}

MAKE_HOOK_MATCH(MainEffectPostRenderPass_ExecuteNoPostProcessPass, &GlobalNamespace::MainEffectPostRenderPass::ExecuteNoPostProcessPass,
                void, GlobalNamespace::MainEffectPostRenderPass_PassData* data,
                UnityEngine::Rendering::RenderGraphModule::RasterGraphContext context) {
  VivifyPart("MainEffectPostRenderPass.ExecuteNoPostProcessPass", [&] {
    if (!gReportedNoPostProcess && IsUrpMainCamera() && Runtime::Instance().HasScreenEffects(0)) {
      gReportedNoPostProcess = true;
      PaperLogger.warn("Vivify screen effects (URP): this map has screen effects, but the game's bloom/post-processing "
                       "is off, and Vivify draws them in that pass; they are not shown");
    }
  });
  MainEffectPostRenderPass_ExecuteNoPostProcessPass(data, context);
}
#endif

#ifdef VIVIFY_GAME_1_45_3
// 1.45.3 added the model name.
MAKE_HOOK_MATCH(SaberModelController_Init, &GlobalNamespace::SaberModelController::Init, void, GlobalNamespace::SaberModelController* self, UnityEngine::Transform* parent, GlobalNamespace::Saber* saber, UnityEngine::Color trailTintColor, StringW modelName) {
  SaberModelController_Init(self, parent, saber, trailTintColor, modelName);
#else
MAKE_HOOK_MATCH(SaberModelController_Init, &GlobalNamespace::SaberModelController::Init, void, GlobalNamespace::SaberModelController* self, UnityEngine::Transform* parent, GlobalNamespace::Saber* saber, UnityEngine::Color trailTintColor) {
  SaberModelController_Init(self, parent, saber, trailTintColor);
#endif
  VivifyPart("SaberModelController.Init", [&] { Runtime::Instance().TrackSaberModel(self, saber, parent); });
}

MAKE_HOOK_MATCH(GameNoteController_Init, &GlobalNamespace::GameNoteController::Init, void, GlobalNamespace::GameNoteController* self, GlobalNamespace::NoteData* noteData, VIVIFY_IN(GlobalNamespace::NoteSpawnData) noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float cutAngleTolerance, float uniformScale) {
  GameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, cutAngleTolerance, uniformScale);
  VivifyPart("GameNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(BombNoteController_Init, &GlobalNamespace::BombNoteController::Init, void, GlobalNamespace::BombNoteController* self, GlobalNamespace::NoteData* noteData, VIVIFY_IN(GlobalNamespace::NoteSpawnData) noteSpawnData) {
  BombNoteController_Init(self, noteData, noteSpawnData);
  VivifyPart("BombNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(BurstSliderGameNoteController_Init, &GlobalNamespace::BurstSliderGameNoteController::Init, void, GlobalNamespace::BurstSliderGameNoteController* self, GlobalNamespace::NoteData* noteData, VIVIFY_IN(GlobalNamespace::NoteSpawnData) noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float uniformScale) {
  BurstSliderGameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, uniformScale);
  VivifyPart("BurstSliderGameNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(NoteCutCoreEffectsSpawner_SpawnNoteCutEffect, &GlobalNamespace::NoteCutCoreEffectsSpawner::SpawnNoteCutEffect, void, GlobalNamespace::NoteCutCoreEffectsSpawner* self, VIVIFY_IN(GlobalNamespace::NoteCutInfo) noteCutInfo, GlobalNamespace::NoteController* noteController, int32_t sparkleParticlesCount, int32_t explosionParticlesCount) {

  bool pushed = false;
  VivifyPart("NoteCutCoreEffectsSpawner.SpawnNoteCutEffect", [&] {
    bool const customDebris = Runtime::Instance().GetCurrentBeatmapData() != nullptr &&
                              !Runtime::Instance().IsResetting() && !Runtime::Instance().IsReduceDebrisEnabled();
    if (customDebris) {
      Runtime::Instance().PushActiveDebrisPrefabs(
          Runtime::Instance().FindAssignedDebrisPrefabs(noteCutInfo->noteData));
      pushed = true;
    }
  });
  NoteCutCoreEffectsSpawner_SpawnNoteCutEffect(self, noteCutInfo, noteController, sparkleParticlesCount, explosionParticlesCount);
  if (pushed) {
    VivifyPart("NoteCutCoreEffectsSpawner.SpawnNoteCutEffect", [&] { Runtime::Instance().PopActiveDebrisPrefabs(); });
  }
}

MAKE_HOOK_MATCH(NoteDebris_Init, &GlobalNamespace::NoteDebris::Init, void, GlobalNamespace::NoteDebris* self, GlobalNamespace::ColorType colorType, UnityEngine::Vector3 notePos, UnityEngine::Quaternion noteRot, UnityEngine::Vector3 noteMoveVec, UnityEngine::Vector3 noteScale, UnityEngine::Vector3 positionOffset, UnityEngine::Quaternion rotationOffset, UnityEngine::Vector3 cutPoint, UnityEngine::Vector3 cutNormal, UnityEngine::Vector3 force, UnityEngine::Vector3 torque, float lifeTime, UnityEngine::Vector3 cutoutOffset, bool forceOnlySimplePhysics) {
  NoteDebris_Init(self, colorType, notePos, noteRot, noteMoveVec, noteScale, positionOffset, rotationOffset, cutPoint, cutNormal, force, torque, lifeTime, cutoutOffset, forceOnlySimplePhysics);
  VivifyPart("NoteDebris.Init", [&] {
    Runtime::Instance().RestoreDebrisVisuals(self);
    if (Runtime::Instance().GetCurrentBeatmapData() == nullptr || Runtime::Instance().IsResetting()) return;
    Runtime::Instance().ReplaceDebrisVisuals(self);
  });
}

MAKE_HOOK_MATCH(AlwaysVisibleQuad_OnEnable, &GlobalNamespace::AlwaysVisibleQuad::OnEnable, void, GlobalNamespace::AlwaysVisibleQuad* self) {
  AlwaysVisibleQuad_OnEnable(self);
  VivifyPart("AlwaysVisibleQuad.OnEnable", [&] {
    if (Runtime::Instance().GetCurrentBeatmapData() == nullptr || Runtime::Instance().IsResetting()) return;
    if (!IsManagedAlive(self)) return;
    auto transform = self->get_transform();
    if (!IsManagedAlive(transform.unsafePtr())) return;
    transform->set_position(UnityEngine::Vector3(0.0f, -1000.0f, 0.0f));
  });
}

#ifndef VIVIFY_GAME_1_45_3
MAKE_HOOK_MATCH(ImageEffectController_OnRenderImage, &GlobalNamespace::ImageEffectController::OnRenderImage, void, GlobalNamespace::ImageEffectController* self, UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  bool bypass = false;
  VivifyPart("ImageEffectController.OnRenderImage", [&] { bypass = Runtime::Instance().ShouldBypassImageEffect(self); });
  if (bypass) {
    UnityEngine::Graphics::Blit(static_cast<UnityEngine::Texture*>(src), dest);
    return;
  }
  ImageEffectController_OnRenderImage(self, src, dest);
}
#endif

// 1.45.3 renders mirrors with URP's PlanarMirrorRenderPass; these hooks
// are for 1.40.8's MirrorRendererSO.
#ifndef VIVIFY_GAME_1_45_3
MAKE_HOOK_MATCH(MirrorRendererSO_RenderMirrorTexture, &GlobalNamespace::MirrorRendererSO::RenderMirrorTexture,
                UnityW<UnityEngine::Texture>, GlobalNamespace::MirrorRendererSO* self,
                UnityEngine::Vector3 position, UnityEngine::Vector3 normal) {
  auto* target = self;
  CameraBindings bindings;
  VivifyPart("MirrorRendererSO.RenderMirrorTexture", [&] {
    auto active = mirrorRenders.Resolve(reinterpret_cast<uintptr_t>(self));
    auto* candidate = active ? reinterpret_cast<GlobalNamespace::MirrorRendererSO*>(active) : SeparateEye::MirrorForCurrentCamera(self);
    if (candidate != self) bindings.Capture(UnityEngine::Camera::get_current().unsafePtr());
    target = candidate;
  });
  MirrorRenderScope const scope(self, target);
  auto result = MirrorRendererSO_RenderMirrorTexture(target, position, normal);
  if (!bindings.Restore()) SeparateEye::RejectCameraBindings();
  return result;
}
// The game's mirror draws its reflection once per eye, into the left and
// right halves of its stereo texture, from a head pose it works out itself
// (0.15.40). In Lifelike (its runway and water) and luminescent (the lake) the
// reflection is the game's floor mirror, and on the Quest it turned and slid
// with the head: the pose the mirror drew from was not the one the headset
// drew that eye with. Each half is now drawn from the eye's own view and
// projection, the ones the frame itself uses. Where the game's pose was
// already right this changes nothing; the first stereo draw of each level
// logs how far apart the two were.
namespace {
void const* mirrorPoseLevel = nullptr;
}
MAKE_HOOK_MATCH(MirrorRendererSO_RenderMirror, &GlobalNamespace::MirrorRendererSO::RenderMirror,
                void, GlobalNamespace::MirrorRendererSO* self, UnityEngine::Vector3 camPosition,
                UnityEngine::Quaternion camRotation, UnityEngine::Matrix4x4 camProjectionMatrix,
                UnityEngine::Rect screenRect, UnityEngine::Vector3 planePosition, UnityEngine::Vector3 planeNormal) {
  VivifyPart("MirrorRendererSO.RenderMirror", [&] {
    auto& runtime = Runtime::Instance();
    if (runtime.GetCurrentBeatmapData() == nullptr) return;
    auto* camera = UnityEngine::Camera::get_current().unsafePtr();
    if (!IsManagedAlive(camera) || !camera->get_stereoEnabled()) return;
    float const x = screenRect.get_x(), width = screenRect.get_width();
    bool half = false;
    int eye = 0;
    if (width > 0.0f && width <= 1.0f) {
      half = width < 0.75f;
      eye = x >= 0.25f ? 1 : 0;
    } else if (width > 1.0f && self->____stereoTextureWidth > 0) {
      half = width < self->____stereoTextureWidth * 0.75f;
      eye = x >= self->____stereoTextureWidth * 0.25f ? 1 : 0;
    }
    if (!half) return;
    auto const which = UnityEngine::Camera_StereoscopicEye(eye);
    auto const view = camera->GetStereoViewMatrix(which);
    auto inverse = UnityEngine::Matrix4x4::Inverse(view);
    auto const position4 = inverse.GetColumn(3);
    auto const back = inverse.GetColumn(2);
    auto const up = inverse.GetColumn(1);
    UnityEngine::Vector3 const position(position4.x, position4.y, position4.z);
    auto const rotation = UnityEngine::Quaternion::LookRotation(UnityEngine::Vector3(-back.x, -back.y, -back.z),
                                                                UnityEngine::Vector3(up.x, up.y, up.z));
    void const* level = runtime.GetCurrentBeatmapData();
    if (level != mirrorPoseLevel && eye == 1) {
      mirrorPoseLevel = level;
      float const dx = camPosition.x - position.x, dy = camPosition.y - position.y, dz = camPosition.z - position.z;
      PaperLogger.info("Vivify mirror: eye pose used for the reflection (right eye): the game's was {:.3f} m and "
                       "{:.2f} degrees from the eye's own; rect=({:.2f},{:.2f})",
                       std::sqrt(dx * dx + dy * dy + dz * dz), UnityEngine::Quaternion::Angle(camRotation, rotation),
                       x, width);
    }
    camPosition = position;
    camRotation = rotation;
    camProjectionMatrix = camera->GetStereoProjectionMatrix(which);
  });
  MirrorRendererSO_RenderMirror(self, camPosition, camRotation, camProjectionMatrix, screenRect, planePosition,
                                planeNormal);
}
MAKE_HOOK_MATCH(MirrorRendererSO_PrepareForNextFrame, &GlobalNamespace::MirrorRendererSO::PrepareForNextFrame,
                void, GlobalNamespace::MirrorRendererSO* self) {
  MirrorRendererSO_PrepareForNextFrame(self);
  VivifyPart("MirrorRendererSO.PrepareForNextFrame", [&] {
    for (auto* copy : SeparateEye::MirrorCopies(self)) MirrorRendererSO_PrepareForNextFrame(copy);
  });
}
MAKE_HOOK_MATCH(MirrorRendererSO_OnDisable, &GlobalNamespace::MirrorRendererSO::OnDisable,
                void, GlobalNamespace::MirrorRendererSO* self) {
  VivifyPart("MirrorRendererSO.OnDisable", [&] { SeparateEye::ForgetMirrorCopies(self); });
  MirrorRendererSO_OnDisable(self);
}
MAKE_HOOK_MATCH(MirrorRendererSO_CreateOrUpdateMirrorCamera, &GlobalNamespace::MirrorRendererSO::CreateOrUpdateMirrorCamera,
                void, GlobalNamespace::MirrorRendererSO* self, UnityEngine::Camera* camera, UnityEngine::RenderTexture* texture) {
  MirrorRendererSO_CreateOrUpdateMirrorCamera(self, camera, texture);
  VivifyPart("MirrorRendererSO.CreateOrUpdateMirrorCamera", [&] {
    if (SeparateEye::IsPrivateMirror(self)) Runtime::Instance().ConfigureMirrorCamera(self->____mirrorCamera.unsafePtr());
  });
}
#endif

MAKE_HOOK_MATCH(PauseController_Pause, &GlobalNamespace::PauseController::Pause, void, GlobalNamespace::PauseController* self) {
  VivifyPart("PauseController_Pause", [] { Runtime::Instance().SetPauseMenuActive(true); });
  PauseController_Pause(self);
}

MAKE_HOOK_MATCH(PauseMenuManager_ShowMenu, &GlobalNamespace::PauseMenuManager::ShowMenu, void, GlobalNamespace::PauseMenuManager* self) {
  VivifyPart("PauseMenuManager_ShowMenu", [] { Runtime::Instance().SetPauseMenuActive(true); });
  PauseMenuManager_ShowMenu(self);
}

MAKE_HOOK_MATCH(PauseMenuManager_MenuButtonPressed, &GlobalNamespace::PauseMenuManager::MenuButtonPressed, void, GlobalNamespace::PauseMenuManager* self) {
  VivifyPart("PauseMenuManager_MenuButtonPressed", [] { Runtime::Instance().SetPauseMenuActive(true); });
  PauseMenuManager_MenuButtonPressed(self);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidPressMenuButton, &GlobalNamespace::PauseController::HandlePauseMenuManagerDidPressMenuButton, void, GlobalNamespace::PauseController* self) {
  VivifyPart("PauseController_HandlePauseMenuManagerDidPressMenuButton", [] { Runtime::Instance().SetPauseMenuActive(true); });
  PauseController_HandlePauseMenuManagerDidPressMenuButton(self);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidFinishResumeAnimation, &GlobalNamespace::PauseController::HandlePauseMenuManagerDidFinishResumeAnimation, void, GlobalNamespace::PauseController* self) {
  PauseController_HandlePauseMenuManagerDidFinishResumeAnimation(self);
  VivifyPart("PauseController_HandlePauseMenuManagerDidFinishResumeAnimation", [] { Runtime::Instance().SetPauseMenuActive(false); });
}

MAKE_HOOK_MATCH(GamePause_Resume, &GlobalNamespace::GamePause::Resume, void, GlobalNamespace::GamePause* self) {
  GamePause_Resume(self);
  VivifyPart("GamePause_Resume", [] { Runtime::Instance().SetPauseMenuActive(false); });
}

// Ported from the rbatteries1-design/Lars27110 base. VRCenterAdjust drives the
// automatic room-scale recenter/floor-offset. Its methods touch
// _settingsManager / _settingsApplicator fields that aren't always populated
// this early in scene setup, and Vivify's own world-space cameras/effects are
// positioned relative to room space -- letting a recenter run underneath them
// (or running these methods against a not-yet-initialized instance) can shift
// world-space visuals out of alignment with gameplay. bailing out here (and
// via the "Disable VR Center Adjust Handling" settings toggle) keeps that from
// happening; skipped calls just fall through to Beat Saber's stock behavior on
// the next legitimate call once the fields are populated.
bool ShouldSkipVRCenterAdjust(GlobalNamespace::VRCenterAdjust* self, std::string_view method) {
  if (!IsManagedAlive(self)) return true;
  // Only while a Vivify map plays (0.14.25). Everywhere else -- the menu,
  // ordinary maps -- the game's recentering runs exactly as without Vivify;
  // until now it was skipped there too, whenever its fields were not yet set,
  // which they are not in the menu.
  if (Runtime::Instance().GetCurrentBeatmapData() == nullptr) return false;
  bool const missingSettingsManager = self->____settingsManager == nullptr;
  bool const missingSettingsApplicator = !IsManagedAlive(self->____settingsApplicator.unsafePtr());
  if (GetDisableVRCenterAdjust()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("VRCenterAdjust.{} skipped: isolation toggle enabled", method);
    }
    return true;
  }
  if (missingSettingsManager || missingSettingsApplicator) {
    // Update runs every frame, and with debug logging on this line alone was
    // 21,790 of a 25,005-line session log. Once per method is enough to know.
    static std::set<std::string_view> sReported;
    if (sReported.insert(method).second) {
      PaperLogger.warn("VRCenterAdjust.{} skipped: missing _settingsManager={} _settingsApplicator={}",
                       method, BoolText(missingSettingsManager), BoolText(missingSettingsApplicator));
    }
    return true;
  }
  return false;
}

MAKE_HOOK_MATCH(VRCenterAdjust_Start, &GlobalNamespace::VRCenterAdjust::Start, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "Start")) return;
  VRCenterAdjust_Start(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_OnEnable, &GlobalNamespace::VRCenterAdjust::OnEnable, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "OnEnable")) return;
  VRCenterAdjust_OnEnable(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_OnDisable, &GlobalNamespace::VRCenterAdjust::OnDisable, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "OnDisable")) return;
  VRCenterAdjust_OnDisable(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_Update, &GlobalNamespace::VRCenterAdjust::Update, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "Update")) return;
  VRCenterAdjust_Update(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_ResetRoom, &GlobalNamespace::VRCenterAdjust::ResetRoom, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "ResetRoom")) return;
  VRCenterAdjust_ResetRoom(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_SetRoomTransformOffset, &GlobalNamespace::VRCenterAdjust::SetRoomTransformOffset, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "SetRoomTransformOffset")) return;
  VRCenterAdjust_SetRoomTransformOffset(self);
}

}

namespace Vivify {

void LateLoad() {
  Runtime::Instance().LateLoad();
  INSTALL_HOOK(PaperLogger, SaberModelController_Init);
#ifdef VIVIFY_GAME_1_45_3
  INSTALL_HOOK(PaperLogger, MainEffectRendererFeature_AddRenderPasses);
  INSTALL_HOOK(PaperLogger, MainEffectPostRenderPass_ExecutePyramidBloomPass);
  INSTALL_HOOK(PaperLogger, MainEffectPostRenderPass_ExecuteNoPostProcessPass);
#endif
  INSTALL_HOOK(PaperLogger, GameNoteController_Init);
  INSTALL_HOOK(PaperLogger, BombNoteController_Init);
  INSTALL_HOOK(PaperLogger, BurstSliderGameNoteController_Init);
  INSTALL_HOOK(PaperLogger, NoteCutCoreEffectsSpawner_SpawnNoteCutEffect);
  INSTALL_HOOK(PaperLogger, NoteDebris_Init);
  INSTALL_HOOK(PaperLogger, AlwaysVisibleQuad_OnEnable);
#ifndef VIVIFY_GAME_1_45_3
  INSTALL_HOOK(PaperLogger, ImageEffectController_OnRenderImage);
#endif
#ifndef VIVIFY_GAME_1_45_3
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_RenderMirrorTexture);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_PrepareForNextFrame);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_RenderMirror);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_OnDisable);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_CreateOrUpdateMirrorCamera);
#endif
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_Start);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_OnEnable);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_OnDisable);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_Update);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_ResetRoom);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_SetRoomTransformOffset);
  INSTALL_HOOK(PaperLogger, PauseController_Pause);
  INSTALL_HOOK(PaperLogger, PauseMenuManager_ShowMenu);
  INSTALL_HOOK(PaperLogger, PauseMenuManager_MenuButtonPressed);
  INSTALL_HOOK(PaperLogger, PauseController_HandlePauseMenuManagerDidPressMenuButton);
  INSTALL_HOOK(PaperLogger, PauseController_HandlePauseMenuManagerDidFinishResumeAnimation);
  INSTALL_HOOK(PaperLogger, GamePause_Resume);
}

void RefreshMultipassRendering() {
  Runtime::Instance().RefreshMultipassRendering();
}

void RefreshIsolationSettings() {
  Runtime::Instance().RefreshIsolationSettings();
}

}
