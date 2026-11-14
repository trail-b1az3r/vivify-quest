#pragma once
#include <string>
#include <chrono>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <optional>
#include <mutex>
#include "VivifyTypes.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include <algorithm>
#include "UnityEngine/ReflectionProbe.hpp"
#include "UnityEngine/Rendering/CommandBuffer.hpp"
#include "UnityEngine/AssetBundleCreateRequest.hpp"
#include "UnityEngine/AssetBundleRequest.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"

namespace Vivify {
// One frustum holding both eyes' views: the widest left and right, up and
// down tangents of the two projections, around the head (0.15.16). 0.15.13
// used the left eye's projection widened a fixed quarter, which is skewed to
// the left and could cut off the right edge of the right eye.
inline UnityEngine::Matrix4x4 BothEyesCulling(UnityEngine::Matrix4x4 const& left, UnityEngine::Matrix4x4 const& right) {
  auto tangents = [](UnityEngine::Matrix4x4 const& m, float& l, float& r, float& b, float& t) {
    l = (m.m02 - 1.0f) / m.m00;
    r = (m.m02 + 1.0f) / m.m00;
    b = (m.m12 - 1.0f) / m.m11;
    t = (m.m12 + 1.0f) / m.m11;
  };
  float l0, r0, b0, t0, l1, r1, b1, t1;
  tangents(left, l0, r0, b0, t0);
  tangents(right, l1, r1, b1, t1);
  // A little extra for the eyes sitting either side of the head's centre.
  float const margin = 0.05f;
  float const l = std::min(l0, l1) - margin, r = std::max(r0, r1) + margin;
  float const b = std::min(b0, b1) - margin, t = std::max(t0, t1) + margin;
  UnityEngine::Matrix4x4 m = left;
  m.m00 = 2.0f / (r - l);
  m.m01 = 0.0f;
  m.m02 = (r + l) / (r - l);
  m.m10 = 0.0f;
  m.m11 = 2.0f / (t - b);
  m.m12 = (t + b) / (t - b);
  return m;
}
}  // namespace Vivify

namespace Vivify {
struct FollowedSaberTrail;
struct RuntimeBehaviour;
struct CullingCameraController;
struct MultipassKeywordController;
struct CameraApplier;

class Runtime {
public:
  // Runs bundle selection again for the selected map (its Windows-bundle
  // choice changed in the menu). Not during a level.
  void ReselectCurrentLevel();
  std::string const& SelectedLevelPath() const { return _selectedLevelPath; }
  bool SelectedMapHasVivify() const { return _selectedMapHasVivifyRequirement; }
  static Runtime& Instance();

  CustomJSONData::CustomBeatmapData* GetCurrentBeatmapData() const { return _currentBeatmapData; }
  bool IsResetting() const { return _isResetting; }
  bool IsPauseMenuActive() const { return _pauseMenuActive; }
  void ValidateFixedMaterialFeatures();
  bool HasSecondaryCameras() const { return !_secondaryCameras.empty(); }
  bool IsMultiplayerGameplayForEyeRenderer() { return IsInMultiplayerGameplay(); }
  bool GetPreEffectsEmpty() const { return _preEffects.empty(); }
  bool GetPostEffectsEmpty() const { return _postEffects.empty(); }
  // Replaying effects twice must not advance shared render-texture state twice.
  bool HasSharedEyeEffectTargets() const {
    for (auto const* effects : {&_preEffects, &_postEffects}) for (auto const& effect : *effects) {
      if (effect.data.source != kMainCameraId) return true;
      for (auto const& target : effect.data.targets) if (target != kMainCameraId) return true;
    }
    return false;
  }

  void AddMidRenderCommandBuffers(UnityEngine::Camera* camera);
  void RemoveMidRenderCommandBuffers();
  void CacheMainRenderDescriptor(UnityEngine::RenderTexture* src);
  bool HasMidRenderEffects() const {
    for (auto const& bucket : _midEffects) {
      if (!bucket.empty()) return true;
    }
    return false;
  }

  void LateLoad();
  void Update();
  void OnBehaviourDestroyed(RuntimeBehaviour* behaviour);
  // Advances an in-flight blit companion load; once a frame, menu included.
  void PollBlitCompanion();
  void PollBlitCompanionPrefabs();
  void FinishBlitCompanion(bool current);
  // Advances "Prepare Shaders Before Playing" (0.14.15): compiles a converted
  // bundle's shaders in the menu, a few materials a frame. Once a frame.
  void PollShaderWarmup();
  // A command buffer for immediate execution (Graphics::ExecuteCommandBuffer),
  // cleared and handed out again on every call. One shared buffer instead of a
  // new one per blit per frame: every allocation was managed garbage, and the
  // collections that follow are frame hitches (0.14.25). Fill it, execute it,
  // and do not keep it.
  UnityEngine::Rendering::CommandBuffer* ImmediateCommandBuffer();
  // Starts a background scan of a bundle for shaders that read the scene
  // depth texture, unless one ran for this version of the file.
  void ScanBundleForSceneDepth(std::string const& bundlePath);
  // Whether the loaded map's shaders read the scene depth texture: empty
  // until every bundle they come from has been scanned.
  std::optional<bool> MapShadersNeedSceneDepth();
  void ApplyFarCulling(UnityEngine::Camera* mainCam);
  void RestoreFarCulling(UnityEngine::Camera* mainCam);
  // Unloads the loaded bundle and forgets every per-level bundle choice, so
  // "Reset Vivify (clean slate)" can delete the files and the next level
  // selection starts from nothing.
  void ReleaseBundlesForReset();
  void SetPauseMenuActive(bool active);
  void RefreshMultipassRendering();
  void RefreshIsolationSettings();

  void RegisterSyncedObject(UnityEngine::GameObject* root, float startTime);
  void UnregisterSyncedObject(UnityEngine::GameObject* root);

  std::vector<AssignedPrefabInfo*> FindAssignedPrefabs(std::string_view objectType,
                                                       GlobalNamespace::NoteData* noteData,
                                                       AssignedPrefabKind kind);
  std::vector<AssignedPrefabInfo*> FindAssignedSaberPrefabs(int type);
  std::vector<AssignedPrefabInfo*> FindAssignedSaberTrailPrefabs(int type);
  std::vector<AssignedPrefabInfo*> FindAssignedDebrisPrefabs(GlobalNamespace::NoteData* noteData);

  bool IsReduceDebrisEnabled();
  void PushActiveDebrisPrefabs(std::vector<AssignedPrefabInfo*> infos);
  void PopActiveDebrisPrefabs();
  void ApplyNotePrefabFor(GlobalNamespace::NoteController* noteController, bool isFreshInit = true);
  void RestoreNoteVisuals(GlobalNamespace::NoteController* noteController);
  void RestoreDebrisVisuals(GlobalNamespace::NoteDebris* debris);
  void RestoreSaberVisuals(GlobalNamespace::SaberModelController* smc);
  void ReplaceNoteVisuals(GlobalNamespace::NoteController* noteController,
                          std::vector<AssignedPrefabInfo*> const& infos);
  void TrackSaberModel(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                       UnityEngine::Transform* parent);
  void ApplySaberVisualsToActive();
  void ApplySaberVisuals(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                         UnityEngine::Transform* parent);
  void ReplaceDebrisVisuals(GlobalNamespace::NoteDebris* debris);

  // phase: -1 passthrough, 0 all, 1 pre-main-effect, 2 post-main-effect.
  void ApplyBlits(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest,
                  std::string const& cameraName = "_Main", int phase = 0);

  bool ShouldBypassImageEffect(GlobalNamespace::ImageEffectController* controller);
  bool CanUseNativeStereoEffects() const;

  bool RenderSecondaryCameraMainEffect(UnityEngine::Camera* camera, UnityEngine::RenderTexture* src,
                                       UnityEngine::RenderTexture* dest);
  void CaptureSecondaryDepth(UnityEngine::RenderTexture* destination);
  void ConfigureMirrorCamera(UnityEngine::Camera* camera);

  void BindSecondaryCameraTextures();
  void RestoreSecondaryCullingLayers();

private:
  friend struct MenuDiagnostics;
  friend struct PlaybackDiagnostics;
  Runtime() = default;

  static void OnCustomEventStatic(GlobalNamespace::BeatmapCallbacksController* callbackController,
                                  CustomJSONData::CustomEventData* customEventData);
  void EnsureBehaviour();
  // reason ends up verbatim in VivifyReport.txt, so callers say what happened.
  void ResetRuntime(std::string_view reason = "level ended (quit or finished)");
  float CurrentSongTime();
  void DetectSongRestart();
  float CurrentBpm() const;
  float DurationBeatsToSeconds(float durationBeats) const;
  bool IsAlive(UnityEngine::Object* object) const;
  bool IsMultiplayerModLoaded() const;
  bool IsInMultiplayerGameplay();
  bool ShouldDisableVisualsForMultiplayer();
  void UpdateSyncedObjects();

  void UpdatePrefabAnimationSpeed();
  float SongSpeed();
  void LogThrottledUpdateError(std::string_view what);

  void HandleLevelSelected(SongCore::API::LevelSelect::LevelWasSelectedEventArgs const& event);
  void SelectBundleForLevel();
  void RunScheduledVisualChecks();
  void DisableRealtimeShadowsForLevel();
  void DisableRealtimeReflectionProbesForLevel();
  // Crash recovery (0.15.2), see VivifyRecovery.hpp.
  void EnterRecoveryModeIfNeeded();
  bool IsRecoverySkippedBlit(std::string const& normalizedAsset) const;
  void RunVisualCheck(std::string_view when);
  void NoteVisualProblem(std::string problem);
  void DownloadBundle(uint32_t checksum, std::string const& levelPath, std::function<void(bool)> callback);
  // The same, saving to destPath instead of the level's Android bundle path.
  void DownloadBundleTo(uint32_t checksum, std::string const& destPath, std::function<void(bool)> callback);
  // Downloads a Quest-less Vivify map's PC bundle and converts it; false when
  // Info.dat names none.
  bool TryDownloadPcBundle(std::string const& levelPath);
  // Loads a level's Android bundle, first standing in the PC build of any of
  // its shaders that were shipped empty (VivifyAssets.cpp, "PC shaders for
  // empty Quest shaders").
  void BeginAndroidBundleLoad(std::string const& levelPath, std::string const& androidBundlePath);
  void FinishAndroidBundleLoad(std::string const& levelPath, std::string const& androidBundlePath);
  void GraftShadersFrom(std::string const& levelPath, std::string const& convertedPath,
                        std::vector<std::string> const& names);
  void ConvertPcForGraft(std::string const& levelPath, std::string const& androidBundlePath,
                         std::string const& pcBundlePath, std::vector<std::string> const& names);
  // Blit companion (0.14.10): the single-view build of a converted map's
  // shaders, for blits into single-screen textures, which the multiview
  // programs of the main conversion cannot draw into.
  void PrepareBlitCompanion(std::string const& levelPath, std::string const& pcBundlePath);
  void LoadBlitCompanion(std::string const& levelPath, std::string const& companionPath, bool wantsPrefabs,
                         uint64_t generation);
  void ClearBlitCompanion(bool resetSeparateEye = true);
  // The material a blit into `destination` should draw with: `material`, or
  // for a single-screen destination a copy of it on the single-view shader.
  UnityEngine::Material* BlitMaterialFor(UnityEngine::Material* material, UnityEngine::RenderTexture* destination);
  // Kicks off an off-thread PC->Android bundle conversion for the currently
  // selected level and re-enables the play button when it lands.
  void ConvertPcBundleAsync(std::string const& levelPath, std::string const& sourceBundlePath);
  void BeginBundleDownload(uint32_t checksum, std::string const& levelPath, std::string const& pcBundleFallback);
  // Abandons any in-flight asset download so its callback becomes a no-op.
  void CancelPendingDownload();
  // Gives up on a download that never called back and falls back to conversion.
  void CheckDownloadTimeout();
  void PreloadBundle(std::string const& bundlePath);
  // Crash guard for on-device converted bundles: a marker file beside the
  // bundle while it loads and plays its first seconds (see VivifyAssets.cpp).
  void ArmLoadGuard(std::string const& bundlePath);
  void DisarmLoadGuard();
  void LoadMainBundle();
  void CacheBundleAssets();
  // True once the watchdog has stood Vivify down for this level.
  bool SelfDisabled() const { return _selfDisabledThisLevel; }
  // Formats everything gathered about the current level for VivifyReport.txt.
  std::string BuildLevelReport(std::string_view outcome) const;
  void WriteLevelStartReport();
  void WriteLevelEndReport(std::string_view outcome);
  // Reports, per bundle, how many shaders can actually draw here and why the
  // rest cannot -- separating "no Android program was ever compiled" from
  // "this GPU refuses every subshader", which need completely different fixes.
  void LogBundleShaderAudit(int seen, int runnable, int noProgram, int deviceRejected,
                            std::vector<std::string> const& deviceRejectedNames);
  UnityEngine::Object* GetAssetObject(std::string_view assetName) const;
  void RetainBundleAsset(UnityEngine::Object* asset);
  void ReleaseRetainedBundleAssets();
  template <typename T>
  T* GetAssetAs(std::string_view assetName) const {
    auto* asset = GetAssetObject(assetName);
    if (!IsManagedAlive(asset)) {
      return nullptr;
    }
    return il2cpp_utils::try_cast<T>(asset).value_or(nullptr);
  }
  void LogUnityPlatformInfoOnce();
  UnityEngine::RenderTextureFormat SupportedRenderTextureFormat(UnityEngine::RenderTextureFormat requested,
                                                                std::string_view context) const;
  void LogMaterialShader(std::string_view context, std::string_view assetPath, UnityEngine::Material* material) const;
  // Asset lookup without the miss warning, for searches whose misses are normal.
  UnityEngine::Object* LookUpAsset(std::string_view assetName) const;
  UnityEngine::Shader* FindUsableShader(std::string const& shaderName);
  // Builds the name -> runnable shader index the lookup above depends on.
  void EnsureGameShaderIndex();
  UnityEngine::Shader* FindFallbackShader();
  // Returns a GPU-usable stand-in for a block-compressed texture this device
  // cannot sample, decoding it once and caching the result.
  UnityEngine::Texture* ResolveUsableTexture(UnityEngine::Texture* texture);
  void DecodeUnsupportedBundleTextures();
  void RepairMaterialShader(UnityEngine::Material* material, std::string_view context = {});
  void RepairGameObjectMaterials(UnityEngine::GameObject* gameObject, std::string_view context = {});
  void ApplyStereoKeywords(UnityEngine::Material* material) const;
  void ApplyGameObjectStereoKeywords(UnityEngine::GameObject* gameObject);
  void RefreshLoadedMaterialStereoKeywords();
  void RepairLoadedMaterialShaders();
  void SetMaterialKeyword(UnityEngine::Material* material, ::StringW keyword, bool enabled) const;

  void HandleCustomEvent(GlobalNamespace::BeatmapCallbacksController* callbackController,
                         CustomJSONData::CustomEventData* customEventData);
  void DispatchEvent(std::string_view type, CustomJSONData::CustomEventData* customEventData,
                     rapidjson::Value const& json);
  CustomJSONData::CustomBeatmapData* GetCustomBeatmapData(GlobalNamespace::BeatmapCallbacksController* callbackController);
  bool EnsureBeatmapPrepared(GlobalNamespace::BeatmapCallbacksController* callbackController, float triggerTime = 0.0f);
  void PrepareBeatmap(CustomJSONData::CustomBeatmapData* beatmapData, float triggerTime = 0.0f);

  void ApplyMissedVivifyEvents(float upToTime);
  rapidjson::Value const* GetEventJson(CustomJSONData::CustomEventData* customEventData) const;
  std::optional<PointDefinitionW> MakePointDefinition(rapidjson::Value const& object,
                                                      std::string_view key,
                                                      Tracks::ffi::WrapBaseValueType type);
  std::vector<TrackW> ReadTracks(rapidjson::Value const& object, bool v2);

  std::optional<MaterialPropertyChange> ParseMaterialProperty(rapidjson::Value const& propertyJson);
  std::vector<MaterialPropertyChange> ParseMaterialProperties(rapidjson::Value const& json);
  std::optional<AnimatorPropertyChange> ParseAnimatorProperty(rapidjson::Value const& propertyJson);
  std::vector<AnimatorPropertyChange> ParseAnimatorProperties(rapidjson::Value const& json);
  bool IsAnimated(MaterialPropertyChange const& property) const;
  bool IsAnimated(AnimatorPropertyChange const& property) const;
  void ApplyMaterialProperty(UnityEngine::Material* material, MaterialPropertyChange const& property, float progress);
  void SaveOriginalGlobalProperty(MaterialPropertyChange const& property);
  void ApplyGlobalProperty(MaterialPropertyChange const& property, float progress);
  void ApplyAnimatorProperty(std::vector<UnityEngine::Animator*> const& animators,
                             AnimatorPropertyChange const& property, float progress);
  void HandleSetMaterialProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  void HandleSetAnimatorProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  void HandleSetGlobalProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  float AnimationProgress(float startTime, float duration, Functions easing, float songTime) const;
  void UpdateMaterialAnimations();
  void UpdateGlobalAnimations();
  void UpdateAnimatorAnimations();
  void RestoreGlobalProperties();
  void ReapplyCurrentGlobalProperties();
  void QueueMaterialPropertyAnimation(UnityEngine::Material* material,
                                      std::vector<MaterialPropertyChange> const& properties,
                                      float startTime, float duration, Functions easing);

  void HandleSetRenderingSettings(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  void SaveRenderSetting(std::string const& name, RenderSettingKind kind);
  void ApplyRenderSettingFloat(std::string const& name, float val);
  void ApplyRenderSettingColor(std::string const& name, UnityEngine::Color val);
  void ApplyRenderSettingBool(std::string const& name, bool val);
  void ApplyRenderSettingInt(std::string const& name, int val);
  void ApplyRenderSettingObject(std::string const& name, std::string const& assetOrId);
  void ParseAndApplyRenderSetting(std::string const& key, rapidjson::Value const& parent,
                                  rapidjson::Value const& val,
                                  std::vector<RenderSettingValue>& out, bool immediate);
  void UpdateRenderSettingAnimations();
  void RestoreRenderSettings();
  void ReapplyCurrentRenderSettings();

  bool SameBlitData(BlitMaterialData const& left, BlitMaterialData const& right);
  std::optional<std::string_view> ReadBlitAssetName(rapidjson::Value const& json);
  void RemoveMatchingBlitEffects(std::vector<ActiveBlitEffect>& effects, rapidjson::Value const& json,
                                 BlitMaterialData const& filter, bool filterAsset);
  PostProcessingOrder ParsePostProcessingOrder(rapidjson::Value const& json);
  void AddOrUpdateBlitEffect(std::vector<ActiveBlitEffect>& effects, ActiveBlitEffect effect);
  void HandleBlit(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  void UpdateBlitEffects();
  void HandleCreateScreenTexture(rapidjson::Value const& json);
  void PrepareDeclaredTextures();
  bool TryApplyStereoBlits(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest,
                           std::string const& cameraName, int phase);
  void HandleCreateCamera(rapidjson::Value const& json);
  std::optional<UnityEngine::DepthTextureMode> ParseDepthTextureModeName(std::string_view s);
  std::optional<UnityEngine::DepthTextureMode> ParseDepthTextureModeValue(rapidjson::Value const& value);
  void ParseCameraPropertyData(rapidjson::Value const& json, CameraPropertyData& out);
  void ApplyCameraProperties(UnityEngine::Camera* camera, CameraPropertyData const& props);
  void ApplyCameraGameObjectProperties(UnityEngine::Camera* camera, CameraPropertyData const& props);
  void HandleSetCameraProperty(rapidjson::Value const& json);
  void CaptureMainCameraOriginals(UnityEngine::Camera* mainCam, UnityEngine::GameObject* mainCamGO);
  void RestoreMainCameraOriginals(bool restoreAuxiliary = true);
  struct CameraDefaults {
    SafePtr<UnityEngine::Camera> camera;
    int instanceId = 0, depthMode = 0;
    UnityEngine::CameraClearFlags clearFlags;
    UnityEngine::Color backgroundColor;
  };
  std::unordered_map<UnityEngine::Camera*, CameraDefaults> _cameraDefaults;
  UnityEngine::Rendering::CommandBuffer* BuildMidRenderCommandBuffer(std::vector<ActiveBlitEffect> const& effects,
                                                                     int bucket);
  bool EnsureMidRenderTextures();
  void ReleaseMidRenderTextures();
  UnityEngine::RenderTexture* EnsureCachedBlitTexture(UnityEngine::RenderTexture*& texture,
                                                      std::optional<RenderTexturePolicy::Descriptor>& allocation,
                                                      UnityEngine::RenderTexture* src);
  void ReleaseRenderTexture(UnityEngine::RenderTexture*& texture);
  void ReleaseCachedBlitTextures();
  void RefreshCameraComponents(bool allowCameraApplier);
  void RefreshCameraApplier(UnityEngine::GameObject* mainCamGO, bool allowCameraApplier);
  void DestroyCameraApplier();
  void RefreshMultipassRendering(UnityEngine::GameObject* mainCamGO);
  MultipassKeywordController* EnsureMultipassKeywordController(UnityEngine::GameObject* gameObject);
  CullingCameraController* EnsureCullingController(UnityEngine::GameObject* gameObject);
  void ApplyCullingProperty(UnityEngine::GameObject* cameraGO, std::optional<CullingMaskData> const& culling);
  void ReleaseDeclaredTextureData(DeclaredTextureData& data);
  void ReleaseSecondaryCameraData(SecondaryCameraData& data);
  UnityEngine::RenderTexture* SecondaryCameraColorRT(SecondaryCameraData const& cam);
  bool DestroyDeclaredTextureById(std::string const& id);
  bool DestroySecondaryCameraById(std::string const& id);
  bool CanUseBlitMaterial(UnityEngine::Material* material, int pass) const;

  void CleanCustomObject(UnityEngine::GameObject* go);
  void PreloadInstantiatePrefabs();
  std::string GetPrefabStorageId(CustomJSONData::CustomEventData* customEventData,
                                 InstantiatePrefabData const& data) const;
  void InstantiatePrefab(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);
  bool DestroyPrefabById(std::string const& id);
  void DestroyObjects(rapidjson::Value const& json);
  void HandleAssignObjectPrefab(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json);

  void RefreshActiveNoteVisuals();
  std::string ComputePrefabFingerprint(std::vector<AssignedPrefabInfo*> const& infos) const;
  std::string ComputeSaberFingerprint(std::vector<AssignedPrefabInfo*> const& modelInfos,
                                      std::vector<AssignedPrefabInfo*> const& trailInfos) const;
  bool ReplacementIntact(VisualReplacement const& replacement) const;
  // True if any spawned renderer would actually draw, i.e. has a material
  // whose shader this GPU can run.
  bool ReplacementCanRender(VisualReplacement const& replacement) const;
  void ReassertNoteReplacement(GlobalNamespace::NoteController* noteController, VisualReplacement& replacement);
  bool AssignmentMatchesTracks(AssignedPrefabInfo const& info, GlobalNamespace::NoteData* noteData);
  void AddAssignedPrefab(std::string_view objectType, AssignedPrefabKind kind, std::string asset,
                         std::vector<TrackW> tracks, bool additive, std::optional<int> saberType = std::nullopt);
  void AddAssignedTrail(std::string asset, bool additive, int saberType,
                        std::optional<UnityEngine::Vector3> topPos, std::optional<UnityEngine::Vector3> bottomPos,
                        std::optional<float> duration, std::optional<int> samplingFrequency,
                        std::optional<int> granularity);
  bool TracksOverlap(std::vector<TrackW> const& left, std::vector<TrackW> const& right) const;
  void ClearAssignedPrefabs(std::string_view objectType, std::optional<AssignedPrefabKind> kind = std::nullopt,
                            std::optional<int> saberType = std::nullopt, std::vector<TrackW> const* tracks = nullptr);
  std::vector<AssignedPrefabInfo*> GetValidPrefabInfos(std::vector<AssignedPrefabInfo*> const& infos);
  // Classifies a replacement prefab asset once and remembers the answer, so a
  // prefab that cannot draw is not instantiated once per note for a whole song.
  PrefabRenderability EvaluatePrefabRenderability(std::string_view asset);
  bool ShouldHideOriginal(std::vector<AssignedPrefabInfo*> const& infos) const;
  void DisableOriginalRenderers(UnityEngine::GameObject* gameObject, VisualReplacement& replacement);
  void DisableOriginalRenderers(ArrayW<UnityEngine::Renderer*, Array<UnityEngine::Renderer*>*> const& renderers,
                                VisualReplacement& replacement);
  UnityEngine::Transform* GetReplacementParent(GlobalNamespace::NoteController* noteController);
  GlobalNamespace::MaterialPropertyBlockController* GetReplacementMaterialPropertyBlockController(
      GlobalNamespace::NoteController* noteController, UnityEngine::Transform* replacementParent);
  void RestoreReplacementData(VisualReplacement& replacement);
  void CacheReplacementRenderers(UnityEngine::GameObject* spawned, VisualReplacement& replacement);
  void InstantiateReplacementPrefab(AssignedPrefabInfo const& info, UnityEngine::Transform* parent,
                                    VisualReplacement& replacement, bool inheritParentLayer = false);
  UnityEngine::Color GetSaberColor(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber);
  UnityEngine::Color GetNoteColor(GlobalNamespace::NoteController* noteController);
  void ApplyColorToRenderers(std::vector<UnityEngine::Renderer*> const& renderers, UnityEngine::Color color);
  void ApplySaberReplacementColor(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                                  VisualReplacement& replacement, bool force = false);
  void UpdateSaberReplacementColors();
  void HideOtherSaberRenderers(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                               VisualReplacement& replacement);
  void DiscoverSabers();
  void HideForeignSaberLights();
  void RestoreForeignSaberLights();
  void ApplyFieldOfView(UnityEngine::Camera* mainCam);
  void RestoreFieldOfView();
  UnityEngine::Camera* _fovCamera = nullptr;
  int _fovAppliedPercent = 0;
  UnityEngine::Matrix4x4 _fovLeftProjection{};
  // What was set last frame, to tell whether the headset kept it (0.15.16).
  float _fovSetM00[2] = {0.0f, 0.0f};
  int _fovFramesChecked = 0;
  bool _fovReported = false;
  void ApplyVivifyFoveation();
  void RestoreVivifyFoveation();
  bool _foveationApplied = false;
  bool _foveationEyeTracked = false;
  bool _eyeTrackedBefore = false;
  int _foveationBefore = 0;
  std::vector<UnityEngine::Light*> _disabledSaberLights;
  // The game's mirrors switched off for a map with Game Mirrors off (0.15.18).
  std::vector<UnityEngine::Renderer*> _hiddenMirrorRenderers;
  float _lastMirrorSweep = -1.0f;
  void HideGameMirrors();
  void RestoreGameMirrors();
  float _lastSaberLightSweep = 0.0f;
  void NoteReflectionProbes(UnityEngine::GameObject* root);
  void RefreshSlowProbes();
  void OnAnyCameraPreCull(UnityEngine::Camera* camera);
  void OnAnyCameraPostRender(UnityEngine::Camera* camera);
  void InstallCameraCallbacks();
  std::vector<UnityEngine::ReflectionProbe*> _slowProbes;
  float _lastSlowProbeRefresh = 0.0f;
  bool _probeCameraRendering = false;
  void HideOtherSaberTrails(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                            VisualReplacement& replacement);
  // Whether the loaded bundle is a PC bundle converted on the headset.
  bool LoadedBundleIsConverted() const;
  bool EnsureMapSaberVisible(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                             VisualReplacement& replacement);
  void NoteHeadsetLayers(UnityEngine::GameObject* root, std::string const& asset);
  // Fall Away's head-following HUD (layer 17), drawn nearer the eyes
  // (0.15.17): see PullHeadsetHud.
  struct HudObject {
    UnityEngine::Transform* transform = nullptr;
    UnityEngine::Vector3 setPosition{};
    UnityEngine::Vector3 originalPosition{};
    UnityEngine::Vector3 setScale{};
    UnityEngine::Vector3 originalScale{};
    bool applied = false;
  };
  std::vector<HudObject> _hudObjects;
  void PullHeadsetHud(UnityEngine::Camera* camera);
  // Headset-only layers this level's prefabs use, and those added to the main
  // camera's culling mask for it.
  uint32_t _mapHeadsetLayers = 0;
  uint32_t _addedMainCamLayers = 0;
  float _lastSaberHideSweep = 0.0f;
  GlobalNamespace::SaberTrailRenderer* FindSaberTrailRendererPrefab(GlobalNamespace::SaberModelController* smc);
  void ApplySaberTrailVisuals(GlobalNamespace::SaberModelController* smc, GlobalNamespace::Saber* saber,
                              std::vector<AssignedPrefabInfo*> const& infos, VisualReplacement& replacement);
  void ApplyReplacementRenderersToMaterialBlock(GlobalNamespace::MaterialPropertyBlockController* mpb,
                                                VisualReplacement& replacement, bool hideOriginal,
                                                std::optional<UnityEngine::Color> fallbackColor = std::nullopt);
  void ApplyReplacementRenderersToMaterialBlock(UnityEngine::GameObject* gameObject,
                                                VisualReplacement& replacement, bool hideOriginal,
                                                std::optional<UnityEngine::Color> fallbackColor = std::nullopt);
  void SyncReplacementTintColors();
  bool UsesStandInShading(std::vector<UnityEngine::Renderer*> const& renderers) const;
  void RestoreAllVisualReplacements();
  void PurgeInvalidActiveSabers();
  void ForceGameObjectRenderersOnTop(UnityEngine::GameObject* gameObject);
  void ForceRendererOnTop(UnityEngine::Renderer* renderer);
  void RestoreOverlayRenderState();
  void SetLayerRecursively(UnityEngine::GameObject* gameObject, int layer);

  RuntimeBehaviour* _behaviour = nullptr;
  CustomJSONData::CustomBeatmapData* _currentBeatmapData = nullptr;
  GlobalNamespace::AudioTimeSyncController* _audioTimeSyncController = nullptr;
  TracksAD::BeatmapAssociatedData* _beatmapAD = nullptr;
  TracksAD::BeatmapAssociatedData _fallbackBeatmapAD;
  UnityEngine::AssetBundle* _mainBundle = nullptr;
  std::string _selectedLevelPath;
  std::string _preloadedBundlePath;
  bool _selectedMapHasVivifyRequirement = false;
  std::unordered_map<std::string, UnityEngine::Object*> _assets;
  std::unordered_map<std::string, UnityEngine::Object*> _assetsByName;
  struct RetainedBundleAsset {
    SafePtr<UnityEngine::Object> object;
    int originalHideFlags;
  };
  // Native caches are invisible to Unity's unused-resource scan. Keep assets
  // needed by later events alive until this selection is released, including
  // materials that no currently instantiated prefab references.
  std::unordered_map<UnityEngine::Object*, RetainedBundleAsset> _retainedBundleAssets;
  std::unordered_map<std::string, UnityEngine::Shader*> _supportedShadersByName;
  // Every runnable shader in the process, by name. Rebuilt per level, because
  // the objects behind it are Unity's to destroy.
  std::unordered_map<std::string, UnityEngine::Shader*> _gameShadersByName;
  bool _gameShaderIndexBuilt = false;
  std::unordered_map<CustomJSONData::CustomEventData*, InstantiatePrefabData> _instantiatePrefabs;
  std::unordered_map<std::string, LivePrefab> _livePrefabs;
  std::vector<SyncedObject> _syncedObjects;
  std::vector<ActiveMaterialAnimation> _materialAnimations;
  std::vector<ActiveGlobalAnimation> _globalAnimations;
  std::vector<ActiveAnimatorAnimation> _animatorAnimations;

  std::vector<ActiveMaterialAnimation> _pausedMaterialAnimations;
  std::vector<ActiveGlobalAnimation> _pausedGlobalAnimations;
  std::vector<ActiveAnimatorAnimation> _pausedAnimatorAnimations;
  std::vector<ActiveRenderSettingAnimation> _pausedRenderSettingAnimations;
  std::unordered_map<int, SavedGlobalValue> _savedGlobalProperties;
  std::unordered_map<std::string, bool> _savedGlobalKeywords;

  std::unordered_map<int, SavedGlobalValue> _currentGlobalProperties;
  std::unordered_map<std::string, bool> _currentGlobalKeywords;
  std::unordered_set<UnityEngine::Material*> _repairedMaterials;
  std::unordered_map<std::string, PrefabRenderability> _prefabRenderability;
  std::unordered_map<UnityEngine::Renderer*, int> _overlayRendererSortingOrders;
  std::unordered_map<std::string, DeclaredTextureData> _declaredTextures;
  std::unordered_map<std::string, SecondaryCameraData> _secondaryCameras;
  std::unordered_map<std::string, CameraPropertyData> _cameraProperties;
  std::vector<ActiveBlitEffect> _preEffects;
  std::vector<ActiveBlitEffect> _postEffects;

  std::array<std::vector<ActiveBlitEffect>, kMidRenderOrderCount> _midEffects;

  std::vector<std::pair<int, SafePtr<UnityEngine::Rendering::CommandBuffer>>> _activeMidCommandBuffers;
  // Reused buffers (see ImmediateCommandBuffer): one for immediate blits, one
  // per mid-render bucket, which is attached to the camera for a frame at most.
  SafePtr<UnityEngine::Rendering::CommandBuffer> _immediateCommandBuffer;
  std::array<SafePtr<UnityEngine::Rendering::CommandBuffer>, kMidRenderOrderCount> _midCommandBufferPool;
  UnityEngine::Camera* _midCommandBufferCamera = nullptr;

  UnityEngine::RenderTextureDescriptor _cachedMainDescriptor{};

  bool _hasMainDescriptor = false;

  UnityEngine::RenderTexture* _midMainRT = nullptr;
  UnityEngine::RenderTexture* _midScratchRT = nullptr;
  std::optional<RenderTexturePolicy::Descriptor> _midAllocation;

  std::vector<UnityEngine::RenderTexture*> _midSelfBlitTemps;
  std::vector<ActiveRenderSettingAnimation> _renderSettingAnimations;
  std::vector<SavedRenderSetting> _savedRenderSettings;
  std::vector<SavedRenderSetting> _currentRenderSettings;
  std::vector<AssignedPrefabInfo> _assignedPrefabs;
  std::vector<std::vector<AssignedPrefabInfo*>> _activeDebrisPrefabStack;
  std::vector<AssignedPrefabInfo*> _lastCutDebrisPrefabs;
  std::vector<ActiveSaberVisual> _activeSabers;
  std::unordered_map<GlobalNamespace::NoteController*, VisualReplacement> _noteReplacements;
  std::unordered_map<GlobalNamespace::SaberModelController*, VisualReplacement> _saberReplacements;
  std::unordered_map<GlobalNamespace::NoteDebris*, VisualReplacement> _debrisReplacements;
  CameraApplier* _cameraApplier = nullptr;
  UnityEngine::GameObject* _cameraApplierGO = nullptr;
  MultipassKeywordController* _multipassController = nullptr;

  UnityEngine::GameObject* _lastMainCameraGO = nullptr;
  bool _mainCameraPropsDirty = false;
  std::optional<int> _mainCamOriginalDepthMode;
  std::optional<int> _mainCamOriginalClearFlags;
  std::optional<UnityEngine::Color> _mainCamOriginalBackgroundColor;
  // Far Culling Distance (0.14.23): the camera's own per-layer distances, kept
  // to put back, and the distance applied over them (0: none applied).
  std::optional<std::vector<float>> _mainCamOriginalCullDistances;
  bool _mainCamOriginalCullSpherical = false;
  int _appliedFarCulling = 0;
  UnityEngine::RenderTexture* _mainBlitTexture = nullptr;
  UnityEngine::RenderTexture* _scratchBlitTexture = nullptr;
  // Flat-screen guard: 16x16 samples of the picture before and after a
  // call's effects, and per-effect bookkeeping.
  struct BlitGuardWatch {
    float start = 0.0f;
    int lastFrame = -10;
    int samples = 0;
    int flat = 0;
  };
  UnityEngine::RenderTexture* _blitGuardIn = nullptr;
  UnityEngine::RenderTexture* _blitGuardOut = nullptr;
  UnityEngine::Texture2D* _blitGuardReader = nullptr;
  std::unordered_map<UnityEngine::Material*, BlitGuardWatch> _blitGuardWatches;
  std::set<UnityEngine::Material*> _blitGuardKilled;
  bool _blitGuardAllOff = false;
  bool _blitGuardLogged = false;
  float _blitGuardLastSample = 0.0f;
  UnityEngine::Vector4 _blitGuardScreenParams = UnityEngine::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  bool EnsureBlitGuardTextures();
  bool BlitGuardDue(std::vector<UnityEngine::Material*> const& materials);
  void BlitGuardEvaluate(std::vector<UnityEngine::Material*> const& materials, UnityEngine::RenderTexture* src);
  std::optional<RenderTexturePolicy::Descriptor> _mainBlitAllocation, _scratchBlitAllocation;
  struct BlitMaterialValidity {
    SafePtr<UnityEngine::Material> material;
    SafePtr<UnityEngine::Shader> shader;
    bool supported = false, fallback = false, valid = false;
  };
  mutable std::unordered_map<UnityEngine::Material*, BlitMaterialValidity> _blitMaterialValidCache;
  std::unordered_set<CustomJSONData::CustomEventData*> _catchUpAppliedCustomEvents;
  float _lastSongTime = -1.0f;
  float _lastKnownSongLength = -1.0f;
  bool _recoveryMode = false;
  std::vector<std::string> _recoverySkippedBlits;
  // This map went down right after its own saber trail was created last time
  // (0.15.15); it plays without it. Independent of Crash Recovery Mode.
  bool _skipMapTrails = false;
  bool _skipMapTrailsLogged = false;
  std::chrono::steady_clock::time_point _lastRecoveryHeartbeat{};
  float _lastSyncSongTime = -1.0f;
  float _lastSongAdvanceAt = -1.0f;  // unscaled time the song clock last moved (0.15.17)
  int _songTimeCacheFrame = -1;
  int _lastUpdateErrorFrame = -1000;
  int _lastSyncHeartbeatFrame = -1000;
  float _songTimeCache = 0.0f;
  bool _loggedUnityPlatformInfo = false;
  bool _pauseMenuActive = false;
  bool _isResetting = false;

  // Watchdog. Vivify does a lot of work on the main thread at level load --
  // realising every asset in a bundle, decoding block-compressed textures,
  // repairing shaders -- and any of it running long turns into a frozen game
  // that has to be force-quit, with no indication of which part was to blame.
  //
  // Rather than trust each individual path to stay fast, per-frame work is
  // timed. A sustained run of frames over budget makes Vivify stand down for
  // the rest of the level and say so: the map loses its Vivify visuals, which
  // is bad, but the game keeps running and the log names the phase.
  bool _selfDisabledThisLevel = false;

  // Everything the on-device report needs. Gathered as the level loads so that
  // a level which then freezes still has a complete "started" block on disk --
  // a frozen game never reaches the end-of-level write, so anything only
  // recorded at the end would be lost exactly when it matters most.
  std::string _graphicsSummary;
  std::string _sourceBundleScanText;
  double _loadMsCacheAssets = 0.0;
  double _loadMsDecodeTextures = 0.0;
  double _loadMsRepairShaders = 0.0;
  double _loadMsTotal = 0.0;
  int _auditShadersSeen = 0;
  int _auditShadersRunnable = 0;
  int _auditShadersNoProgram = 0;
  int _auditShadersDeviceRejected = 0;
  // Live visual checks (VivifyVisualCheck.cpp, 0.14.29): what looked wrong
  // this level, for the top of its report block.
  std::vector<std::string> _visualProblems;
  int _visualChecksRun = 0;
  std::vector<std::string> _auditRejectedNames;
  int _texturesDecoded = 0;
  int _texturesSkipped = 0;
  // Textures the decoder refused rather than turn into black ones.
  int _texturesUnreadable = 0;
  int _texturesBlank = 0;
  bool _levelReportOpen = false;
  bool _fallbackShaderSearchFailed = false;
  int _slowFrameStreak = 0;
  // Wall-clock gap between gameplay frames, for stalls outside Vivify's own
  // work (the GPU driver compiling a map's shaders on first draw).
  std::chrono::steady_clock::time_point _lastGameplayFrame{};
  int _stallsLogged = 0;
  double _worstFrameMs = 0.0;

  bool _reduceDebris = false;
  bool _reduceDebrisCached = false;

  bool _inMultiplayerGameplay = false;
  bool _inMultiplayerGameplayCached = false;

  // Bundle chosen for the selected level. Usually the level's own
  // bundleAndroid2021.vivify, but it can point at a downloaded or an
  // on-device-converted bundle that lives outside the song folder.
  std::string _selectedBundlePath;
  // Source path of a PC->Android bundle conversion currently running on a
  // worker thread, so re-selecting the same level does not start a second one.
  std::string _bundleConversionSource;
  // PC-built shaders standing in for shaders a level's Android bundle shipped
  // empty, by shader name, for _graftLevelPath. Kept alive with
  // DontUnloadUnusedAsset; replaced when another level needs them.
  std::unordered_map<std::string, UnityEngine::Shader*> _graftedShaders;
  // The converted PC bundle _graftedShaders came from.
  std::string _graftDonorPath;
  // Whether a bundle's shaders read the scene depth texture, by
  // DepthScanKey(path); filled in by a worker thread (0.14.21).
  std::mutex _depthScanMutex;
  std::unordered_map<std::string, bool> _depthScanResults;
  std::unordered_set<std::string> _depthScansRunning;
  bool _sceneDepthSkipLogged = false;
  // MapShadersNeedSceneDepth runs every frame: the bundles it last looked at,
  // their scan keys (computed once, as they stat the file) and, once known,
  // its answer.
  std::string _depthSourcesFor;
  std::vector<std::string> _depthSourceKeys;
  std::optional<bool> _depthDecision;
  // Blit companion shaders by name, for _blitCompanionLevel, and the material
  // copies made on them (original -> copy).
  std::unordered_map<std::string, UnityEngine::Shader*> _singleViewShaders;
  std::unordered_map<UnityEngine::Material*, UnityEngine::Material*> _singleViewBlitMaterials;
  // Shaders in the selected map's Quest bundle whose programs are all
  // single-view, by name; a blit with one runs once per eye.
  std::set<std::string> _singleViewOnlyNativeShaders;
  std::set<std::string> _stereoDropsTextureShaders;
  std::map<std::string, std::set<std::string>> _intUniformsByShader;
  std::set<UnityEngine::Material*> _intFixedMaterials;
  std::set<std::string> _perEyeBlitLogged;
  bool NeedsPerEyeBlit(UnityEngine::Material* material, UnityEngine::RenderTexture* destination);
  std::string _blitCompanionLevel;
  struct FixedFeatureMaterial { SafePtr<UnityEngine::Material> material; SafePtr<UnityEngine::Shader> shader; };
  std::unordered_map<UnityEngine::Material*, FixedFeatureMaterial> _fixedFeatureMaterials;
  std::unordered_map<UnityEngine::Renderer*, SafePtr<UnityEngine::Renderer>> _fixedFeatureRenderers;
  using FixedFeatureMaterialList = System::Collections::Generic::List_1<UnityW<UnityEngine::Material>>;
  SafePtr<FixedFeatureMaterialList> _fixedFeatureMaterialList;
  std::unordered_set<UnityEngine::Material*> _fixedFeatureChecked;
  bool _fixedFeatureViolation = false;
  std::optional<bool> _fixedFeatureChartProof;
  bool RememberFixedMaterialFeatures(UnityEngine::Material* material);
  static bool FixedMaterialFeaturesMatch(UnityEngine::Material* material);
  void RegisterFixedFeatureRenderers(UnityEngine::GameObject* root);
  void RejectFixedMaterialFeatures(UnityEngine::Material* material);
  // Shader preparation queue for the selected converted bundle.
  std::vector<UnityEngine::Material*> _warmupQueue;
  size_t _warmupIndex = 0;
  int _warmupPassAttempts = 0, _warmupPassFailures = 0;
  struct WarmupUnboundPass { std::string shader; int pass, passCount; };
  std::vector<WarmupUnboundPass> _warmupUnboundPasses;
  std::string _warmupLevel;
  std::string _warmupBundle;
  float _warmupStarted = 0.0f;
  void StartShaderWarmup(std::string const& bundlePath);
  // An in-flight companion load (0.14.11: asynchronous, never on the main
  // thread in one go -- the synchronous load froze converted maps).
  SafePtr<UnityEngine::AssetBundleCreateRequest> _blitCompanionCreate;
  SafePtr<UnityEngine::AssetBundleRequest> _blitCompanionAssets;
  UnityEngine::AssetBundle* _blitCompanionBundle = nullptr;
  std::string _blitCompanionLoadingLevel;
  std::string _blitCompanionLoadingPath;
  uint64_t _blitCompanionGeneration = 0;
  uint64_t _blitCompanionLoadingGeneration = 0;
  struct PendingBlitCompanion {
    std::string level, path;
    bool wantsPrefabs;
    uint64_t generation;
  };
  std::optional<PendingBlitCompanion> _pendingBlitCompanion;
  // Single-view shaders for a map's own texture cameras (0.15.3): the
  // companion's prefabs are loaded too when the map is known to have such
  // cameras, so every shader its prefabs use has a single-view twin.
  bool _blitCompanionWantsPrefabs = false;
  bool _blitCompanionLoadingWantsPrefabs = false;
  SafePtr<UnityEngine::AssetBundleRequest> _blitCompanionPrefabs;
  std::string _companionPcBundlePath;
  // Materials of spawned map objects whose shader has a single-view twin,
  // swapped while a texture camera renders (SingleViewCameraShaders).
  std::unordered_map<UnityEngine::Material*, std::pair<UnityEngine::Shader*, UnityEngine::Shader*>> _singleViewSwaps;
  bool _singleViewSwapped = false;

public:
  void NoteTextureCameras(int count);
  void RegisterSingleViewMaterials(UnityEngine::GameObject* root);
  void RegisterLiveSingleViewMaterials();
  void BeginSingleViewCamera();
  void EndSingleViewCamera();

private:
  std::string _graftLevelPath;
  int _graftGeneration = 0;
  int _graftApplied = 0;
  float _graftDeadline = -1.0f;  // realtime by which the graft's PC download must answer
  std::string _graftPendingLevel;
  std::string _graftPendingAndroid;
  // Converted bundle whose .loading marker is on disk, or empty.
  std::string _loadGuardPath;

  // In-flight asset download. WebUtils does not guarantee a callback on every
  // failure mode, so the play button is not left waiting on one forever.
  // Generic shader used when a bundle's own shader cannot run on this GPU.
  // Resolved lazily from the shaders actually loaded in the process.
  UnityEngine::Shader* _fallbackShader = nullptr;
  // Materials whose original shader could not run here and were given the
  // generic stand-in. Fine for geometry, never valid for a full-screen blit.
  std::unordered_set<UnityEngine::Material*> _fallbackShadedMaterials;
  // Original (undecodable) texture -> decoded RGBA32 replacement. nullptr value
  // means it was tried and could not be decoded, so it is not retried.
  std::unordered_map<UnityEngine::Texture*, UnityEngine::Texture*> _decodedTextures;
  int _shaderRepairAttempts = 0;
  int _shaderRepairSucceeded = 0;
  int _shaderRepairFailed = 0;
  // Counted apart from the failures: these are meant to be undrawn, and folding
  // them into "could not be repaired" hid a rule that was deleting scenery.
  int _screenEffectsDeclined = 0;
  int _standInsDimmedFromWhite = 0;
  int _instancingDisabledMaterials = 0;  // converted-bundle materials with GPU instancing turned off
  // Materials already visited by the texture pass. One material is normally
  // shared by many renderers across a prefab, and the decode budget is spent in
  // real milliseconds.
  std::unordered_set<UnityEngine::Material*> _texturesScannedMaterials;

  int _downloadGeneration = 0;
  float _downloadDeadline = -1.0f;
  std::string _pendingDownloadLevelPath;
  std::string _pendingDownloadPcFallback;
};
}
