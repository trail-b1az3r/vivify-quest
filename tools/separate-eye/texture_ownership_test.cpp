// Actual production cleanup methods with API stand-ins; no Unity/GPU runs.
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include "VivifyRenderTexturePolicy.hpp"

template<class T> struct Ptr {
  T* value;
  T* unsafePtr() const { return value; }
  operator T*() const { return value; }
};
namespace Vivify { struct SecondaryCameraController; }
namespace UnityEngine {
struct Texture { bool alive = true; };
struct Shader {
  static std::unordered_map<int, Texture*> bindings;
  static int writes;
  static Ptr<Texture> GetGlobalTexture(int id) { return {bindings[id]}; }
  static void SetGlobalTexture(int id, Texture* texture) { bindings[id] = texture; ++writes; }
};
std::unordered_map<int, Texture*> Shader::bindings;
int Shader::writes = 0;
enum class RenderTextureFormat { RFloat = 1, ARGBHalf = 2 };
struct SystemInfo { static bool SupportsRenderTextureFormat(RenderTextureFormat) { return true; } };
struct CaptureDescriptor {
  Vivify::RenderTexturePolicy::Descriptor value{512, 256, 1, 1, 1, 3, 0, 24, 2};
  void set_msaaSamples(int n) { value.samples = n; }
  void set_colorFormat(RenderTextureFormat n) { value.color = static_cast<int>(n); }
  void set_depthBufferBits(int n) { value.depth = n; }
};
struct RenderTexture : Texture {
  CaptureDescriptor descriptor;
  CaptureDescriptor get_descriptor() const { return descriptor; }
  bool IsCreated() const { return created; }
  static int allocationMode, allocations;
  static std::vector<RenderTexture*> allocated;
  static RenderTexture* New_ctor(CaptureDescriptor descriptor) {
    ++allocations;
    if (allocationMode == 1) return nullptr;
    if (allocationMode == 3) throw std::runtime_error("injected allocation failure");
    auto* result = new RenderTexture;
    result->descriptor = descriptor; result->created = false;
    allocated.push_back(result); return result;
  }
  bool Create() { created = allocationMode != 2; return created; }
  bool created = true;
  int releases = 0, destroys = 0, boundDuringRelease = 0;
  void Release() {
    for (auto const& [_, current] : Shader::bindings) if (current == this) ++boundDuringRelease;
    ++releases; created = false;
  }
};
int RenderTexture::allocationMode = 0, RenderTexture::allocations = 0;
std::vector<RenderTexture*> RenderTexture::allocated;
struct GameObject {
  bool alive = true, destroyQueued = false;
  Vivify::SecondaryCameraController* controller = nullptr;
  template<class T> T GetComponent() { return static_cast<T>(controller); }
};
struct Camera {
  bool alive = true;
  GameObject* owner = nullptr;
  Ptr<GameObject> get_gameObject() { return {owner}; }
};
struct Object {
  static void Destroy(RenderTexture* texture) { ++texture->destroys; texture->alive = false; }
  // Leave native resources for the explicitly simulated OnDestroy callback.
  static void Destroy(GameObject* object) { object->destroyQueued = true; }
};
}
template<class T> bool IsAlive(T* p) { return p && p->alive; }
template<class T> bool IsManagedAlive(T* p) { return IsAlive(p); }
#include "VivifyTextureBindings.hpp"

namespace Vivify {
namespace RenderTexturePolicy {
// Descriptor stand-in only; actual allocation/cleanup control flow is extracted.
Descriptor Describe(UnityEngine::CaptureDescriptor const& d) { return d.value; }
}
struct DeclaredTextureData {
  int propertyId = 0;
  UnityEngine::RenderTexture* texture = nullptr;
  std::optional<int> allocation;
};
struct SecondaryCameraData {
  UnityEngine::Camera* camera = nullptr;
  std::optional<int> texturePropertyId, depthTexturePropertyId;
  UnityEngine::RenderTexture* colorRT = nullptr;
  UnityEngine::RenderTexture* depthRT = nullptr;
};
struct SecondaryCameraController {
  bool alive = true;
  std::optional<int> colorKey, depthKey;
  std::unordered_map<int, UnityEngine::RenderTexture*> colorTextures, depthTextures;
  std::unordered_map<int, RenderTexturePolicy::Descriptor> _colorAllocations, _depthAllocations;
  UnityEngine::RenderTexture* EnsureEyeTexture(std::unordered_map<int, UnityEngine::RenderTexture*>&,
                                              int, UnityEngine::RenderTexture*, bool);
  void ReleaseGlobalBindings();
  void OnDestroy();
};
struct Runtime {
  std::unordered_map<UnityEngine::Camera*, int> _cameraDefaults;
  void ReleaseDeclaredTextureData(DeclaredTextureData&);
  void ReleaseSecondaryCameraData(SecondaryCameraData&);
};
#include "ActualEnsureEyeTexture.inc"
#include "ActualReleaseBindings.inc"
#include "ActualControllerDestroy.inc"
#include "ActualReleaseDeclared.inc"
#include "ActualReleaseCamera.inc"
}

int main() {
  using namespace Vivify;
  using namespace UnityEngine;
  int checks = 0, failures = 0;
  auto check = [&](bool result, char const* label) {
    ++checks;
    if (!result) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
  };
  auto reset = [] { Shader::bindings.clear(); Shader::writes = 0; };
  for (int property : {-103, 17, 10000}) {
    Runtime runtime;
    {
      reset(); RenderTexture owned;
      DeclaredTextureData screen{property, &owned, 123}; Shader::bindings[property] = &owned;
      runtime.ReleaseDeclaredTextureData(screen);
      check(!Shader::bindings[property], "declared cleanup clears its own live binding");
      check(!owned.alive && owned.releases == 1 && owned.destroys == 1 && !owned.boundDuringRelease,
            "declared texture unbound before release");
      check(!screen.texture && !screen.allocation, "declared native handle and allocation forgotten");
      RenderTexture replacement; Shader::bindings[property] = &replacement;
      runtime.ReleaseDeclaredTextureData(screen);
      check(Shader::bindings[property] == &replacement, "repeated declared cleanup preserves successor");
    }
    {
      reset(); RenderTexture old, replacement;
      DeclaredTextureData screen{property, &old, 123}; Shader::bindings[property] = &replacement;
      runtime.ReleaseDeclaredTextureData(screen);
      check(Shader::bindings[property] == &replacement && replacement.alive && !replacement.releases,
            "declared cleanup preserves another owner's replacement");
      check(!old.alive && !old.boundDuringRelease && !Shader::writes, "old screen still released without writing foreign binding");
      screen.allocation = 456; runtime.ReleaseDeclaredTextureData(screen);
      check(Shader::bindings[property] == &replacement && !screen.allocation, "unallocated screen cleanup cannot erase another owner");
    }
    for (int eye : {0, 1, 2}) {
      reset(); RenderTexture color, depth;
      SecondaryCameraController controller;
      controller.colorKey = property; controller.depthKey = property + 1000;
      controller.colorTextures[eye] = &color; controller.depthTextures[eye] = &depth;
      controller._colorAllocations[eye] = {}; controller._depthAllocations[eye] = {};
      Shader::bindings[property] = &color; Shader::bindings[property + 1000] = &depth;
      controller.OnDestroy();
      check(!Shader::bindings[property] && !Shader::bindings[property + 1000], "external camera destruction clears owned color and depth");
      check(!color.boundDuringRelease && !depth.boundDuringRelease, "camera unbinds images before releasing storage");
      check(!color.alive && !depth.alive && color.releases == 1 && depth.releases == 1, "external camera destruction releases native images once");
      check(controller.colorTextures.empty() && controller.depthTextures.empty() &&
            controller._colorAllocations.empty() && controller._depthAllocations.empty(), "camera teardown clears every eye and allocation entry");
    }
    {
      reset(); RenderTexture color, depth;
      SecondaryCameraController controller;
      controller.colorKey = controller.depthKey = property;
      controller.colorTextures[0] = &color; controller.depthTextures[1] = &depth;
      Shader::bindings[property] = &depth;
      controller.OnDestroy();
      check(!Shader::bindings[property] && Shader::writes == 1, "aliased color and depth key clears only the bound image");
      check(!color.boundDuringRelease && !depth.boundDuringRelease, "aliased key has no dangling capture on destruction");
    }
    {
      reset(); RenderTexture oldColor, oldDepth, replacementColor, replacementDepth;
      SecondaryCameraController oldController;
      oldController.colorKey = property; oldController.depthKey = property + 1000;
      oldController.colorTextures[0] = &oldColor; oldController.depthTextures[1] = &oldDepth;
      GameObject oldObject; oldObject.controller = &oldController;
      Camera oldCamera; oldCamera.owner = &oldObject;
      SecondaryCameraData old; old.camera = &oldCamera;
      old.texturePropertyId = property; old.depthTexturePropertyId = property + 1000;
      runtime._cameraDefaults[&oldCamera] = 1;
      Shader::bindings[property] = &replacementColor; Shader::bindings[property + 1000] = &replacementDepth;
      runtime.ReleaseSecondaryCameraData(old);
      check(Shader::bindings[property] == &replacementColor && Shader::bindings[property + 1000] == &replacementDepth,
            "runtime camera cleanup preserves replacement color and depth");
      check(!old.camera && !old.colorRT && !old.depthRT && !runtime._cameraDefaults.contains(&oldCamera) && oldObject.destroyQueued,
            "runtime camera state removed and destruction scheduled");
      check(oldColor.alive && oldDepth.alive && !oldColor.releases && !oldDepth.releases,
            "controller images retained until destruction callback");
      oldController.OnDestroy();
      check(Shader::bindings[property] == &replacementColor && Shader::bindings[property + 1000] == &replacementDepth,
            "deferred old camera destruction preserves successor");
      check(!oldColor.alive && !oldDepth.alive && replacementColor.alive && replacementDepth.alive,
            "deferred cleanup destroys only old capture images");
      runtime.ReleaseSecondaryCameraData(old); oldController.OnDestroy();
      check(Shader::bindings[property] == &replacementColor && !replacementColor.releases,
            "repeated camera cleanup cannot clear a successor");
    }
    {
      reset(); RenderTexture color, depth;
      SecondaryCameraController controller;
      controller.colorKey = property; controller.depthKey = property + 1000;
      controller.colorTextures[2] = &color; controller.depthTextures[2] = &depth;
      GameObject object; object.controller = &controller; Camera camera; camera.owner = &object;
      SecondaryCameraData data; data.camera = &camera; data.texturePropertyId = property; data.depthTexturePropertyId = property + 1000;
      Shader::bindings[property] = &color; Shader::bindings[property + 1000] = &depth;
      runtime.ReleaseSecondaryCameraData(data);
      check(!Shader::bindings[property] && !Shader::bindings[property + 1000], "runtime unbinds controller-owned eye captures immediately");
      controller.OnDestroy();
      check(!color.boundDuringRelease && !depth.boundDuringRelease, "deferred owned captures released without stale globals");
    }
    {
      reset(); RenderTexture legacy, replacement;
      SecondaryCameraData data; data.colorRT = &legacy; data.texturePropertyId = property;
      Shader::bindings[property] = &replacement;
      runtime.ReleaseSecondaryCameraData(data);
      check(Shader::bindings[property] == &replacement && !legacy.alive && replacement.alive,
            "legacy camera target cleanup preserves replacement");
    }
  }
  // A camera target can resize while its previous capture is still globally
  // bound. Test success and allocation failures without drawing or Unity.
  for (bool depth : {false, true}) for (int eye : {0, 1, 2}) {
    for (int owner : {0, 1, 2}) for (int mode : {0, 1, 2, 3}) {
      reset(); RenderTexture old, source, other;
      SecondaryCameraController controller;
      controller.colorKey = 17; controller.depthKey = 23;
      auto& textures = depth ? controller.depthTextures : controller.colorTextures;
      auto& allocations = depth ? controller._depthAllocations : controller._colorAllocations;
      int const key = depth ? 23 : 17;
      textures[eye] = &old;
      allocations[eye] = old.descriptor.value;
      source.descriptor.value.width += 32; // Force a target change.
      if (owner == 2) textures[eye + 10] = &other; // Another live eye capture.
      Shader::bindings[key] = owner == 0 ? &old : &other;
      RenderTexture::allocationMode = mode;
      RenderTexture* result = nullptr;
      bool threw = false;
      try { result = controller.EnsureEyeTexture(textures, eye, &source, depth); }
      catch (std::runtime_error const&) { threw = true; }
      check(!old.boundDuringRelease, "camera resize unbinds its old image before release");
      check(old.releases == 1 && old.destroys == 1, "camera resize releases old image once");
      check(threw == (mode == 3), "allocation exception propagates after safe cleanup");
      check(owner == 0 ? Shader::bindings[key] == nullptr : Shader::bindings[key] == &other,
            mode == 0 ? "camera resize clears only the retired image binding" : "failed camera allocation leaves no released global");
      check(Shader::writes == (owner == 0 ? 1 : 0), "camera resize only clears its owned property");
      check(mode == 0 ? result && result->created && textures.at(eye) == result && allocations.contains(eye)
                      : !result && !textures.contains(eye) && !allocations.contains(eye),
            "failed camera storage cannot be cached as a usable capture");
      check(other.alive && !other.releases && (owner != 2 || textures.at(eye + 10) == &other),
            "camera resize leaves other eye storage intact");
      if (mode == 2) check(!RenderTexture::allocated.back()->alive, "failed native Create destroys partial capture");
      if (mode == 0) {
        auto* saved = result; int const prior = RenderTexture::allocations;
        Shader::bindings[key] = result;
        check(controller.EnsureEyeTexture(textures, eye, &source, depth) == saved &&
              RenderTexture::allocations == prior && Shader::bindings[key] == saved && !saved->releases,
              "unchanged descriptor retains feedback capture and binding");
      }
    }
  }
  // An optional property ID of zero is present, and an already-dead wrapper
  // can still be the saved binding. Clearing identity must not dereference it.
  for (bool depth : {false, true}) {
    reset(); RenderTexture old, source; old.alive = false;
    SecondaryCameraController controller;
    if (depth) controller.depthKey = 0; else controller.colorKey = 0;
    auto& textures = depth ? controller.depthTextures : controller.colorTextures;
    textures[1] = &old; Shader::bindings[0] = &old;
    RenderTexture::allocationMode = 1;
    check(!controller.EnsureEyeTexture(textures, 1, &source, depth), "failed dead-wrapper replacement returns no capture");
    check(!Shader::bindings[0] && Shader::writes == 1, "camera resize clears owned dead wrapper at property zero");
    check(!old.releases && !old.destroys && textures.empty(), "dead capture wrapper is forgotten without native release");
  }
  RenderTexture::allocationMode = 0;
  reset();
  for (auto* texture : RenderTexture::allocated) delete texture;
  std::printf("%d texture ownership and lifecycle checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
