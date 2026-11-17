#include "VivifyRuntimeInternal.hpp"
#include <set>
#include "VivifyComponents.hpp"
#include "VivifySeparateEye.hpp"
#include "VivifySeparateEyePolicy.hpp"
#include "VivifyCameraBindings.hpp"
#include "GlobalNamespace/MirrorRendererSO.hpp"
#include "GlobalNamespace/UserInfo.hpp"

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

MAKE_HOOK_MATCH(SaberModelController_Init, &GlobalNamespace::SaberModelController::Init, void, GlobalNamespace::SaberModelController* self, UnityEngine::Transform* parent, GlobalNamespace::Saber* saber, UnityEngine::Color trailTintColor) {
  SaberModelController_Init(self, parent, saber, trailTintColor);
  VivifyPart("SaberModelController.Init", [&] { Runtime::Instance().TrackSaberModel(self, saber, parent); });
}

MAKE_HOOK_MATCH(GameNoteController_Init, &GlobalNamespace::GameNoteController::Init, void, GlobalNamespace::GameNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float cutAngleTolerance, float uniformScale) {
  GameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, cutAngleTolerance, uniformScale);
  VivifyPart("GameNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(BombNoteController_Init, &GlobalNamespace::BombNoteController::Init, void, GlobalNamespace::BombNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData) {
  BombNoteController_Init(self, noteData, noteSpawnData);
  VivifyPart("BombNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(BurstSliderGameNoteController_Init, &GlobalNamespace::BurstSliderGameNoteController::Init, void, GlobalNamespace::BurstSliderGameNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float uniformScale) {
  BurstSliderGameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, uniformScale);
  VivifyPart("BurstSliderGameNoteController.Init", [&] { Runtime::Instance().ApplyNotePrefabFor(self); });
}

MAKE_HOOK_MATCH(NoteCutCoreEffectsSpawner_SpawnNoteCutEffect, &GlobalNamespace::NoteCutCoreEffectsSpawner::SpawnNoteCutEffect, void, GlobalNamespace::NoteCutCoreEffectsSpawner* self, ByRef<GlobalNamespace::NoteCutInfo> noteCutInfo, GlobalNamespace::NoteController* noteController, int32_t sparkleParticlesCount, int32_t explosionParticlesCount) {

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

MAKE_HOOK_MATCH(ImageEffectController_OnRenderImage, &GlobalNamespace::ImageEffectController::OnRenderImage, void, GlobalNamespace::ImageEffectController* self, UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  bool bypass = false;
  VivifyPart("ImageEffectController.OnRenderImage", [&] { bypass = Runtime::Instance().ShouldBypassImageEffect(self); });
  if (bypass) {
    UnityEngine::Graphics::Blit(static_cast<UnityEngine::Texture*>(src), dest);
    return;
  }
  ImageEffectController_OnRenderImage(self, src, dest);
}

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
  INSTALL_HOOK(PaperLogger, GameNoteController_Init);
  INSTALL_HOOK(PaperLogger, BombNoteController_Init);
  INSTALL_HOOK(PaperLogger, BurstSliderGameNoteController_Init);
  INSTALL_HOOK(PaperLogger, NoteCutCoreEffectsSpawner_SpawnNoteCutEffect);
  INSTALL_HOOK(PaperLogger, NoteDebris_Init);
  INSTALL_HOOK(PaperLogger, AlwaysVisibleQuad_OnEnable);
  INSTALL_HOOK(PaperLogger, ImageEffectController_OnRenderImage);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_RenderMirrorTexture);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_PrepareForNextFrame);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_OnDisable);
  INSTALL_HOOK(PaperLogger, MirrorRendererSO_CreateOrUpdateMirrorCamera);
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
