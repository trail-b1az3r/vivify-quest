#include "VivifyRuntimeInternal.hpp"
#include "VivifySeparateEye.hpp"
#include "VivifyComponents.hpp"
#include "VivifyAudioLink.hpp"
#include "VivifyRenderTextureDescriptor.hpp"
#include "VivifyTextureBindings.hpp"
#include "UnityEngine/Renderer.hpp"
#include "UnityEngine/Matrix4x4.hpp"
// Defines SaberMovementData's interface conversion that InitFromSaber calls;
// Saber.hpp only declares it, which compiles and then fails to link.
#include "GlobalNamespace/SaberMovementData.hpp"

DEFINE_TYPE(Vivify, OffsetBladeMovementData);
DEFINE_TYPE(Vivify, FollowedSaberTrail);
DEFINE_TYPE(Vivify, RuntimeBehaviour);
DEFINE_TYPE(Vivify, MultipassKeywordController);
DEFINE_TYPE(Vivify, CameraApplier);
DEFINE_TYPE(Vivify, SeparateEyeCapture);
DEFINE_TYPE(Vivify, SingleViewCameraShaders);
DEFINE_TYPE(Vivify, CullingCameraController);
DEFINE_TYPE(Vivify, SecondaryCameraController);

namespace Vivify {

GlobalNamespace::BloomPrePass* SyncCameraBloom(UnityEngine::Camera* sourceCamera, UnityEngine::Camera* target,
                                             GlobalNamespace::BloomPrePass* previous, bool inheritEnabled) {
  if (!IsManagedAlive(sourceCamera) || !IsManagedAlive(target)) return previous;
  auto* source = sourceCamera->GetComponent<GlobalNamespace::BloomPrePass*>();
  auto* copy = IsManagedAlive(previous) ? previous : target->GetComponent<GlobalNamespace::BloomPrePass*>();
  if (!IsManagedAlive(source)) {
    if (IsManagedAlive(copy)) copy->set_enabled(false);
    return copy;
  }
  bool const enabled = inheritEnabled ? source->get_enabled() : (!IsManagedAlive(copy) || copy->get_enabled());
  bool const changed = IsManagedAlive(copy) &&
      (copy->____bloomPrepassRenderer != source->____bloomPrepassRenderer ||
       copy->____bloomPrePassEffectContainer != source->____bloomPrePassEffectContainer ||
       copy->____bloomPrePassRenderData != source->____bloomPrePassRenderData);
  if (changed) { copy->set_enabled(false); UnityEngine::Object::Destroy(copy); copy = nullptr; }
  if (!IsManagedAlive(copy)) {
    // The same serialized fields PC Vivify copies onto LateBloomPrePass.
    // Quest 1.40.8 initializes in LazyInit, so supplying them before its
    // first OnPreRender preserves a private _renderData and render texture.
    copy = target->get_gameObject()->AddComponent<GlobalNamespace::BloomPrePass*>();
    copy->set_enabled(false);
    copy->____bloomPrepassRenderer = source->____bloomPrepassRenderer;
    copy->____bloomPrePassEffectContainer = source->____bloomPrePassEffectContainer;
    copy->____bloomPrePassRenderData = source->____bloomPrePassRenderData;
  }
  copy->____mode = source->____mode;
  copy->set_enabled(enabled);
  return copy;
}

namespace {
inline int StereoActiveEyePropertyId() {

  static int id = UnityEngine::Shader::PropertyToID(u"_StereoActiveEye");
  return id;
}
}

void SetMultipassShaderState(bool enabled, UnityEngine::XR::XRSettings_StereoRenderingMode stereoMode, int eye) {
  static int lastEye = -2;
  if (lastEye == eye) return;
  lastEye = eye;

  (void)enabled;
  (void)stereoMode;
  UnityEngine::Shader::SetGlobalInt(StereoActiveEyePropertyId(), eye);
}

void SetMultipassShaderStateForCamera(UnityEngine::Camera* camera) {
  bool const hasCamera = IsManagedAlive(camera);
  auto const stereoMode = UnityEngine::XR::XRSettings::get_stereoRenderingMode();
  bool const isStereoCamera = hasCamera && UnityEngine::XR::XRSettings::get_enabled() &&
                              camera->get_stereoTargetEye().value__ != UnityEngine::StereoTargetEyeMask::None.value__;
  int const activeEye = hasCamera ? camera->get_stereoActiveEye().value__ : 0;
  SetMultipassShaderState(isStereoCamera, stereoMode, activeEye);
}

void OffsetBladeMovementData::Init(GlobalNamespace::IBladeMovementData* followed, UnityEngine::Transform* parent,
                                   UnityEngine::Vector3 topPos, UnityEngine::Vector3 bottomPos) {
  _followed = followed;
  _parent = parent;
  _topPos = topPos;
  _bottomPos = bottomPos;
}

float_t OffsetBladeMovementData::get_bladeSpeed() {
  return 0.0f;
}

GlobalNamespace::BladeMovementDataElement OffsetBladeMovementData::get_lastAddedData() {
  if (_followed == nullptr) return {};
  return Modify(_followed->get_lastAddedData());
}

GlobalNamespace::BladeMovementDataElement OffsetBladeMovementData::get_prevAddedData() {
  if (_followed == nullptr) return {};
  return Modify(_followed->get_prevAddedData());
}

GlobalNamespace::BladeMovementDataElement OffsetBladeMovementData::Modify(GlobalNamespace::BladeMovementDataElement original) {
  if (!IsManagedAlive(_parent)) return original;
  return GlobalNamespace::BladeMovementDataElement(
      original.time,
      original.segmentAngle,
      AddVectors(original.bottomPos, _parent->TransformVector(_topPos)),
      AddVectors(original.bottomPos, _parent->TransformVector(_bottomPos)),
      original.segmentNormal);
}

void FollowedSaberTrail::Awake() {}

void FollowedSaberTrail::InitFollowed(GlobalNamespace::SaberTrail* followed, UnityEngine::Transform* parent,
                                      UnityEngine::Material* material, UnityEngine::Vector3 topPos,
                                      UnityEngine::Vector3 bottomPos, float_t duration,
                                      int32_t samplingFrequency, int32_t granularity) {
  if (!IsManagedAlive(followed) || !IsManagedAlive(parent) || !IsManagedAlive(material)) return;
  auto* followedRenderer = followed->____trailRenderer.unsafePtr();
  auto* rendererPrefab = followed->____trailRendererPrefab.unsafePtr();
  if (!IsManagedAlive(followedRenderer) || !IsManagedAlive(rendererPrefab)) return;

  _followed = followed;
  _saber = nullptr;
  _controller = nullptr;
  auto sourceParent = followedRenderer->get_transform()->get_parent();
  Setup(followed->____movementData, rendererPrefab, sourceParent.unsafePtr(), parent, material, topPos, bottomPos,
        duration, samplingFrequency, granularity, followed->____whiteSectionMaxDuration, followed->____color);
}

void FollowedSaberTrail::InitFromSaber(GlobalNamespace::Saber* saber, GlobalNamespace::SaberModelController* controller,
                                       GlobalNamespace::SaberTrailRenderer* rendererPrefab,
                                       UnityEngine::Transform* parent, UnityEngine::Material* material,
                                       UnityEngine::Color color, UnityEngine::Vector3 topPos,
                                       UnityEngine::Vector3 bottomPos, float_t duration,
                                       int32_t samplingFrequency, int32_t granularity) {
  if (!IsManagedAlive(saber) || !IsManagedAlive(parent) || !IsManagedAlive(material) ||
      !IsManagedAlive(rendererPrefab)) {
    return;
  }
  // The same data the game hands its own trail in SaberModelController.Init.
  auto* movementData = saber->get_movementDataForVisualEffects();
  if (movementData == nullptr) return;

  _followed = nullptr;
  _saber = saber;
  _controller = controller;
  // 0.03 is the white section the game's saber prefab serialises.
  Setup(movementData->i___GlobalNamespace__IBladeMovementData(), rendererPrefab, nullptr, parent, material, topPos,
        bottomPos, duration, samplingFrequency, granularity, 0.03f, color);
}

void FollowedSaberTrail::Setup(GlobalNamespace::IBladeMovementData* movementData,
                               GlobalNamespace::SaberTrailRenderer* rendererPrefab,
                               UnityEngine::Transform* rendererParent, UnityEngine::Transform* parent,
                               UnityEngine::Material* material, UnityEngine::Vector3 topPos,
                               UnityEngine::Vector3 bottomPos, float_t duration, int32_t samplingFrequency,
                               int32_t granularity, float_t whiteSectionMaxDuration, UnityEngine::Color color) {
  if (_offsetMovementData == nullptr) {
    _offsetMovementData = OffsetBladeMovementData::New_ctor();
  }
  _offsetMovementData->Init(movementData, parent, topPos, bottomPos);

  ____trailDuration = duration;
  ____samplingFrequency = samplingFrequency;
  ____granularity = granularity;
  ____whiteSectionMaxDuration = whiteSectionMaxDuration;
  ____movementData = reinterpret_cast<GlobalNamespace::IBladeMovementData*>(_offsetMovementData);
  ____color = color;

  auto* trailRenderer = ____trailRenderer.unsafePtr();
  if (!IsManagedAlive(trailRenderer)) {
    trailRenderer = UnityEngine::Object::Instantiate<GlobalNamespace::SaberTrailRenderer*>(
        rendererPrefab, UnityEngine::Vector3(0.0f, 0.0f, 0.0f), UnityEngine::Quaternion::get_identity());
    ____trailRenderer = trailRenderer;
    if (!IsManagedAlive(trailRenderer)) return;
    if (IsManagedAlive(rendererParent)) {
      trailRenderer->get_transform()->SetParent(rendererParent);
    }
  }

  if (trailRenderer->____meshRenderer) {
    trailRenderer->____meshRenderer->set_material(material);
  }

  Init();
  Update();
  auto gameObject = get_gameObject();
  if (IsManagedAlive(gameObject.unsafePtr())) {
    gameObject->SetActive(true);
  }
}

bool FollowedSaberTrail::SourceAlive() const {
  return _followed != nullptr ? IsManagedAlive(_followed) : IsManagedAlive(_saber);
}

UnityEngine::Color FollowedSaberTrail::SourceColor() {
  // The saber's colour from the game's colour manager first (0.15.9). The
  // game trail's own colour is not reliable: a custom saber mod hides that
  // trail by making its colour transparent, and the map's trail, copying it,
  // went invisible too.
  if (IsManagedAlive(_controller) && _controller->____colorManager != nullptr && IsManagedAlive(_saber)) {
    return _controller->____colorManager->ColorForSaberType(_saber->get_saberType());
  }
  if (_followed != nullptr && _followed->____color.a > 0.01f) return _followed->____color;
  return ____color;
}

void FollowedSaberTrail::SetColorSource(GlobalNamespace::Saber* saber,
                                        GlobalNamespace::SaberModelController* controller) {
  _saber = saber;
  _controller = controller;
  _hasLastAppliedColor = false;
}

void FollowedSaberTrail::Update() {
  if (!SourceAlive()) return;
  auto* trailRenderer = ____trailRenderer.unsafePtr();
  if (!IsManagedAlive(trailRenderer) || !trailRenderer->____meshRenderer) return;
  ____color = SourceColor();
  if (_hasLastAppliedColor && NearlySameColor(____color, _lastAppliedColor)) return;

  auto* block = UnityEngine::MaterialPropertyBlock::New_ctor();
  if (block == nullptr) return;
  block->SetColor(ColorPropertyId(), ____color);
  trailRenderer->____meshRenderer->SetPropertyBlock(block);
  _lastAppliedColor = ____color;
  _hasLastAppliedColor = true;
}

void FollowedSaberTrail::LateUpdate() {
  auto* trailRenderer = ____trailRenderer.unsafePtr();
  auto* meshRenderer = IsManagedAlive(trailRenderer) ? trailRenderer->____meshRenderer.unsafePtr() : nullptr;
  if (!SourceAlive() || ____movementData == nullptr || !IsManagedAlive(meshRenderer)) {
    Cleanup();
    auto gameObject = get_gameObject();
    if (IsManagedAlive(gameObject.unsafePtr())) {
      gameObject->SetActive(false);
    }
    return;
  }
  static_cast<GlobalNamespace::SaberTrail*>(this)->LateUpdate();
}

void FollowedSaberTrail::Cleanup() {
  auto* trailRenderer = ____trailRenderer.unsafePtr();
  if (IsManagedAlive(trailRenderer)) {
    UnityEngine::Object::Destroy(trailRenderer->get_gameObject());
  }
  ____trailRenderer = nullptr;
  _followed = nullptr;
  _saber = nullptr;
  _controller = nullptr;
  _hasLastAppliedColor = false;
}

namespace {

constexpr int kCullingLayer = 22;

bool HasOwnedCullingScope(UnityEngine::MonoBehaviour* self) {
  auto go = self->get_gameObject();
  if (!IsManagedAlive(go.unsafePtr())) return false;
  return go->GetComponent<SecondaryCameraController*>() != nullptr ||
         go->GetComponent<SingleViewCameraShaders*>() != nullptr;
}
}

void CullingCameraController::OnPreCull() {
  if (HasOwnedCullingScope(this)) return;
  CullingPreCull();
}

void CullingCameraController::OnPostRender() {
  if (HasOwnedCullingScope(this)) return;
  CullingPostRender();
}

void CullingCameraController::CullingPreCull() {
  if (_camera == nullptr) _camera = GetComponent<UnityEngine::Camera*>();
  if (!IsManagedAlive(_camera)) return;
  if (!_hasCullingData) return;

  CullingPostRender();

  if (_whitelist) {
    _cachedMask = _camera->get_cullingMask();
    _camera->set_cullingMask(1 << kCullingLayer);
  }

  std::unordered_set<UnityEngine::GameObject*> seen;
  int trackedGameObjects = 0;
  for (auto const& track : _tracks) {
    if (!track) continue;
    for (auto* go : track.GetGameObjects()) {
      if (!IsManagedAlive(go)) continue;
      trackedGameObjects++;
      auto renderers = go->GetComponentsInChildren<UnityEngine::Renderer*>(true);
      if (!renderers) continue;
      for (auto* renderer : renderers) {
        if (!IsManagedAlive(renderer)) continue;
        auto renderGo = renderer->get_gameObject();
        auto* renderGoPtr = renderGo.unsafePtr();
        if (!IsManagedAlive(renderGoPtr)) continue;
        if (!seen.emplace(renderGoPtr).second) continue;
        _cachedLayers.emplace_back(renderGoPtr, renderGoPtr->get_layer());
        renderGoPtr->set_layer(kCullingLayer);
      }
    }
  }

  if (_diagLogCount < 3 && GetVivifyDebugLogging()) {
    _diagLogCount++;
    auto go = get_gameObject();
    PaperLogger.info("Vivify culling pass on '{}': whitelist={} tracks={} trackedGOs={} movedGOs={} camMask=0x{:08x}",
                     IsManagedAlive(go.unsafePtr()) ? ToStdString(go->get_name()) : std::string("?"),
                     BoolText(_whitelist), _tracks.size(), trackedGameObjects, _cachedLayers.size(),
                     static_cast<uint32_t>(_camera->get_cullingMask()));
  }
}

void CullingCameraController::CullingPostRender() {
  if (_camera != nullptr && UnityEngine::Object::op_Implicit_bool(_camera) && _cachedMask.has_value()) {
    _camera->set_cullingMask(_cachedMask.value());
    _cachedMask.reset();
  }
  for (auto const& [go, layer] : _cachedLayers) {
    if (IsManagedAlive(go)) {
      go->set_layer(layer);
    }
  }
  _cachedLayers.clear();
}

void SecondaryCameraController::OnPreCull() {
  auto* camera = GetCamera();
  if (!IsManagedAlive(camera)) return;
  auto mainCam = UnityEngine::Camera::get_main();
  auto* other = mainCam.unsafePtr();
  if (IsManagedAlive(other) && other != camera) {
    SyncCameraBloom(other, camera, nullptr, false);

    // Match PC RefreshCamera: aspect, eye target, depth or culling distances
    // can change without changing the mask/FOV/clips. Refresh every copied
    // property so secondary color/depth captures never keep that stale state.
    camera->set_stereoTargetEye(other->get_stereoTargetEye());
    camera->set_fieldOfView(other->get_fieldOfView());
    camera->set_aspect(other->get_aspect());
    camera->set_depth(other->get_depth() - 1.0f);
    camera->set_nearClipPlane(other->get_nearClipPlane());
    camera->set_farClipPlane(other->get_farClipPlane());
    camera->set_layerCullDistances(other->get_layerCullDistances());
    camera->set_targetTexture(nullptr);
    camera->set_cullingMask(other->get_cullingMask());
    auto transform = get_transform();
    if (IsManagedAlive(transform.unsafePtr())) {
      transform->set_localPosition(UnityEngine::Vector3::get_zero());
      transform->set_localRotation(UnityEngine::Quaternion::get_identity());
    }
    camera->set_cullingMatrix(
        UnityEngine::Matrix4x4::op_Multiply(other->get_projectionMatrix(), other->get_worldToCameraMatrix()));
    camera->set_projectionMatrix(other->get_projectionMatrix());
    camera->set_nonJitteredProjectionMatrix(other->get_nonJitteredProjectionMatrix());
    camera->set_worldToCameraMatrix(other->get_worldToCameraMatrix());
  }

  auto* culling = GetComponent<CullingCameraController*>();
  if (IsManagedAlive(culling)) {
    culling->CullingPreCull();
  }
}

void SecondaryCameraController::OnPostRender() {
  auto* culling = GetComponent<CullingCameraController*>();
  if (IsManagedAlive(culling)) {
    culling->CullingPostRender();
  }
}

UnityEngine::Camera* SecondaryCameraController::GetCamera() {
  if (!IsManagedAlive(_camera)) {
    auto go = get_gameObject();
    _camera = IsManagedAlive(go.unsafePtr()) ? go->GetComponent<UnityEngine::Camera*>() : nullptr;
  }
  return _camera;
}

// Creates (or re-creates) the per-eye capture texture for this camera.
//
// Depth captures use RenderTextureFormat::RFloat rather than a depth format,
// matching what upstream Vivify's DepthBlit material writes on PC. A map's
// shader samples the texture it named in CreateCamera as an ordinary single
// channel float; handing it a depth-format texture instead reads back as
// something else entirely, which is why depth-driven effects (raymarchers)
// produced nothing at all.
UnityEngine::RenderTexture* SecondaryCameraController::EnsureEyeTexture(
    std::unordered_map<int, UnityEngine::RenderTexture*>& textures, int eye, UnityEngine::RenderTexture* src,
    bool depth) {
  if (!IsManagedAlive(src)) return nullptr;
  auto descriptor = src->get_descriptor();
  descriptor.set_msaaSamples(1);
  if (depth) {
    descriptor.set_colorFormat(UnityEngine::SystemInfo::SupportsRenderTextureFormat(UnityEngine::RenderTextureFormat::RFloat)
                                   ? UnityEngine::RenderTextureFormat::RFloat
                                   : UnityEngine::RenderTextureFormat::ARGBHalf);
    descriptor.set_depthBufferBits(0);
  }
  auto const requested = RenderTexturePolicy::Describe(descriptor);
  auto& allocations = depth ? _depthAllocations : _colorAllocations;
  auto it = textures.find(eye);
  UnityEngine::RenderTexture* tex = it == textures.end() ? nullptr : it->second;
  auto allocation = allocations.find(eye);
  std::optional<RenderTexturePolicy::Descriptor> prior;
  if (allocation != allocations.end()) prior = allocation->second;
  bool const alive = IsManagedAlive(tex);
  if (!RenderTexturePolicy::NeedsAllocation(prior, requested, alive, alive && tex->IsCreated())) return tex;
  // Reallocation is also a release path. Do not leave the retired image bound
  // if replacement allocation fails, or erase a different camera's binding.
  auto const key = depth ? depthKey : colorKey;
  if (key) ClearOwnedTextureBinding(*key, tex);
  if (alive) {
    tex->Release();
    UnityEngine::Object::Destroy(tex);
  }
  textures.erase(eye); allocations.erase(eye);
  tex = UnityEngine::RenderTexture::New_ctor(descriptor);
  if (!IsManagedAlive(tex)) return nullptr;
  if (!tex->Create()) { UnityEngine::Object::Destroy(tex); return nullptr; }
  textures[eye] = tex;
  allocations[eye] = requested;
  return tex;
}

void SecondaryCameraController::OnRenderImage(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  auto* camera = GetCamera();
  int const stereoActiveEye = IsManagedAlive(camera) ? camera->get_stereoActiveEye().value__ : 0;

  if (colorKey.has_value()) {
    if (auto* tex = EnsureEyeTexture(colorTextures, stereoActiveEye, src, false); IsManagedAlive(tex)) {
      if (!Runtime::Instance().RenderSecondaryCameraMainEffect(camera, src, tex)) UnityEngine::Graphics::Blit(src, tex);
    }
  }

  if (depthKey.has_value()) {
    if (auto* tex = EnsureEyeTexture(depthTextures, stereoActiveEye, src, true); IsManagedAlive(tex)) {
      Runtime::Instance().CaptureSecondaryDepth(tex);
    }
  }

  // PC's capture controller writes its named textures, not the headset target.
  // Copying this scene into dest adds a full-screen pass and leaks the capture
  // into a main camera that clears only depth (or does not clear at all).
  (void)dest;
}

void SecondaryCameraController::ReleaseGlobalBindings() {
  auto clear = [](std::optional<int> key, auto const& textures) {
    if (!key) return;
    for (auto const& [_, texture] : textures)
      if (ClearOwnedTextureBinding(*key, texture)) break;
  };
  clear(colorKey, colorTextures);
  clear(depthKey, depthTextures);
}

void SecondaryCameraController::OnDestroy() {
  ReleaseGlobalBindings();
  _colorAllocations.clear(); _depthAllocations.clear();
  for (auto* textures : {&colorTextures, &depthTextures}) {
    for (auto& [eye, tex] : *textures) {
      if (IsManagedAlive(tex)) {
        tex->Release();
        UnityEngine::Object::Destroy(tex);
      }
    }
    textures->clear();
  }
}

void MultipassKeywordController::Awake() {
  auto gameObject = get_gameObject();
  if (IsManagedAlive(gameObject.unsafePtr())) {
    _camera = gameObject->GetComponent<UnityEngine::Camera*>();
  }
}

void MultipassKeywordController::OnPreRender() {
  if (!IsManagedAlive(_camera)) {
    auto gameObject = get_gameObject();
    _camera = IsManagedAlive(gameObject.unsafePtr()) ? gameObject->GetComponent<UnityEngine::Camera*>() : nullptr;
  }
  SetMultipassShaderStateForCamera(_camera);
}

void MultipassKeywordController::OnDisable() {
  SetMultipassShaderState(false);
}

#ifdef VIVIFY_MENU_DIAGNOSTICS
void PollMenuDiagnostics();
#endif

void RuntimeBehaviour::Update() {
#ifdef VIVIFY_MENU_DIAGNOSTICS
  PollMenuDiagnostics();
#endif
  Runtime::Instance().ValidateFixedMaterialFeatures();
  // Before the runtime's own update, which returns early in the menu and when
  // its watchdog stands down; AudioLink has to run regardless (song previews).
  try {
    AudioLink::Tick();
  } catch (std::exception const& ex) {
    static int reported = 0;
    if (reported++ < 3) PaperLogger.error("Vivify AudioLink update threw: {}", ex.what());
  } catch (...) {
    static int reported = 0;
    if (reported++ < 3) PaperLogger.error("Vivify AudioLink update threw a non-std exception");
  }
  try {
    Runtime::Instance().PollShaderWarmup();
  } catch (...) {
    static int reported = 0;
    if (reported++ < 3) PaperLogger.error("Vivify shader preparation threw");
  }
  try {
    Runtime::Instance().PollBlitCompanion();
    Runtime::Instance().PollBlitCompanionPrefabs();
    SeparateEye::Poll();
  } catch (...) {
    static int reported = 0;
    if (reported++ < 3) PaperLogger.error("Vivify Blit companion update threw");
  }
  Runtime::Instance().Update();
}

void RuntimeBehaviour::OnDestroy() {
  Runtime::Instance().OnBehaviourDestroyed(this);
}

void SingleViewCameraShaders::OnPreCull() {
  if (auto* culling = GetComponent<CullingCameraController*>(); IsManagedAlive(culling)) culling->CullingPreCull();
  Runtime::Instance().ValidateFixedMaterialFeatures();
  if (!_separateEyeScope) _separateEyeScope = SeparateEye::BeginTextureCamera(this, GetComponent<UnityEngine::Camera*>());
}

void SingleViewCameraShaders::OnPreRender() {
  if (!_separateEyeScope && !_legacyScope && GetSingleViewMapCameras()) {
    Runtime::Instance().BeginSingleViewCamera();
    _legacyScope = true;
  }
}

void SingleViewCameraShaders::OnPostRender() {
  if (_separateEyeScope) SeparateEye::EndTextureCamera(this);
  if (_legacyScope) Runtime::Instance().EndSingleViewCamera();
  _separateEyeScope = _legacyScope = false;
  if (auto* culling = GetComponent<CullingCameraController*>(); IsManagedAlive(culling)) culling->CullingPostRender();
}

void SingleViewCameraShaders::OnDisable() {
  OnPostRender();
}

void SeparateEyeCapture::OnRenderImage(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  SeparateEye::RenderEyeImage(GetComponent<UnityEngine::Camera*>(), src, dest);
}

void CameraApplier::OnPreCull() {
  Runtime::Instance().ValidateFixedMaterialFeatures();

  Runtime::Instance().RestoreSecondaryCullingLayers();
  if (cameraName == "_Main") SeparateEye::PrepareFrame(GetComponent<UnityEngine::Camera*>(), this);
}

void CameraApplier::OnPreRender() {

  auto& runtime = Runtime::Instance();
  runtime.BindSecondaryCameraTextures();

  auto go = get_gameObject();
  auto* camera = IsManagedAlive(go.unsafePtr()) ? go->GetComponent<UnityEngine::Camera*>() : nullptr;
  runtime.AddMidRenderCommandBuffers(camera);
}

void CameraApplier::OnPostRender() {
  if (cameraName == "_Main") SeparateEye::RestoreMainCamera(GetComponent<UnityEngine::Camera*>());
  Runtime::Instance().RemoveMidRenderCommandBuffers();
}

void CameraApplier::OnDisable() {
  if (cameraName == "_Main") SeparateEye::CancelFrame();
}

void CameraApplier::OnRenderImage(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  auto& runtime = Runtime::Instance();
  if (cameraName == "_Main" && GetExperimentalSeparateEye()) {
    auto go = get_gameObject();
    auto* camera = IsManagedAlive(go.unsafePtr()) ? go->GetComponent<UnityEngine::Camera*>() : nullptr;
    if (SeparateEye::TryRender(camera, src, this)) {
      runtime.CacheMainRenderDescriptor(src);
      runtime.ApplyBlits(src, dest, cameraName, -1);
      return; // both eye images already include pre/main/post effects
    }
  }
  RenderEffects(src, dest);
}

void CameraApplier::RenderEffects(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  auto& runtime = Runtime::Instance();
  runtime.CacheMainRenderDescriptor(src);
  if (hasMainEffect && IsManagedAlive(mainEffectController) &&
      mainEffectController->_mainEffectContainer.ptr() != nullptr &&
      IsManagedAlive(mainEffectController->_mainEffectContainer)) {
    auto mainEffectSO = mainEffectController->_mainEffectContainer->get_mainEffect();
    if (mainEffectSO.ptr() != nullptr && IsManagedAlive(mainEffectSO) && mainEffectSO->get_hasPostProcessEffect()) {
      auto renderMain = [&](UnityEngine::RenderTexture* from, UnityEngine::RenderTexture* to) {
        mainEffectController->OnPreRender();
        try { mainEffectSO->Render(from, to, mainEffectController->____fadeValue); }
        catch (...) { mainEffectController->OnPostRender(); throw; }
        mainEffectController->OnPostRender();
      };
      if (runtime.IsResetting() || GetDisableAllBlits() ||
          (runtime.GetPreEffectsEmpty() && runtime.GetPostEffectsEmpty())) {
        renderMain(src, dest);
        return;
      }
      auto desc = src->get_descriptor();
      desc.set_msaaSamples(1);
      desc.set_depthBufferBits(0);
      struct Temporary {
        SafePtr<UnityEngine::RenderTexture> value;
        explicit Temporary(UnityEngine::RenderTextureDescriptor descriptor) : value(UnityEngine::RenderTexture::GetTemporary(descriptor).unsafePtr()) {}
        ~Temporary() { if (value && IsManagedAlive(value.ptr())) UnityEngine::RenderTexture::ReleaseTemporary(value.ptr()); }
        UnityEngine::RenderTexture* unsafePtr() const { return value ? value.ptr() : nullptr; }
      };
      Temporary temp(desc), temp2(desc);

      bool const tempValid = IsManagedAlive(temp.unsafePtr());
      bool const temp2Valid = IsManagedAlive(temp2.unsafePtr());

      if (tempValid && temp2Valid) {
        runtime.ApplyBlits(src, temp.unsafePtr(), cameraName, 1);
        renderMain(temp.unsafePtr(), temp2.unsafePtr());
        runtime.ApplyBlits(temp2.unsafePtr(), dest, cameraName, 2);
      } else if (tempValid) {
        runtime.ApplyBlits(src, temp.unsafePtr(), cameraName, 1);
        renderMain(temp.unsafePtr(), dest);
        runtime.ApplyBlits(dest, dest, cameraName, 2);
      } else {
        renderMain(src, dest);
        runtime.ApplyBlits(dest, dest, cameraName, 2);
      }

      return;
    }
  }
  runtime.ApplyBlits(src, dest, cameraName, 0);
}

}
