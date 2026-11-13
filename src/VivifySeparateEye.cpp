#include "VivifySeparateEye.hpp"
#include "VivifySeparateEyePolicy.hpp"
#include "VivifyCameraBindings.hpp"
#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"
#include "VivifyBundleConvert.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/CameraClearFlags.hpp"
#include "UnityEngine/StereoTargetEyeMask.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/VRTextureUsage.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/SystemInfo.hpp"
#include "UnityEngine/GL.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include "UnityEngine/ReflectionProbe.hpp"
#include "VivifyProjectors.hpp"
#include "VivifyStereoBindings.hpp"
#include "VivifyTessellationCache.hpp"
#include "UnityEngine/Vector4.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/XR/XRSettings.hpp"
#include "GlobalNamespace/BloomPrePass.hpp"
#include "GlobalNamespace/BloomPrePassRendererSO.hpp"
#include "GlobalNamespace/BloomPrePassRenderDataSO.hpp"
#include "GlobalNamespace/MirrorRendererSO.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <list>
#include <chrono>
#include <deque>

namespace Vivify::SeparateEye {
namespace {
// Dedicated version: these are shader donors, never ordinary runtime caches.
constexpr char kVersion[] = "separate-eye-v9-upstream0159-typed-names";
using ShaderMap = std::unordered_map<std::string, SafePtr<UnityEngine::Shader>>;
struct Load {
  uint64_t generation;
  int eye;
  SafePtr<UnityEngine::AssetBundleCreateRequest> create;
  SafePtr<UnityEngine::AssetBundleRequest> assets;
  SafePtr<UnityEngine::AssetBundle> bundle;
};
struct MaterialPair {
  SafePtr<UnityEngine::Material> source;
  std::array<SafePtr<UnityEngine::Material>, 6> copies;
  std::array<uint64_t, 6> synchronized{};
};
struct OriginalMaterial {
  SafePtr<UnityEngine::Material> source;
  SafePtr<UnityEngine::Shader> sourceShader;
  SafePtr<UnityEngine::Shader> fallbackShader;
  std::string shader;
  std::optional<UnityEngine::Color> color, fallbackColor;
};
std::unordered_map<UnityEngine::Material*, OriginalMaterial> originals;
struct NativeCheck { uint64_t epoch = 0; bool compatible = false; };
std::unordered_map<UnityEngine::Material*, NativeCheck> nativeChecks;
uint64_t nativeCheckEpoch = 1;
uint64_t generation = 0;
std::string preparedLevel, rejection;
std::string preparedSource, preparedCache;
std::array<ShaderMap, 6> shaders;
std::array<ShaderMap, 6> donorAssets;
using BindingCatalog = std::unordered_map<std::string, uint16_t>;
std::array<BindingCatalog, 6> bindingCatalogs;
std::unordered_map<UnityEngine::Shader*, StereoBindings::Mask> shaderBindings;
std::array<std::unordered_set<std::string>, 6> ambiguousNames;
std::array<std::unordered_map<UnityEngine::Shader*, SafePtr<UnityEngine::Shader>>, 6> donorObjects;
struct SourceShaderAsset { SafePtr<UnityEngine::Shader> shader; std::vector<std::string> paths; };
std::unordered_map<UnityEngine::Shader*, SourceShaderAsset> sourceShaderAssets;
std::list<Load> loads;
struct QueuedLoad { uint64_t generation; std::string level; std::array<std::string, 6> paths; };
std::optional<QueuedLoad> pending;
std::array<bool, 6> ready{};
bool completeStageInfo = false;
std::unordered_set<std::string> separateEyeShaderNames;
std::vector<std::string> tessellationShaderNames;
std::unordered_set<std::string> activeSceneEyeTessellationNames;
enum class SceneRoute { Unreported, Native, Separate };
SceneRoute sceneRoute = SceneRoute::Unreported;
int nativeFrames = 0, separateFrames = 0;
bool preflightStarted = false, preflightComplete = false, preparingGate = false, preparationErrorShown = false;
std::chrono::steady_clock::time_point preparationStarted;
std::deque<SafePtr<UnityEngine::Shader>> shaderChecks;
constexpr char kPlayGate[] = "VivifyRenderer";
void PreparationFailed() {
  if (preparationErrorShown) return;
  preparationErrorShown = true;
  // SongCore's Disable does not update an existing reason. Replace the
  // preparation reason synchronously so a failure is visible in the menu.
  if (preparingGate) SongCore::API::PlayButton::EnablePlayButton(kPlayGate);
  SongCore::API::PlayButton::DisablePlayButton(kPlayGate, "Map effects could not be prepared. Turn off the experimental renderer to play.");
  preparingGate = true;
}
std::unordered_map<UnityEngine::Material*, MaterialPair> materials;
std::array<SafePtr<UnityEngine::Camera>, 2> cameras;
std::array<SafePtr<GlobalNamespace::BloomPrePass>, 2> eyeBloom;
std::array<SafePtr<UnityEngine::RenderTexture>, 2> colors;
SafePtr<UnityEngine::RenderTexture> staging;
bool rendering = false, failed = false, reported = false;
// 0/1: scene eyes; 2: authored mono cameras; 3/4: per-eye screen effects.
int captureEye = -1;
int blitEye = -1;
bool captureComplete = false, captureNeedsDepth = false;
SafePtr<CameraApplier> imageEffects;
std::unordered_map<UnityEngine::Material*, UnityEngine::Material*> cloneSources;
int width = 0, height = 0, format = -1;
std::list<SafePtr<UnityEngine::Renderer>> sceneRenderers;
std::unordered_set<UnityEngine::Renderer*> rendererIndex;
std::list<SafePtr<UnityEngine::Behaviour>> sceneProjectors;
std::unordered_set<UnityEngine::Behaviour*> projectorIndex;
using MaterialList = System::Collections::Generic::List_1<UnityW<UnityEngine::Material>>;
SafePtr<MaterialList> materialList;
SceneFrame completedFrame;
std::optional<UnityEngine::RenderTextureDescriptor> mainDescriptor;
SafePtr<UnityEngine::Camera> descriptorCamera, suppressedCamera;
SafePtr<GlobalNamespace::BloomPrePass> suppressedBloom;
bool suppressedBloomEnabled = false;
int suppressedMask = 0, suppressedDepth = 0;
bool suppressed = false, descriptorHdr = false, nativeMaterialsInstalled = false;
SafePtr<UnityEngine::Camera> nativeMaterialCamera;
int xrWidth = 0, xrHeight = 0;
float xrScale = 1;
uint64_t materialEpoch = 1;
int timingFrames = 0;
double preparationMs = 0, peakPreparationMs = 0;
std::mutex conversionMutex;
using MirrorCache = System::Collections::Generic::Dictionary_2<GlobalNamespace::MirrorRendererSO_CameraTransformData,
                                                              UnityW<UnityEngine::RenderTexture>>;
struct MirrorCopy {
  SafePtr<UnityEngine::Camera> camera;
  SafePtr<GlobalNamespace::MirrorRendererSO> renderer;
  int cameraId;
};
struct MirrorSource {
  SafePtr<GlobalNamespace::MirrorRendererSO> source;
  std::list<MirrorCopy> copies;
};
std::unordered_map<GlobalNamespace::MirrorRendererSO*, MirrorSource> mirrorSources;
std::unordered_set<GlobalNamespace::MirrorRendererSO*> privateMirrors;
std::string MaterialShaderName(UnityEngine::Material* material) {
  auto found = originals.find(material);
  if (found != originals.end()) return found->second.shader;
  return IsManagedAlive(material) && IsManagedAlive(material->get_shader().unsafePtr())
             ? MaterialSpecialization::Canonical(std::string(material->get_shader()->get_name())) : "";
}

template <class T> bool Alive(T* object) { return IsManagedAlive(object); }
template <class T> void Destroy(SafePtr<T>& object) {
  if (object && Alive(object.ptr())) UnityEngine::Object::Destroy(object.ptr());
  object = nullptr;
}
UnityEngine::Shader* AssetShader(UnityEngine::Object* asset) {
  if (!Alive(asset)) return nullptr;
  if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr)) return shader;
  if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr)) return material->get_shader().unsafePtr();
  return nullptr;
}
void Remember(UnityEngine::Shader* shader, int mode) {
  if (!Alive(shader)) return;
  std::string name(shader->get_name());
  if (!IsDonorShaderName(name) || name.starts_with("__VivifyNativeOther__/")) return;
  auto binding = bindingCatalogs[mode].find(name);
  shaderBindings[shader] = binding == bindingCatalogs[mode].end() ? StereoBindings::All : binding->second;
  shader->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
  for (char const* prefix : {"__VivifyEye0__/", "__VivifyEye1__/", "__VivifyMono__/", "__VivifyBlitEye0__/", "__VivifyBlitEye1__/", "__VivifyNativePerspective__/"})
    if (name.starts_with(prefix)) { name.erase(0, std::string_view(prefix).size()); break; }
  name = MaterialSpecialization::Canonical(std::move(name));
  donorObjects[mode].try_emplace(shader, shader);
  if (ambiguousNames[mode].contains(name)) return;
  auto& result = shaders[mode];
  auto [found, inserted] = result.emplace(std::move(name), shader);
  if (!inserted && found->second.ptr() != shader) {
    ambiguousNames[mode].insert(found->first);
    PaperLogger.info("Vivify donor {}: shader name '{}' has multiple objects; selecting by bundle asset identity", mode, found->first);
    result.erase(found);
  }
}
void Collect(UnityEngine::Object* asset, int mode) {
  if (!Alive(asset)) return;
  if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr)) Remember(shader, mode);
  if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr))
    Remember(material->get_shader().unsafePtr(), mode);
  if (auto* prefab = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr)) {
    for (auto* renderer : prefab->GetComponentsInChildren<UnityEngine::Renderer*>(true)) {
      if (!Alive(renderer)) continue;
      for (auto material : renderer->get_sharedMaterials())
        if (Alive(material.unsafePtr())) Remember(material->get_shader().unsafePtr(), mode);
    }
    for (auto* projector : Projectors::InChildren(prefab)) {
      if (!Alive(projector)) continue;
      auto* material = Projectors::GetMaterial(projector);
      if (Alive(material)) Remember(material->get_shader().unsafePtr(), mode);
    }
  }
}
void LogRejection(std::string_view reason) {
  if (reason == rejection) return;
  rejection = reason;
  PaperLogger.info("Vivify separate-eye prototype: keeping normal frame: {}", reason);
}

// Shared-material snapshots are rooted while nested Camera.Render can run GC.
struct Swap {
  SafePtr<UnityEngine::Renderer> renderer;
  SafePtr<UnityEngine::Behaviour> projector;
  SafePtr<Array<UnityEngine::Material*>> original;
  std::array<SafePtr<Array<UnityEngine::Material*>>, 2> eye;
  bool installed = false, active = false;
};
bool OwnerAlive(Swap const& swap) {
  return (swap.renderer && Alive(swap.renderer.ptr())) || (swap.projector && Alive(swap.projector.ptr()));
}
void SetMaterials(Swap& swap, ArrayW<UnityEngine::Material*> array) {
  if (swap.renderer && Alive(swap.renderer.ptr())) swap.renderer->set_sharedMaterials(array);
  else if (swap.projector && Alive(swap.projector.ptr())) {
    if (array.size() != 1) throw std::runtime_error("projector material snapshot must have one slot");
    Projectors::SetMaterial(swap.projector.ptr(), array[0]);
  }
}
void UnwindTextureCameras();
void RestoreMaterials(std::vector<Swap>& swaps) noexcept;
struct Restore {
  std::vector<Swap>& swaps;
  CameraBindings bindings;
  bool retainMaterials = false;
  explicit Restore(std::vector<Swap>& s) : swaps(s) {}
  void Capture(UnityEngine::Camera* main) {
    bindings.Capture(main);
  }
  ~Restore() noexcept {
    UnwindTextureCameras();
    if (!retainMaterials) RestoreMaterials(swaps);
    if (!bindings.Restore()) failed = true;
    captureEye = -1; imageEffects = nullptr; rendering = false;
  }
};

using ProjectionFunction = void (*)(UnityEngine::Matrix4x4*, bool, UnityEngine::Matrix4x4*);
ProjectionFunction GetProjectionFunction() {
  static auto fn = reinterpret_cast<ProjectionFunction>(il2cpp_functions::resolve_icall("UnityEngine.GL::GetGPUProjectionMatrix_Injected"));
  return fn;
}
int CopySupport() {
  using Function = int (*)();
  static auto fn = reinterpret_cast<Function>(il2cpp_functions::resolve_icall("UnityEngine.SystemInfo::GetCopyTextureSupport"));
  return fn ? fn() : 0;
}
UnityEngine::Matrix4x4 GpuProjection(UnityEngine::Matrix4x4 projection) {
  // The managed wrapper is stripped from Quest's generated headers. Resolve
  // the engine's injected icall rather than guessing API/Y-flip conventions.
  auto fn = GetProjectionFunction();
  if (!fn) throw std::runtime_error("GPU projection icall unavailable");
  UnityEngine::Matrix4x4 result;
  fn(&projection, true, &result);
  return result;
}

// Reuse rooted arrays instead of allocating them once per material per frame.
using Matrix = UnityEngine::Matrix4x4;
struct StereoUniforms {
  std::array<SafePtr<Array<Matrix>>, 8> matrices;
  std::array<SafePtr<Array<Matrix>>, 3> blitMatrices;
  SafePtr<Array<UnityEngine::Vector4>> positions, scale;
  std::array<int, 10> ids{};
  int positionId = 0, scaleId = 0;
  bool initialized = false;
} uniforms;
void UpdateStereo(UnityEngine::Camera* main) {
  if (!uniforms.initialized) {
    char const* names[] = {"unity_StereoMatrixV", "unity_StereoMatrixP", "unity_StereoMatrixVP",
                          "unity_StereoMatrixInvV", "unity_StereoMatrixInvP", "unity_StereoMatrixInvVP",
                          "unity_StereoCameraProjection", "unity_StereoCameraInvProjection", "unity_StereoWorldToCamera", "unity_StereoCameraToWorld"};
    for (int i = 0; i != 10; ++i) uniforms.ids[i] = UnityEngine::Shader::PropertyToID(StringW(std::string(names[i])));
    for (auto& array : uniforms.matrices)
      array = static_cast<Array<Matrix>*>(ArrayW<Matrix>(static_cast<il2cpp_array_size_t>(2)));
    std::array<Matrix, 3> const draw = {Matrix::get_identity(), GpuProjection(Matrix::Ortho(0, 1, 0, 1, -1, 100)),
                                      Matrix::Inverse(GpuProjection(Matrix::Ortho(0, 1, 0, 1, -1, 100)))};
    for (int i = 0; i != 3; ++i) {
      auto values = ArrayW<Matrix>(static_cast<il2cpp_array_size_t>(2));
      values[0] = values[1] = draw[i];
      uniforms.blitMatrices[i] = static_cast<Array<Matrix>*>(values);
    }
    uniforms.positions = static_cast<Array<UnityEngine::Vector4>*>(ArrayW<UnityEngine::Vector4>(static_cast<il2cpp_array_size_t>(2)));
    uniforms.scale = static_cast<Array<UnityEngine::Vector4>*>(ArrayW<UnityEngine::Vector4>(static_cast<il2cpp_array_size_t>(2)));
    uniforms.positionId = UnityEngine::Shader::PropertyToID(u"unity_StereoWorldSpaceCameraPos");
    uniforms.scaleId = UnityEngine::Shader::PropertyToID(u"unity_StereoScaleOffset");
    uniforms.initialized = true;
  }
  for (int eye = 0; eye != 2; ++eye) {
    auto const e = UnityEngine::Camera_StereoscopicEye(eye);
    std::array<Matrix, 8> values;
    values[0] = main->GetStereoViewMatrix(e);
    values[6] = main->GetStereoProjectionMatrix(e);
    values[1] = GpuProjection(values[6]);
    values[2] = Matrix::op_Multiply(values[1], values[0]);
    values[3] = Matrix::Inverse(values[0]);
    values[4] = Matrix::Inverse(values[1]);
    values[5] = Matrix::Inverse(values[2]);
    values[7] = Matrix::Inverse(values[6]);
    for (int i = 0; i != 8; ++i) ArrayW<Matrix>(uniforms.matrices[i].ptr())[eye] = values[i];
    ArrayW<UnityEngine::Vector4>(uniforms.positions.ptr())[eye] = values[3].GetColumn(3);
    ArrayW<UnityEngine::Vector4>(uniforms.scale.ptr())[eye] = UnityEngine::Vector4(1, 1, 0, 0);
  }
}
void BindStereo(UnityEngine::Material* material, StereoBindings::Mask bindings, bool blit) {
  int const index[] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 3};
  // Graphics.Blit's mesh occupies 0..1 and uses an orthographic draw transform.
  // Explicit stereo arrays bypass Unity's normal per-draw matrix update: using
  // the headset's scene VP here projects the screen quad back into the scene.
  // Camera projection/position data remain the real eyes for reconstruction.
  int const drawIndex[] = {0, 1, 1, 0, 2, 2};
  for (int i = 0; i != 10; ++i) if (StereoBindings::Needs(bindings, i)) {
    if (blit && i < 6) {
      material->SetMatrixArray(uniforms.ids[i], ArrayW<Matrix>(uniforms.blitMatrices[drawIndex[i]].ptr()));
    } else material->SetMatrixArray(uniforms.ids[i], ArrayW<Matrix>(uniforms.matrices[index[i]].ptr()));
  }
  if (StereoBindings::Needs(bindings, 10)) material->SetVectorArray(uniforms.positionId, ArrayW<UnityEngine::Vector4>(uniforms.positions.ptr()));
  if (StereoBindings::Needs(bindings, 11)) material->SetVectorArray(uniforms.scaleId, ArrayW<UnityEngine::Vector4>(uniforms.scale.ptr()));
}

UnityEngine::Shader* FindDonor(UnityEngine::Material* source, int mode) {
  auto original = originals.find(source);
  if (original == originals.end()) return nullptr;
  auto* actual = Alive(source) ? source->get_shader().unsafePtr() : nullptr;
  // A known live authored shader assignment wins over the original capture.
  // Repaired stand-ins retain the captured shader's asset identity instead.
  bool const repaired = original->second.fallbackShader && actual == original->second.fallbackShader.ptr();
  auto identity = sourceShaderAssets.find(repaired ? original->second.sourceShader.ptr() : actual);
  if (identity == sourceShaderAssets.end()) identity = sourceShaderAssets.find(original->second.sourceShader.ptr());
  if (identity != sourceShaderAssets.end()) {
    // Unique object aliases are authoritative within matching bundles.
    // Authored asset paths bridge separately built Quest/Windows bundles.
    for (bool privateOnly : {true, false}) for (auto const& path : identity->second.paths) {
      if (path.starts_with(BundleConvert::kRenderDependencyAssetPrefix) != privateOnly) continue;
      auto found = donorAssets[mode].find(path);
      if (found != donorAssets[mode].end()) return found->second.ptr();
    }
  }
  // Official Quest and Windows bundles can have different object IDs/paths.
  // They may use a unique name when no shared asset binding exists. Remember
  // removes ambiguous names, so this never chooses between colliding shaders.
  auto const name = identity != sourceShaderAssets.end()
      ? MaterialSpecialization::Canonical(std::string(identity->second.shader->get_name())) : original->second.shader;
  auto found = shaders[mode].find(name);
  return found != shaders[mode].end() ? found->second.ptr() : nullptr;
}
UnityEngine::Material* CopyMaterial(UnityEngine::Material* source, int eye) {
  if (!Alive(source) || !Alive(source->get_shader().unsafePtr())) return source;
  if (auto clone = cloneSources.find(source); clone != cloneSources.end()) source = clone->second;
  if (!originals.contains(source)) return source; // never substitute native game materials just by name
  auto const name = MaterialShaderName(source);
  auto* donor = FindDonor(source, eye);
  if (!donor) throw std::runtime_error("missing donor shader identity: " + name);
  if (!Alive(donor) || !donor->get_isSupported())
    throw std::runtime_error("eye donor shader is unsupported: " + name);
  auto& pair = materials[source];
  pair.source = source;
  // Scene materials share one preparation snapshot. Blits can observe new
  // values between uses within that snapshot, so refresh those copies on
  // every use. A different donor also invalidates any scene snapshot.
  bool const screenEffect = eye == 3 || eye == 4;
  if (!screenEffect && pair.copies[eye] && Alive(pair.copies[eye].ptr()) &&
      pair.synchronized[eye] == materialEpoch && pair.copies[eye]->get_shader().unsafePtr() == donor)
    return pair.copies[eye].ptr();
  if (!pair.copies[eye] || !Alive(pair.copies[eye].ptr()))
    pair.copies[eye] = UnityEngine::Material::New_ctor(source);
  auto* clone = pair.copies[eye].ptr();
  clone->CopyPropertiesFromMaterial(source); // animated properties, keywords and queue
  if (clone->get_shader().unsafePtr() != donor) clone->set_shader(donor);
  auto original = originals.find(source);
  if (original != originals.end() && original->second.color && original->second.fallbackColor &&
      source->HasProperty(u"_Color")) {
    auto current = source->GetColor(u"_Color");
    auto fallback = *original->second.fallbackColor;
    // Undo the stand-in's grey tint only while it is still that exact value.
    // Later map color animations continue to win.
    if (current.r == fallback.r && current.g == fallback.g && current.b == fallback.b && current.a == fallback.a)
      clone->SetColor(u"_Color", *original->second.color);
  }
  clone->set_enableInstancing(false);
  cloneSources[clone] = source;
  if (eye != 2 && eye != 5) {
    auto bindings = shaderBindings.find(donor);
    BindStereo(clone, bindings == shaderBindings.end() ? StereoBindings::All : bindings->second, eye == 3 || eye == 4);
  }
  pair.synchronized[eye] = materialEpoch;
  return clone;
}

// Snapshot the currently installed arrays, which can themselves be eye clones
// during a nested authored-camera callback. Resolve those back to their source
// before selecting the mono donor, then restore the exact prior arrays.
std::vector<Swap> sceneSwaps;
std::vector<std::vector<Swap>> freeTextureSwaps;
bool ShadowsDisabled() {
  // Getters are stripped from the generated managed API. The Unity 2021.3
  // native bindings return scalar values. Missing bindings mean unknown.
  using Shadows = int (*)(); using Distance = float (*)();
  static auto shadows = reinterpret_cast<Shadows>(il2cpp_functions::resolve_icall("UnityEngine.QualitySettings::get_shadows"));
  static auto distance = reinterpret_cast<Distance>(il2cpp_functions::resolve_icall("UnityEngine.QualitySettings::get_shadowDistance"));
  return (shadows && shadows() == 0) || (distance && distance() <= 0);
}
void PrepareEyeMaterials(std::vector<Swap>& swaps, bool mono) {
  ++materialEpoch;
  for (auto& swap : swaps) {
    if (!swap.active) continue;
    auto saved = ArrayW<UnityEngine::Material*>(swap.original.ptr());
    for (int eye = 0; eye != (mono ? 1 : 2); ++eye) {
      if (!swap.eye[eye] || ArrayW<UnityEngine::Material*>(swap.eye[eye].ptr()).size() != saved.size())
        swap.eye[eye] = static_cast<Array<UnityEngine::Material*>*>(ArrayW<UnityEngine::Material*>(static_cast<il2cpp_array_size_t>(saved.size())));
      auto array = ArrayW<UnityEngine::Material*>(swap.eye[eye].ptr());
      for (int i = 0; i < saved.size(); ++i) array[i] = CopyMaterial(saved[i], mono ? 2 : eye);
    }
  }
}
void CaptureMaterials(std::vector<Swap>& swaps, UnityEngine::Camera* camera) {
  ++nativeCheckEpoch;
  bool const noShadows = ShadowsDisabled();
  uint32_t const mask = Alive(camera) ? static_cast<uint32_t>(camera->get_cullingMask()) : ~uint32_t(0);
  for (auto& swap : swaps) swap.active = false;
  if (!materialList) materialList = MaterialList::New_ctor();
  size_t used = 0;
  for (auto it = sceneRenderers.begin(); it != sceneRenderers.end();) {
    auto* renderer = it->ptr();
    if (!Alive(renderer)) { rendererIndex.erase(renderer); it = sceneRenderers.erase(it); continue; }
    ++it;
    if (!Alive(renderer) || !renderer->get_enabled() || !renderer->get_gameObject()->get_activeInHierarchy()) continue;
    if (!NeedsCameraMaterials(mask, renderer->get_gameObject()->get_layer(), noShadows)) continue;
    renderer->GetSharedMaterials(materialList.ptr());
    int const count = materialList->get_Count();
    // These renderers are registered map/replacement instances. An unknown
    // material must participate in classification, never disappear from the
    // proof and accidentally authorize native rendering.
    if (count == 0) continue;
    if (used == swaps.size()) swaps.emplace_back();
    auto& swap = swaps[used++]; swap.renderer = renderer; swap.projector = nullptr; swap.active = true;
    auto arrayFor = [count](SafePtr<Array<UnityEngine::Material*>>& storage) {
      if (!storage || ArrayW<UnityEngine::Material*>(storage.ptr()).size() != count)
        storage = static_cast<Array<UnityEngine::Material*>*>(ArrayW<UnityEngine::Material*>(static_cast<il2cpp_array_size_t>(count)));
      return ArrayW<UnityEngine::Material*>(storage.ptr());
    };
    auto saved = arrayFor(swap.original);
    for (int i = 0; i < count; ++i) saved[i] = materialList->get_Item(i).unsafePtr();

  }
  // A Projector is not a Renderer. Its own GameObject layer does not select
  // which receiver layers it projects onto, so do not cull it by that layer.
  for (auto it = sceneProjectors.begin(); it != sceneProjectors.end();) {
    auto* projector = it->ptr();
    if (!Alive(projector)) { projectorIndex.erase(projector); it = sceneProjectors.erase(it); continue; }
    ++it;
    if (!projector->get_enabled() || !projector->get_gameObject()->get_activeInHierarchy()) continue;
    auto* material = Projectors::GetMaterial(projector);
    if (!Alive(material)) continue;
    if (used == swaps.size()) swaps.emplace_back();
    auto& swap = swaps[used++]; swap.renderer = nullptr; swap.projector = projector; swap.active = true;
    if (!swap.original || ArrayW<UnityEngine::Material*>(swap.original.ptr()).size() != 1)
      swap.original = static_cast<Array<UnityEngine::Material*>*>(ArrayW<UnityEngine::Material*>(static_cast<il2cpp_array_size_t>(1)));
    ArrayW<UnityEngine::Material*>(swap.original.ptr())[0] = material;
  }
  // Keep inactive slots rooted for reuse as pooled notes enter/leave the scene.
}
void RestoreMaterials(std::vector<Swap>& swaps) noexcept {
  for (auto& swap : swaps) try {
    if (swap.installed && OwnerAlive(swap) && swap.original)
      SetMaterials(swap, ArrayW<UnityEngine::Material*>(swap.original.ptr()));
    swap.installed = false;
  } catch (...) { failed = true; PaperLogger.error("Vivify separate-eye: camera material restore failed"); }
}
struct TextureCameraScope { void const* owner; std::vector<Swap> swaps; };
std::list<TextureCameraScope> textureScopes;
void UnwindTextureCameras() {
  while (!textureScopes.empty()) {
    RestoreMaterials(textureScopes.back().swaps);
    freeTextureSwaps.push_back(std::move(textureScopes.back().swaps)); textureScopes.pop_back();
  }
}

void EnsureTargets(UnityEngine::RenderTextureDescriptor descriptor) {
  int const nextFormat = descriptor.get_graphicsFormat().value__;
  if (width == descriptor.get_width() && height == descriptor.get_height() && format == nextFormat &&
      colors[0] && colors[1] && colors[0]->IsCreated() && colors[1]->IsCreated()) return;
  for (auto& texture : colors) { if (texture) texture->Release(); Destroy(texture); }
  if (staging) staging->Release();
  Destroy(staging);
  width = descriptor.get_width(); height = descriptor.get_height(); format = nextFormat;
  descriptor.set_msaaSamples(1); descriptor.set_depthBufferBits(0);
  descriptor.set_useMipMap(false); descriptor.set_autoGenerateMips(false);
  descriptor.set_useDynamicScale(false); descriptor.set_vrUsage(UnityEngine::VRTextureUsage::None);
  descriptor.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
  descriptor.set_volumeDepth(1); descriptor.set_depthBufferBits(24);
  for (auto& texture : colors) {
    texture = UnityEngine::RenderTexture::New_ctor(descriptor);
    texture->set_name(&texture == &colors[0] ? u"Vivify Eye Color Left" : u"Vivify Eye Color Right");
    if (!texture->Create()) throw std::runtime_error("could not create ordinary eye target");
  }
}

void SyncEyeBloom(UnityEngine::Camera* main, UnityEngine::Camera* eyeCamera, int eye) {
  auto* prior = eyeBloom[eye] ? eyeBloom[eye].ptr() : nullptr;
  auto* copy = SyncCameraBloom(main, eyeCamera, prior);
  eyeBloom[eye] = copy;
  if (copy && copy != prior)
    PaperLogger.info("Vivify separate-eye {}: configured its own native bloom pre-pass", eye);
}
} // namespace

bool IsPrivateMirror(GlobalNamespace::MirrorRendererSO* renderer) { return privateMirrors.contains(renderer); }
GlobalNamespace::MirrorRendererSO* MirrorForCurrentCamera(GlobalNamespace::MirrorRendererSO* source) {
  auto& runtime = Runtime::Instance();
  if (!GetExperimentalSeparateEye() || !Alive(source) || privateMirrors.contains(source) ||
      !runtime.GetCurrentBeatmapData() || runtime.IsResetting() || runtime.IsPauseMenuActive()) return source;
  auto* camera = UnityEngine::Camera::get_current().unsafePtr();
  if (!Alive(camera)) return source;
  auto& entry = mirrorSources[source]; entry.source = source;
  int const id = camera->GetInstanceID();
  auto at = std::find_if(entry.copies.begin(), entry.copies.end(), [&](auto const& item) {
    return item.camera.ptr() == camera && item.cameraId == id && item.renderer && Alive(item.renderer.ptr());
  });
  if (at == entry.copies.end()) {
    auto* copy = UnityEngine::Object::Instantiate<GlobalNamespace::MirrorRendererSO*>(source);
    if (!Alive(copy)) throw std::runtime_error("could not create a camera-specific mirror cache");
    copy->set_hideFlags(UnityEngine::HideFlags::HideAndDontSave);
    // Never inherit a mutable dictionary, camera or temporary texture from
    // the source, even if Unity changes its ScriptableObject clone rules.
    copy->____renderTextures = MirrorCache::New_ctor();
    copy->____mirrorCamera = nullptr;
    copy->____bloomPrePassRenderTexture = nullptr;
    privateMirrors.insert(copy);
    entry.copies.emplace_back(MirrorCopy{camera, copy, id});
    at = std::prev(entry.copies.end());
    PaperLogger.info("Vivify mirror: isolated native reflection cache for camera '{}' ({})", std::string(camera->get_name()), id);
  }
  auto* copy = at->renderer.ptr();
  // Follow the source's live quality/settings values, while each copy retains
  // its own camera, render textures and native lifecycle.
  copy->____reflectLayers = source->____reflectLayers;
  copy->____stereoTextureWidth = source->____stereoTextureWidth;
  copy->____stereoTextureHeight = source->____stereoTextureHeight;
  copy->____monoTextureWidth = source->____monoTextureWidth;
  copy->____monoTextureHeight = source->____monoTextureHeight;
  copy->____maxAntiAliasing = source->____maxAntiAliasing;
  copy->____disableDepthTexture = source->____disableDepthTexture;
  copy->____enableBloomPrePass = source->____enableBloomPrePass;
  copy->____bloomPrePassRenderer = source->____bloomPrePassRenderer;
  copy->____bloomPrePassEffect = source->____bloomPrePassEffect;
  copy->____clearDepthShader = source->____clearDepthShader;
  return copy;
}
std::vector<GlobalNamespace::MirrorRendererSO*> MirrorCopies(GlobalNamespace::MirrorRendererSO* source) {
  std::vector<GlobalNamespace::MirrorRendererSO*> result;
  auto found = mirrorSources.find(source);
  if (found == mirrorSources.end()) return result;
  auto& copies = found->second.copies;
  for (auto it = copies.begin(); it != copies.end();) {
    if (!it->renderer || !Alive(it->renderer.ptr()) || !it->camera || !Alive(it->camera.ptr()) ||
        it->camera->GetInstanceID() != it->cameraId) {
      if (it->renderer && Alive(it->renderer.ptr())) UnityEngine::Object::Destroy(it->renderer.ptr());
      privateMirrors.erase(it->renderer.ptr()); it = copies.erase(it);
    } else { result.push_back(it->renderer.ptr()); ++it; }
  }
  return result;
}
void ForgetMirrorCopies(GlobalNamespace::MirrorRendererSO* source) {
  auto found = mirrorSources.find(source);
  if (found == mirrorSources.end()) return;
  auto copies = std::move(found->second.copies);
  mirrorSources.erase(found);
  for (auto& copy : copies) {
    privateMirrors.erase(copy.renderer.ptr());
    if (copy.renderer && Alive(copy.renderer.ptr())) UnityEngine::Object::Destroy(copy.renderer.ptr());
  }
}

void RememberMaterial(UnityEngine::Material* material) {
  if (!GetExperimentalSeparateEye() || !Alive(material) || originals.contains(material)) return;
  auto* shader = material->get_shader().unsafePtr();
  if (!Alive(shader)) return;
  OriginalMaterial info; info.source = material; info.sourceShader = shader;
  info.shader = MaterialSpecialization::Canonical(std::string(shader->get_name()));
  if (material->HasProperty(u"_Color")) info.color = material->GetColor(u"_Color");
  originals.emplace(material, std::move(info));
}
void RememberAsset(UnityEngine::Object* asset, std::string const& bundlePath) {
  if (!GetExperimentalSeparateEye() || bundlePath.empty()) return;
  auto* shader = AssetShader(asset);
  if (!Alive(shader) || IsDonorShaderName(std::string(shader->get_name()))) return;
  auto [at, inserted] = sourceShaderAssets.try_emplace(shader, SourceShaderAsset{shader, {bundlePath}});
  if (!inserted && std::find(at->second.paths.begin(), at->second.paths.end(), bundlePath) == at->second.paths.end())
    at->second.paths.push_back(bundlePath);
}
void RememberRenderers(UnityEngine::GameObject* root) {
  if (!GetExperimentalSeparateEye() || !Alive(root)) return;
  for (auto* renderer : root->GetComponentsInChildren<UnityEngine::Renderer*>(true))
    if (Alive(renderer) && rendererIndex.emplace(renderer).second) sceneRenderers.emplace_back(renderer);
  for (auto* projector : Projectors::InChildren(root))
    if (Alive(projector) && projectorIndex.emplace(projector).second) sceneProjectors.emplace_back(projector);
}
void RememberFallback(UnityEngine::Material* material) {
  auto original = originals.find(material);
  if (original != originals.end() && Alive(material)) {
    original->second.fallbackShader = material->get_shader().unsafePtr();
    if (material->HasProperty(u"_Color")) original->second.fallbackColor = material->GetColor(u"_Color");
  }
}
void NoteTextureCameras() {} // Authored cameras now use their own scoped mono donors.

bool BeginTextureCamera(void const* owner, UnityEngine::Camera* camera) {
  auto& runtime = Runtime::Instance();
  if (!GetExperimentalSeparateEye() || failed || !preflightComplete || !ready[2] ||
      preparedLevel != runtime.SelectedLevelPath() || runtime.IsResetting() ||
      runtime.IsPauseMenuActive() || GetRecoveryModeActive() || runtime.IsMultiplayerGameplayForEyeRenderer()) return false;
  if (std::any_of(textureScopes.begin(), textureScopes.end(), [owner](auto const& scope) { return scope.owner == owner; })) return true;
  textureScopes.emplace_back(TextureCameraScope{owner, {}});
  auto& scope = textureScopes.back();
  if (!freeTextureSwaps.empty()) { scope.swaps = std::move(freeTextureSwaps.back()); freeTextureSwaps.pop_back(); }
  char const* phase = "capture materials";
  auto reportFailure = [&](std::string const& detail) {
    std::string const cameraName = Alive(camera) ? std::string(camera->get_name()) : "<destroyed>";
    RestoreMaterials(scope.swaps); textureScopes.pop_back(); failed = true;
    PaperLogger.error("Vivify separate-eye: authored texture-camera '{}' setup failed during {}: {}", cameraName, phase, detail);
  };
  try {
    CaptureMaterials(scope.swaps, camera);
    phase = "prepare mono materials";
    PrepareEyeMaterials(scope.swaps, true);
    phase = "install mono materials";
    for (auto& swap : scope.swaps) if (swap.active && OwnerAlive(swap)) {
      swap.installed = true;
      SetMaterials(swap, ArrayW<UnityEngine::Material*>(swap.eye[0].ptr()));
    }
    return true;
  } catch (Il2CppExceptionWrapper const& ex) {
    reportFailure(il2cpp_utils::ExceptionToString(ex.ex));
  } catch (std::exception const& ex) {
    reportFailure(ex.what());
  } catch (...) { reportFailure("unknown native exception"); }
  return false;
}
void EndTextureCamera(void const* owner) {
  auto at = std::find_if(textureScopes.begin(), textureScopes.end(), [owner](auto const& scope) { return scope.owner == owner; });
  if (at == textureScopes.end()) return;
  // Unwind inner scopes first even if Unity disables/destroys an outer camera.
  while (!textureScopes.empty()) {
    bool const last = textureScopes.back().owner == owner;
    RestoreMaterials(textureScopes.back().swaps);
    freeTextureSwaps.push_back(std::move(textureScopes.back().swaps)); textureScopes.pop_back();
    if (last) break;
  }
}
bool CanUseNativeMaterial(UnityEngine::Material* source) {
  if (!source) return true; // unshaded passthrough blit
  if (!completeStageInfo || !Alive(source)) return false;
  auto original = originals.find(source);
  if (original == originals.end()) return false; // unknown provenance is conservative
  auto* shader = source->get_shader().unsafePtr();
  // Callbacks can replace a material's shader after this capture epoch was
  // classified. A cached native permission only belongs to the proven source
  // shader; recheck that live identity before taking the cached shortcut.
  if (!Alive(shader) || shader != original->second.sourceShader.ptr()) return false;
  auto cached = nativeChecks.find(source);
  if (cached != nativeChecks.end() && cached->second.epoch == nativeCheckEpoch) return cached->second.compatible;
  NativeMaterialConditions material;
  material.completeStageInfo = completeStageInfo; material.rememberedSource = true;
  // Prefer upstream's native geometry implementation. Keep our scene path
  // only when its validated tessellation pair adds the requested hull/domain
  // stages; untranslated tessellation alone cannot justify extra scene draws.
  material.sceneEyeTessellation = activeSceneEyeTessellationNames.contains(original->second.shader);
  material.supported = Alive(shader) && shader->get_isSupported();
  material.sourceShaderMatches = Alive(shader) && shader == original->second.sourceShader.ptr() &&
      MaterialSpecialization::Canonical(std::string(shader->get_name())) == original->second.shader;
  bool const compatible = CanReuseNativeMaterial(material);
  nativeChecks[source] = NativeCheck{nativeCheckEpoch, compatible};
  return compatible;
}
UnityEngine::Material* BlitMaterial(UnityEngine::Material* source) {
  int const eye = rendering && captureEye >= 0 ? captureEye : blitEye;
  return eye >= 0 && source ? CopyMaterial(source, eye + 3) : source;
}
bool IsRenderingEye() { return (rendering && captureEye >= 0) || blitEye >= 0; }
bool BeginBlitEye(UnityEngine::Camera* camera, int eye) {
  auto& runtime = Runtime::Instance();
  if (eye < 0 || eye > 1 || rendering || blitEye >= 0 || !GetExperimentalSeparateEye() || failed ||
      !Alive(camera) || !camera->get_stereoEnabled() || !preflightComplete || !ready[3] || !ready[4] ||
      preparedLevel != runtime.SelectedLevelPath() || GetRecoveryModeActive() ||
      !runtime.GetCurrentBeatmapData() || runtime.IsResetting() || runtime.IsPauseMenuActive() ||
      runtime.IsMultiplayerGameplayForEyeRenderer() || (CopySupport() & 5) != 5) return false;
  if (eye == 0) UpdateStereo(camera);
  ++materialEpoch;
  blitEye = eye;
  return true;
}
void EndBlitEye() { blitEye = -1; }
void RenderEyeImage(UnityEngine::Camera* camera, UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  if (!rendering || captureEye < 0 || !imageEffects || camera != cameras[captureEye].ptr()) {
    UnityEngine::Graphics::Blit(src, dest); return;
  }
  try {
    if (captureNeedsDepth) {
      auto* depth = UnityEngine::Shader::GetGlobalTexture(u"_CameraDepthTexture").unsafePtr();
      auto* depthRT = Alive(depth) ? il2cpp_utils::try_cast<UnityEngine::RenderTexture>(depth).value_or(nullptr) : nullptr;
      if (!Alive(depthRT) || depthRT->get_descriptor().get_dimension() != UnityEngine::Rendering::TextureDimension::Tex2D ||
          depthRT->get_width() != src->get_width() || depthRT->get_height() != src->get_height())
        throw std::runtime_error("eye camera did not supply an ordinary scene-depth texture");
      if (!reported)
        PaperLogger.info("Vivify separate-eye {}: depth={}x{}, color={}x{}, rendering map effects in the eye camera callback",
                         captureEye, depthRT->get_width(), depthRT->get_height(), src->get_width(), src->get_height());
    }
    // Execute while Unity's current camera, depth texture, _ZBufferParams and
    // projection uniforms still belong to THIS mono render. Processing the
    // composited array later would read the original main camera's depth.
    ++materialEpoch; // refresh post-effect properties after this scene's callbacks
    imageEffects->RenderEffects(src, dest);
    captureComplete = true;
  } catch (std::exception const& ex) {
    failed = true; PaperLogger.error("Vivify separate-eye image effect failed: {}", ex.what());
  } catch (...) {
    failed = true; PaperLogger.error("Vivify separate-eye image effect failed");
  }
  if (!captureComplete) UnityEngine::Graphics::Blit(src, dest);
}

void Reset(bool unloadDonors) {
  if (rendering || blitEye >= 0) return; // the enclosing camera/effect scope owns restoration
  CancelFrame();
  while (!mirrorSources.empty()) ForgetMirrorCopies(mirrorSources.begin()->first);
  mainDescriptor.reset(); descriptorCamera = nullptr;
  UnwindTextureCameras();
  RestoreMaterials(sceneSwaps);
  sceneSwaps.clear(); freeTextureSwaps.clear();
  sceneRenderers.clear(); rendererIndex.clear(); materialList = nullptr;
  sceneProjectors.clear(); projectorIndex.clear();
  captureEye = blitEye = -1; imageEffects = nullptr;
  cloneSources.clear();
  nativeChecks.clear();
  for (auto& [_, pair] : materials) for (auto& copy : pair.copies) Destroy(copy);
  materials.clear();
  for (auto& camera : cameras) {
    if (camera && Alive(camera.ptr())) UnityEngine::Object::Destroy(camera->get_gameObject());
    camera = nullptr;
  }
  for (auto& bloom : eyeBloom) bloom = nullptr;
  for (auto& texture : colors) { if (texture) texture->Release(); Destroy(texture); }
  if (staging) staging->Release();
  Destroy(staging);
  width = height = 0; format = -1;
  failed = reported = false; rejection.clear(); sceneRoute = SceneRoute::Unreported;
  nativeFrames = separateFrames = 0;
  timingFrames = 0; preparationMs = peakPreparationMs = 0;
  if (unloadDonors) {
    ++generation; preparedLevel.clear(); ready = {};
    pending.reset();
    shaderChecks.clear(); preflightStarted = preflightComplete = preparationErrorShown = false;
    if (preparingGate) SongCore::API::PlayButton::EnablePlayButton(kPlayGate);
    preparingGate = false;
    completeStageInfo = false; separateEyeShaderNames.clear(); tessellationShaderNames.clear();
    activeSceneEyeTessellationNames.clear(); preparedSource.clear(); preparedCache.clear();
    originals.clear();
    sourceShaderAssets.clear();
    shaderBindings.clear();
    for (auto& catalog : bindingCatalogs) catalog.clear();
    for (int mode = 0; mode != 6; ++mode) {
      shaders[mode].clear(); donorAssets[mode].clear(); ambiguousNames[mode].clear();
      for (auto& [_, shader] : donorObjects[mode]) Destroy(shader);
      donorObjects[mode].clear();
    }
    // In-flight Unity requests cannot be cancelled. Poll drains stale loads.
  }
}

static void PrepareImpl(std::string const& level, std::string const& source, std::string const& cache,
                        bool tessellationRequested, bool deviceTessellation, bool force = false) {
  if (!GetExperimentalSeparateEye() || source.empty()) return;
  if (!force && preparedLevel == level) return;
  // Bundle loading has already remembered this selection's original shader
  // names/colors before shader repair. Reset donors without forgetting them.
  auto loadedMaterials = std::move(originals);
  auto loadedShaderAssets = std::move(sourceShaderAssets);
  Reset(true);
  originals = std::move(loadedMaterials);
  sourceShaderAssets = std::move(loadedShaderAssets);
  preparedLevel = level;
  preparedSource = source; preparedCache = cache;
  preparationStarted = std::chrono::steady_clock::now();
  SongCore::API::PlayButton::DisablePlayButton(kPlayGate, "Preparing map effects..."); preparingGate = true;
  uint64_t const epoch = generation;
  std::thread([epoch, level, source, cache, tessellationRequested, deviceTessellation]() {
    std::lock_guard lock(conversionMutex);
    std::array<std::string, 6> paths;
    std::array<BindingCatalog, 6> catalogs;
    bool success = true;
    TessellationCache::Result tessellation;
    BundleConvert::ShaderScan stageScan;
    try {
    stageScan = BundleConvert::ScanShaders(source);
    if (tessellationRequested && !stageScan.tessellationShaderNames.empty()) {
      TessellationCache::Context context{source,
          cache + "|" + BundleConvert::ShaderCacheVersion(kVersion),
          cache + ".tess-scene-pair.proof",
          {cache + ".tess-scene-left.vivify", cache + ".tess-scene-right.vivify"},
          stageScan.tessellationShaderNames};
      tessellation = TessellationCache::Prepare(true, deviceTessellation,
          stageScan.sceneStageInfoComplete && stageScan.glslSourcePrograms == 0, context,
          [&](std::string const& destination, int eye) {
            BundleConvert::ShaderConversionOptions options;
            options.singleViewOnly = true; options.singleViewStereoEye = eye;
            options.namespaceEyeShaders = true; options.renameArchiveFiles = true;
            options.skipQuestUnusedVariants = true; options.exposeRenderDependencies = true;
            options.specializeImmutableMaterialFeatures = MapAllowsImmutableMaterialFeatures(source);
            options.uniformTriangleTessellation = true;
            auto converted = BundleConvert::ConvertShadersToGles(source, destination, options);
            if (!converted.ok() || converted.shadersRefused || converted.variantsRefused)
              PaperLogger.warn("Vivify optional triangle tessellation eye {} declined: {}", eye, converted.message);
            return TessellationCache::Conversion{converted.ok() && !converted.shadersRefused && !converted.variantsRefused,
                converted.tessellationShadersTranslated, converted.tessellationShadersUntranslated,
                std::move(converted.tessellationShaderNamesTranslated)};
          });
      if (tessellation.selected)
        PaperLogger.info("Vivify optional triangle tessellation: {} source shader(s) covered in both scene eyes{}; map-camera and blit stages remain separate", tessellation.covered.size(), tessellation.cached ? " (validated pair cache)" : "");
      else
        PaperLogger.warn("Vivify optional triangle tessellation: {}; retaining normal scene-eye donors for '{}'", tessellation.reason, level);
    }
    // Only our DXBC -> GLSL naming/parameter mapping is proved here. A bundle
    // containing pre-existing GLSL can use other aliases and keeps all calls.
    bool const analyzeBindings = stageScan.sceneStageInfoComplete && stageScan.glslSourcePrograms == 0;
    for (int eye = 0; eye != 6; ++eye) {
      constexpr char const* suffix[] = {".eye-left.vivify", ".eye-right.vivify", ".mono-isolated.vivify",
                                      ".blit-left.vivify", ".blit-right.vivify", ".native-perspective.vivify"};
      paths[eye] = cache + suffix[eye];
      if (eye < 2 && tessellation.selected) {
        paths[eye] = tessellation.paths[eye];
        if (analyzeBindings) catalogs[eye] = BundleConvert::ScanShaders(paths[eye]).stereoBindingMasks;
        continue;
      }
      std::string version; std::ifstream(paths[eye] + ".version") >> version;
      std::string const cacheVersion = BundleConvert::ShaderCacheVersion(
          eye == 5 ? "perspective-native-v7-upstream0159-typed-names" : kVersion);
      if (version == cacheVersion && std::filesystem::exists(paths[eye])) {
        if (analyzeBindings && eye != 2 && eye != 5) catalogs[eye] = BundleConvert::ScanShaders(paths[eye]).stereoBindingMasks;
        continue;
      }
      BundleConvert::ShaderConversionOptions options;
      options.singleViewOnly = true; options.singleViewStereoEye = eye == 2 ? -1 : (eye == 5 ? 0 : (eye >= 3 ? eye - 3 : eye));
      options.namespaceEyeShaders = eye != 2; options.namespaceSingleViewShaders = eye == 2;
      options.singleViewBlit = eye == 3 || eye == 4;
      options.nativePerspectiveDonor = eye == 5;
      options.renameArchiveFiles = true; options.skipQuestUnusedVariants = true;
      // Preserve authored geometry in the explicit-eye donors, independently
      // of the ordinary renderer's upstream flat-geometry fallback setting.
      options.dropGeometryStages = false; options.dropUntranslatableGeometry = false;
      options.specializeImmutableMaterialFeatures = MapAllowsImmutableMaterialFeatures(source);
      options.exposeRenderDependencies = true;
      auto result = BundleConvert::ConvertShadersToGles(source, paths[eye], options);
      if (!result.ok() || result.shadersRefused || result.variantsRefused) {
        PaperLogger.warn("Vivify separate-eye conversion refused '{}': {} ({} shaders / {} variants refused)",
                         level, result.message, result.shadersRefused, result.variantsRefused);
        success = false; break;
      }
      std::ofstream(paths[eye] + ".version") << cacheVersion << '\n';
      if (analyzeBindings && eye != 2 && eye != 5) catalogs[eye] = BundleConvert::ScanShaders(paths[eye]).stereoBindingMasks;
    }
    } catch (std::exception const& ex) {
      PaperLogger.error("Vivify separate-eye conversion exception: {}", ex.what()); success = false;
    } catch (...) { success = false; }
    BSML::MainThreadScheduler::Schedule([epoch, level, paths, success, catalogs = std::move(catalogs),
                                         covered = std::move(tessellation.covered), stageScan = std::move(stageScan)]() mutable {
      if (epoch != generation || level != preparedLevel || !GetExperimentalSeparateEye()) return;
      if (!success) { failed = true; PreparationFailed(); return; }
      completeStageInfo = stageScan.sceneStageInfoComplete;
      bindingCatalogs = std::move(catalogs);
      separateEyeShaderNames = std::unordered_set<std::string>(stageScan.separateEyeShaderNames.begin(), stageScan.separateEyeShaderNames.end());
      tessellationShaderNames = std::move(stageScan.tessellationShaderNames);
      activeSceneEyeTessellationNames = std::unordered_set<std::string>(covered.begin(), covered.end());
      for (auto const& name : tessellationShaderNames) {
        if (activeSceneEyeTessellationNames.contains(name))
          PaperLogger.info("Vivify renderer coverage: '{}' has paired triangle tessellation scene-eye donors; authored-camera/blit tessellation is still untranslated", name);
        else
          PaperLogger.warn("Vivify renderer coverage: '{}' retains untranslated hull/domain tessellation stages; linked GLES programs do not establish PC parity", name);
      }
      PaperLogger.info("Vivify separate-eye classification: complete={}, {} geometry/tessellation shader(s)", completeStageInfo, separateEyeShaderNames.size());
      pending = QueuedLoad{epoch, level, paths};
    });
  }).detach();
}

void Prepare(std::string const& level, std::string const& source, std::string const& cache) {
  if (!GetExperimentalSeparateEye() || source.empty() || preparedLevel == level) return;
  // Capture the user setting and read Unity capability on the main thread.
  // Default off does not query or change any device state.
  bool const requested = GetUniformTriangleTessellation();
  bool supported = false;
  if (requested) {
    try {
      using Supports = bool (*)();
      static auto supports = reinterpret_cast<Supports>(il2cpp_functions::resolve_icall("UnityEngine.SystemInfo::SupportsTessellationShaders"));
      supported = supports && supports();
    } catch (...) {}
  }
  PrepareImpl(level, source, cache, requested, supported);
}

static bool RetryWithoutTessellation() {
  if (activeSceneEyeTessellationNames.empty() || preflightComplete || preparedSource.empty() ||
      Runtime::Instance().GetCurrentBeatmapData() != nullptr) return false;
  auto const level = preparedLevel, source = preparedSource, cache = preparedCache;
  PaperLogger.warn("Vivify optional triangle tessellation: Unity preparation declined the pair; retrying normal scene-eye donors without changing the saved setting");
  PrepareImpl(level, source, cache, false, false, true);
  return true;
}

void PollImpl() {
  if (GetExperimentalSeparateEye() && preparingGate && !preparationErrorShown &&
      std::chrono::steady_clock::now() - preparationStarted > std::chrono::minutes(3)) {
    if (RetryWithoutTessellation()) return;
    failed = true; PaperLogger.error("Vivify renderer preparation timed out; reselect or disable the experimental renderer");
    PreparationFailed();
  }
  if (!GetExperimentalSeparateEye() && preparingGate) {
    SongCore::API::PlayButton::EnablePlayButton(kPlayGate); preparingGate = false;
  }
  for (auto it = loads.begin(); it != loads.end();) {
    auto& load = *it;
    bool const current = load.generation == generation && GetExperimentalSeparateEye();
    if (load.create) {
      if (!load.create->get_isDone()) { ++it; continue; }
      load.bundle = load.create->get_assetBundle().unsafePtr(); load.create = nullptr;
      if (!load.bundle || !current) {
        if (current && !load.bundle) { failed = true; PaperLogger.error("Vivify donor {} asset bundle load failed", load.eye); }
        if (load.bundle) load.bundle->Unload(true);
        it = loads.erase(it); continue;
      }
      load.assets = load.bundle->LoadAllAssetsAsync<UnityEngine::Object*>();
      if (!load.assets) { failed = true; PaperLogger.error("Vivify donor {} asset request could not start", load.eye); }
    }
    if (load.assets && !load.assets->get_isDone()) { ++it; continue; }
    if (current && load.assets) {
      for (auto asset : load.assets->get_allAssets()) Collect(asset.unsafePtr(), load.eye);
      // The conversion preserves authored container keys and material PPtrs.
      // Private dependency aliases expose animation-only references too.
      for (auto path : load.bundle->GetAllAssetNames()) {
        if (!path) continue;
        auto* shader = AssetShader(load.bundle->LoadAsset(path).unsafePtr());
        Remember(shader, load.eye);
        if (donorObjects[load.eye].contains(shader)) donorAssets[load.eye].emplace(std::string(path), shader);
      }
      ready[load.eye] = load.eye == 5 || !donorObjects[load.eye].empty();
      if (!ready[load.eye]) failed = true;
      PaperLogger.info("Vivify separate-eye donor {} ready: {} shader object(s), {} asset bindings, {} ambiguous display name(s)",
                       load.eye, donorObjects[load.eye].size(), donorAssets[load.eye].size(), ambiguousNames[load.eye].size());
    }
    if (load.bundle) load.bundle->Unload(!current);
    it = loads.erase(it);
  }
  // A stale request must finish/unload before a new request can load the same
  // cabinet names. Unity refuses simultaneously loaded duplicate bundles.
  if (loads.empty() && pending) {
    auto next = std::move(*pending); pending.reset();
    if (next.generation != generation || next.level != preparedLevel || !GetExperimentalSeparateEye()) return;
    for (int eye = 0; eye != 6; ++eye) {
      auto* request = UnityEngine::AssetBundle::LoadFromFileAsync(StringW(next.paths[eye]));
      if (request == nullptr) { failed = true; continue; }
      loads.push_back({next.generation, eye, request, {}, {}});
    }
  }
  if (!GetExperimentalSeparateEye() || preparedLevel.empty()) return;
  if (failed) { if (RetryWithoutTessellation()) return; if (preparingGate) PreparationFailed(); return; }
  bool const donorsReady = loads.empty() && !pending && std::all_of(ready.begin(), ready.end(), [](bool value) { return value; });
  if (!donorsReady || preflightComplete) return;
  if (!preflightStarted) {
    bool needsSeparate = !completeStageInfo;
    for (auto const& name : separateEyeShaderNames) if (!shaders[5].contains(name)) needsSeparate = true;
    if (needsSeparate && (!GetProjectionFunction() || (CopySupport() & 5) != 5)) {
      failed = true; PaperLogger.error("Vivify renderer preflight: required projection/cross-type copy support unavailable");
      PreparationFailed(); return;
    }
    for (int mode = 0; mode != 5; ++mode) for (auto const& [_, shader] : donorObjects[mode]) shaderChecks.emplace_back(shader.ptr());
    preflightStarted = true;
  }
  // Spread support queries across menu frames. This is a capability check,
  // not a claim that all keyword variants have been drawn or warmed on GPU.
  for (int budget = 0; budget != 4 && !shaderChecks.empty(); ++budget) {
    auto* shader = shaderChecks.front().ptr();
    if (!Alive(shader) || !shader->get_isSupported()) {
      failed = true;
      PaperLogger.error("Vivify renderer preflight: shader '{}' is unsupported", Alive(shader) ? std::string(shader->get_name()) : "<destroyed>");
      if (RetryWithoutTessellation()) return;
      PreparationFailed(); return;
    }
    shaderChecks.pop_front();
  }
  if (shaderChecks.empty()) {
    preflightComplete = true;
    SongCore::API::PlayButton::EnablePlayButton(kPlayGate); preparingGate = false;
    if (tessellationShaderNames.empty())
      PaperLogger.info("Vivify renderer preflight complete: map effects ready; GPU image/performance validation is still required");
    else
      PaperLogger.info("Vivify renderer preflight complete: {} source tessellation shader(s), {} paired scene-eye shader(s), {} untranslated in scene eyes; authored-camera/blit tessellation and PC parity remain separate", tessellationShaderNames.size(), ActiveSceneEyeTessellationShaderCount(), UntranslatedTessellationShaderCount());
  }
}

std::string PreparationState() {
  return failed ? "failed" : preflightComplete ? "ready" : preparedLevel.empty() ? "not-requested" : "preparing";
}
int PreparedDonorCount() { return static_cast<int>(std::count(ready.begin(), ready.end(), true)); }
int SourceTessellationShaderCount() { return static_cast<int>(tessellationShaderNames.size()); }
int ActiveSceneEyeTessellationShaderCount() { return preflightComplete && !failed ? static_cast<int>(activeSceneEyeTessellationNames.size()) : 0; }
int UntranslatedTessellationShaderCount() {
  return static_cast<int>(std::count_if(tessellationShaderNames.begin(), tessellationShaderNames.end(), [](auto const& name) {
    return !preflightComplete || failed || !activeSceneEyeTessellationNames.contains(name);
  }));
}
std::vector<UnityEngine::Shader*> PreparedShadersForDiagnostics() {
  std::vector<UnityEngine::Shader*> result;
  if (!preflightComplete) return result;
  for (auto const& map : donorObjects) for (auto const& [_, shader] : map) if (shader && Alive(shader.ptr())) result.push_back(shader.ptr());
  return result;
}

void Poll() {
  try { PollImpl(); }
  catch (std::exception const& ex) {
    failed = true; PaperLogger.error("Vivify renderer preparation failed: {}", ex.what());
    if (RetryWithoutTessellation()) return;
    if (GetExperimentalSeparateEye() && !preparedLevel.empty()) PreparationFailed();
  } catch (...) {
    failed = true; PaperLogger.error("Vivify renderer preparation failed with an unknown exception");
    if (RetryWithoutTessellation()) return;
    if (GetExperimentalSeparateEye() && !preparedLevel.empty()) PreparationFailed();
  }
}

bool CanUsePerspectiveEmpty(UnityEngine::Material* source, bool perspective, bool noShadows) {
  if (!Alive(source)) return false;
  auto original = originals.find(source);
  if (original == originals.end()) return false;
  auto* donor = FindDonor(source, 5);
  EmptyGeometryConditions conditions;
  conditions.provedEveryVariant = completeStageInfo && ready[5] && Alive(donor);
  conditions.perspectiveCamera = perspective; conditions.shadowsDisabled = noShadows; conditions.rememberedSource = true;
  conditions.donorSupported = Alive(donor) && donor->get_isSupported();
  return CanUseEmptyGeometry(conditions);
}
void InstallNativeMaterials(std::vector<Swap>& swaps, UnityEngine::Camera* main, bool perspective, bool noShadows) {
  ++materialEpoch;
  for (auto& swap : swaps) {
    if (!swap.active) continue;
    auto saved = ArrayW<UnityEngine::Material*>(swap.original.ptr());
    bool changes = false;
    for (auto* source : saved) if (!CanUseNativeMaterial(source) && CanUsePerspectiveEmpty(source, perspective, noShadows)) changes = true;
    if (!changes) continue;
    if (!swap.eye[0] || ArrayW<UnityEngine::Material*>(swap.eye[0].ptr()).size() != saved.size())
      swap.eye[0] = static_cast<Array<UnityEngine::Material*>*>(ArrayW<UnityEngine::Material*>(static_cast<il2cpp_array_size_t>(saved.size())));
    auto array = ArrayW<UnityEngine::Material*>(swap.eye[0].ptr());
    for (int i = 0; i < saved.size(); ++i)
      array[i] = CanUsePerspectiveEmpty(saved[i], perspective, noShadows) ? CopyMaterial(saved[i], 5) : saved[i];
    // Arm restoration before a setter can throw after partially applying.
    nativeMaterialsInstalled = true; nativeMaterialCamera = main; swap.installed = true;
    SetMaterials(swap, array);
  }
}

bool RenderEyes(UnityEngine::Camera* main, UnityEngine::RenderTextureDescriptor desc, CameraApplier* effects) {
  if (!GetExperimentalSeparateEye() || rendering || !Alive(main) || !Alive(effects)) return false;
  auto& runtime = Runtime::Instance();
  FrameConditions frame;
  frame.requested = true;
  frame.ready = !failed && preflightComplete && std::all_of(ready.begin(), ready.end(), [](bool value) { return value; }) && preparedLevel == runtime.SelectedLevelPath();
  frame.playing = runtime.GetCurrentBeatmapData() != nullptr;
  frame.paused = runtime.IsPauseMenuActive(); frame.resetting = runtime.IsResetting();
  frame.multiplayer = runtime.IsMultiplayerGameplayForEyeRenderer();
  frame.recovery = GetRecoveryModeActive();
  // A probe renders additional cameras while our material swaps are active.
  // Its per-face uniforms/compositing are outside this prototype's scope.
  for (auto* probe : UnityEngine::Object::FindObjectsOfType<UnityEngine::ReflectionProbe*>())
    if (Alive(probe) && probe->get_enabled() && probe->get_mode().value__ == 1) frame.reflections = true;
  frame.midEffects = runtime.HasMidRenderEffects(); frame.secondaryCameras = runtime.HasSecondaryCameras();
  frame.sharedEffectTargets = runtime.HasSharedEyeEffectTargets();
  frame.externalCommands = main->get_commandBufferCount() != 0;
  frame.clearsBackground = main->get_clearFlags() == UnityEngine::CameraClearFlags::SolidColor;
  auto rect = main->get_rect();
  frame.croppedViewport = rect.m_XMin != 0 || rect.m_YMin != 0 || rect.m_Width != 1 || rect.m_Height != 1 ||
                          UnityEngine::XR::XRSettings::get_renderViewportScale() != 1;
  bool const needsDepth = runtime.MapShadersNeedSceneDepth().value_or(true) || main->get_depthTextureMode().value__ != 0;
  frame.stereo = main->get_stereoEnabled();
  frame.twoLayerArray = desc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray &&
                        desc.get_volumeDepth() == 2;
  frame.resolved = desc.get_msaaSamples() == 1;
  // This SystemInfo wrapper/enum is stripped from the game's managed API.
  // Unity 2021.3 CopyTextureSupport: Basic=1, DifferentTypes=4.
  int const copy = CopySupport();
  frame.copySupported = (copy & 1) != 0 && (copy & 4) != 0;
  auto reason = Rejection(frame, false);
  if (!reason.empty()) { LogRejection(reason); return false; }
  auto& swaps = sceneSwaps;
  rendering = true;
  Restore restore(swaps);
  try {
    CaptureMaterials(swaps, main);
    NativeSceneConditions native;
    native.completeStageInfo = completeStageInfo;
    native.nativeMultiview = UnityEngine::XR::XRSettings::get_stereoRenderingMode().value__ == 3;
    native.effectsCompatible = runtime.CanUseNativeStereoEffects();
    native.sceneMaterialsCompatible = true;
    std::vector<std::string> nativeBlockers;
    bool const perspective = !main->get_orthographic(), noShadows = ShadowsDisabled();
    for (auto const& swap : swaps) {
      if (!swap.active) continue;
      for (auto* source : ArrayW<UnityEngine::Material*>(swap.original.ptr())) {
        if (!CanUseNativeMaterial(source) && !CanUsePerspectiveEmpty(source, perspective, noShadows)) {
          native.sceneMaterialsCompatible = false;
          if (sceneRoute != SceneRoute::Separate && nativeBlockers.size() < 8) {
            auto remembered = originals.find(source);
            auto* actual = Alive(source) ? source->get_shader().unsafePtr() : nullptr;
            nativeBlockers.push_back(fmt::format("material='{}' sourceShader='{}' currentShader='{}' remembered={} geometryOrTessellation={}",
                Alive(source) ? std::string(source->get_name()) : "<null>", MaterialShaderName(source),
                Alive(actual) ? std::string(actual->get_name()) : "<null>", remembered != originals.end(),
                remembered != originals.end() && separateEyeShaderNames.contains(remembered->second.shader)));
          }
        }
      }
    }
    if (CanReuseMultiview(native)) {
      InstallNativeMaterials(swaps, main, perspective, noShadows);
      restore.retainMaterials = true;
      ++nativeFrames;
      if (sceneRoute != SceneRoute::Native) {
        PaperLogger.info("Vivify scene route: native multiview; no separate-eye scene renders needed for this frame");
        sceneRoute = SceneRoute::Native;
      }
      return false;
    }
    if (auto separateReason = Rejection(frame); !separateReason.empty()) {
      LogRejection(separateReason); return false;
    }
    restore.Capture(main);
    EnsureTargets(desc);
    UpdateStereo(main);
    PrepareEyeMaterials(swaps, false);
    if (sceneRoute != SceneRoute::Separate) {
      PaperLogger.info("Vivify scene route: separate eyes; native compatibility was not established");
      PaperLogger.info("Vivify scene route detail: perspective={} shadowsDisabled={} effectsCompatible={} image={}x{} XR={}x{} viewportScale={} bloom={}",
                       perspective, noShadows, native.effectsCompatible, desc.get_width(), desc.get_height(),
                       UnityEngine::XR::XRSettings::get_eyeTextureWidth(), UnityEngine::XR::XRSettings::get_eyeTextureHeight(),
                       UnityEngine::XR::XRSettings::get_renderViewportScale(), effects->hasMainEffect);
      for (auto const& blocker : nativeBlockers) PaperLogger.info("Vivify native scene blocker: {}", blocker);
      sceneRoute = SceneRoute::Separate;
    }
    ++separateFrames;
    if (runtime.GetPreEffectsEmpty() && runtime.GetPostEffectsEmpty() &&
        std::none_of(swaps.begin(), swaps.end(), [](auto const& swap) { return swap.active; })) return false;
    imageEffects = effects; captureNeedsDepth = needsDepth;
    for (int eye = 0; eye != 2; ++eye) {
      if (!cameras[eye] || !Alive(cameras[eye].ptr())) {
        auto* go = UnityEngine::GameObject::New_ctor(u"Vivify Experimental Eye Camera");
        go->set_hideFlags(UnityEngine::HideFlags::HideAndDontSave);
        cameras[eye] = go->AddComponent<UnityEngine::Camera*>();
        go->AddComponent<SeparateEyeCapture*>();
      }
      auto* camera = cameras[eye].ptr();
      camera->CopyFrom(main); camera->set_enabled(false);
      camera->RemoveAllCommandBuffers();
      camera->set_stereoTargetEye(UnityEngine::StereoTargetEyeMask::None);
      camera->set_targetTexture(colors[eye].ptr());
      camera->set_depthTextureMode(UnityEngine::DepthTextureMode(main->get_depthTextureMode().value__ | (needsDepth ? 1 : 0)));
      camera->set_allowMSAA(false); camera->set_allowDynamicResolution(false);
      camera->set_rect(UnityEngine::Rect(0, 0, 1, 1));
      auto const e = UnityEngine::Camera_StereoscopicEye(eye);
      auto view = main->GetStereoViewMatrix(e);
      auto inverse = UnityEngine::Matrix4x4::Inverse(view);
      auto position = inverse.GetColumn(3);
      auto back = inverse.GetColumn(2), up = inverse.GetColumn(1);
      camera->get_transform()->set_position(UnityEngine::Vector3(position.x, position.y, position.z));
      camera->get_transform()->set_rotation(UnityEngine::Quaternion::LookRotation(
          UnityEngine::Vector3(-back.x, -back.y, -back.z), UnityEngine::Vector3(up.x, up.y, up.z)));
      camera->set_worldToCameraMatrix(view);
      auto projection = main->GetStereoProjectionMatrix(e);
      camera->set_projectionMatrix(projection);
      camera->set_nonJitteredProjectionMatrix(main->GetStereoNonJitteredProjectionMatrix(e));
      camera->set_cullingMatrix(UnityEngine::Matrix4x4::op_Multiply(projection, view));
      SyncEyeBloom(main, camera, eye);
      for (auto& swap : swaps) if (swap.active && OwnerAlive(swap)) {
        swap.installed = true;
        SetMaterials(swap, ArrayW<UnityEngine::Material*>(swap.eye[eye].ptr()));
      }
      captureEye = eye; captureComplete = false;
      {
        // The original main camera has not reached OnPreRender yet. Set its
        // bloom/fade shader state for scene drawing as well as image filtering.
        struct SceneEffect {
          GlobalNamespace::MainEffectController* controller;
          explicit SceneEffect(CameraApplier* effects) : controller(effects->hasMainEffect && Alive(effects->mainEffectController)
                                                                    ? effects->mainEffectController : nullptr) {
            if (controller) controller->OnPreRender();
          }
          ~SceneEffect() { if (Alive(controller)) controller->OnPostRender(); }
        } sceneEffect(effects);
        camera->Render();
      }
      captureEye = -1;
      if (failed || !captureComplete) throw std::runtime_error("eye image callback did not complete");
      RestoreMaterials(swaps);
    }
    // Both complete images stay in their ordinary targets until presentation.
    // Unity's XR-aware blit presents them; no third scene draw is needed.
    if (!reported) {
      PaperLogger.warn("Vivify separate-eye prototype: two scene renders completed before main culling, {} tracked material renderer(s); Quest validation required", swaps.size());
      reported = true;
    }
    return true;
  } catch (std::exception const& ex) {
    failed = true;
    PaperLogger.error("Vivify separate-eye prototype disabled for this session: {}", ex.what());
  } catch (...) {
    failed = true;
    PaperLogger.error("Vivify separate-eye prototype disabled after a rendering exception");
  }
  return false;
}
void RestoreMainCamera(UnityEngine::Camera* main) {
  if (nativeMaterialsInstalled && (!main || main == nativeMaterialCamera.ptr())) {
    RestoreMaterials(sceneSwaps); nativeMaterialsInstalled = false; nativeMaterialCamera = nullptr;
  }
  if (!suppressed || (main && main != suppressedCamera.ptr())) return;
  auto* camera = suppressedCamera ? suppressedCamera.ptr() : nullptr;
  try {
    if (Alive(camera)) {
      camera->set_cullingMask(suppressedMask);
      camera->set_depthTextureMode(UnityEngine::DepthTextureMode(suppressedDepth));
    }
    if (suppressedBloom && Alive(suppressedBloom.ptr())) suppressedBloom->set_enabled(suppressedBloomEnabled);
    suppressedBloom = nullptr;
    suppressed = false; suppressedCamera = nullptr;
  } catch (...) {
    failed = true;
    PaperLogger.error("Vivify separate-eye: restoring the main camera failed; retrying at the next lifecycle boundary");
  }
}
void RejectFixedMaterialFeatures() {
  failed = true;
  CancelFrame();
  PreparationFailed();
}
void RejectCameraBindings() {
  failed = true;
  CancelFrame();
  PaperLogger.error("Vivify separate-eye: camera binding restoration failed; experimental rendering disabled");
}

void CancelFrame() {
  RestoreMainCamera(); completedFrame.Clear();
}

void PrepareFrame(UnityEngine::Camera* main, CameraApplier* effects) {
  if (rendering) return;
  CancelFrame();
  if (!GetExperimentalSeparateEye() || !Alive(main) || !Alive(effects) || !mainDescriptor ||
      !descriptorCamera || descriptorCamera.ptr() != main) return;
  // Learn the actual image-effect descriptor from a normal frame. Do not guess
  // HDR/array formats from XR's display descriptor. Relearn after quality changes.
  if (main->get_allowHDR() != descriptorHdr || UnityEngine::XR::XRSettings::get_eyeTextureWidth() != xrWidth ||
      UnityEngine::XR::XRSettings::get_eyeTextureHeight() != xrHeight ||
      UnityEngine::XR::XRSettings::get_renderViewportScale() != xrScale) {
    mainDescriptor.reset(); return;
  }
  int const frame = UnityEngine::Time::get_frameCount();
  completedFrame.Begin(frame, reinterpret_cast<uintptr_t>(main));
  auto const start = std::chrono::steady_clock::now();
  if (!RenderEyes(main, *mainDescriptor, effects) || failed) { completedFrame.Clear(); return; }
  double const elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  preparationMs += elapsed; peakPreparationMs = std::max(peakPreparationMs, elapsed);
  if (++timingFrames == 300) {
    PaperLogger.info("Vivify separate-eye performance: 300 prepared frames, eye preparation CPU wall time avg={:.2f}ms max={:.2f}ms (not GPU frame time)",
                     preparationMs / timingFrames, peakPreparationMs);
    PaperLogger.info("Vivify scene-route counts: native={} separate={}", nativeFrames, separateFrames);
    timingFrames = 0; preparationMs = peakPreparationMs = 0;
  }
  completedFrame.Complete();
  // No culling state is touched on failure. A complete, current stereo image
  // exists before skipping the redundant main-camera geometry and depth pass.
  if (!completedFrame.Ready(frame, reinterpret_cast<uintptr_t>(main))) return;
  try {
    suppressedCamera = main;
    suppressedMask = main->get_cullingMask(); suppressedDepth = main->get_depthTextureMode().value__;
    suppressed = true;
    main->set_cullingMask(0);
    main->set_depthTextureMode(UnityEngine::DepthTextureMode::None);
    auto* bloom = main->GetComponent<GlobalNamespace::BloomPrePass*>();
    if (Alive(bloom)) {
      suppressedBloom = bloom; suppressedBloomEnabled = bloom->get_enabled();
      bloom->set_enabled(false); // the completed eyes already contain their lighting
    }
  } catch (...) {
    CancelFrame(); failed = true;
    PaperLogger.error("Vivify separate-eye: main draw suppression failed; keeping normal render");
  }
}

bool TryRender(UnityEngine::Camera* main, UnityEngine::RenderTexture* source, CameraApplier*) {
  if (!Alive(main) || !Alive(source)) { CancelFrame(); return false; }
  RestoreMainCamera(main); // also covers engines that run image filters before OnPostRender
  auto actual = source->get_descriptor();
  mainDescriptor = actual; descriptorCamera = main; descriptorHdr = main->get_allowHDR();
  xrWidth = UnityEngine::XR::XRSettings::get_eyeTextureWidth(); xrHeight = UnityEngine::XR::XRSettings::get_eyeTextureHeight();
  xrScale = UnityEngine::XR::XRSettings::get_renderViewportScale();
  bool const current = completedFrame.Ready(UnityEngine::Time::get_frameCount(), reinterpret_cast<uintptr_t>(main));
  completedFrame.Clear();
  if (!current || !colors[0] || !colors[1]) return false;
  try {
    auto expected = colors[0]->get_descriptor();
    // Raw copies are only between ordinary, non-XR textures. PC Vivify
    // presents through Unity's blit path; do the same at the XR boundary so
    // Unity applies its target/viewport conventions. Matching width/format
    // alone does not prove a raw copy into an XR target is equivalent.
    if (!staging || !staging->IsCreated()) {
      expected.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2DArray);
      expected.set_volumeDepth(2); expected.set_depthBufferBits(0);
      staging = UnityEngine::RenderTexture::New_ctor(expected);
      staging->set_name(u"Vivify Stereo Staging");
      if (!staging->Create()) throw std::runtime_error("could not create presentation staging array");
    }
    for (int eye = 0; eye != 2; ++eye)
      UnityEngine::Graphics::CopyTexture(colors[eye].ptr(), 0, 0, staging.ptr(), eye, 0);
    Runtime::Instance().ApplyBlits(staging.ptr(), source, "_Main", -1);
    if (actual.get_width() != expected.get_width() || actual.get_height() != expected.get_height() ||
        actual.get_graphicsFormat() != expected.get_graphicsFormat()) {
      LogRejection("late image descriptor change; presenting scaled completed frame and relearning targets");
    }
    return true;
  } catch (...) {
    failed = true;
    PaperLogger.error("Vivify separate-eye: completed-frame presentation failed; normal rendering resumes next frame");
    return false;
  }
}
} // namespace Vivify::SeparateEye
