#pragma once
#include <string_view>
#include <cstdint>
#include <vector>
namespace Vivify::SeparateEye {
// Preserve the effective renderer's identity during recursive mirror calls.
// Allocating another per-camera clone here would bypass the game's own
// recursion guards, which compare against that renderer's mirror camera.
class MirrorRenderStack {
 public:
  uintptr_t Resolve(uintptr_t source) const {
    if (!source) return 0;
    for (auto it = _active.rbegin(); it != _active.rend(); ++it)
      if (it->source == source || it->target == source) return it->target;
    return 0;
  }
  void Push(uintptr_t source, uintptr_t target) { _active.push_back({source, target}); }
  void Pop() { if (!_active.empty()) _active.pop_back(); }
 private:
  struct Entry { uintptr_t source, target; };
  std::vector<Entry> _active;
};
// Layer exclusion is safe for material preparation only when realtime shadow
// contribution has been ruled out. Unknown shadow settings keep every renderer.
constexpr bool NeedsCameraMaterials(uint32_t mask, int layer, bool shadowsDisabled) {
  if (!shadowsDisabled || layer < 0 || layer > 31) return true;
  return (mask & (uint32_t(1) << layer)) != 0;
}
struct NativeSceneConditions {
  bool completeStageInfo = false, effectsCompatible = false, sceneMaterialsCompatible = false, nativeMultiview = false;
};
struct NativeMaterialConditions {
  bool completeStageInfo = false, rememberedSource = false, supported = false, sourceShaderMatches = false;
  // Upstream 0.15.11 handles supported native geometry. Only an active
  // explicit-eye tessellation pair adds stages unavailable in that program.
  bool sceneEyeTessellation = true;
};
constexpr bool CanReuseNativeMaterial(NativeMaterialConditions const& m) {
  return m.completeStageInfo && m.rememberedSource && m.supported && m.sourceShaderMatches && !m.sceneEyeTessellation;
}
constexpr bool CanReuseMultiview(NativeSceneConditions const& f) {
  return f.completeStageInfo && f.effectsCompatible && f.sceneMaterialsCompatible && f.nativeMultiview;
}
struct EmptyGeometryConditions {
  bool provedEveryVariant = false, perspectiveCamera = false, shadowsDisabled = false, donorSupported = false, rememberedSource = false;
};
constexpr bool CanUseEmptyGeometry(EmptyGeometryConditions const& f) {
  return f.provedEveryVariant && f.perspectiveCamera && f.shadowsDisabled && f.donorSupported && f.rememberedSource;
}
// A completed frame cannot be reused after a restart, a skipped callback, a
// different camera or a later frame. Suppression requires a completed image.
struct SceneFrame {
  int frame = -1;
  uintptr_t camera = 0;
  bool complete = false;
  void Begin(int nextFrame, uintptr_t nextCamera) { frame = nextFrame; camera = nextCamera; complete = false; }
  void Complete() { complete = camera != 0 && frame >= 0; }
  void Clear() { frame = -1; camera = 0; complete = false; }
  bool Ready(int currentFrame, uintptr_t currentCamera) const {
    return complete && frame == currentFrame && camera == currentCamera;
  }
};
struct FrameConditions {
  bool requested = false, ready = false, playing = false, paused = false, resetting = false;
  bool multiplayer = false, recovery = false, reflections = false, midEffects = false, secondaryCameras = false, sharedEffectTargets = false;
  bool stereo = false, twoLayerArray = false, resolved = false, copySupported = false;
  bool externalCommands = false, croppedViewport = false, clearsBackground = false;
};
constexpr std::string_view Rejection(FrameConditions const& f, bool needsSeparateEye = true) {
  if (!f.requested) return "disabled";
  if (!f.ready) return "eye shader donors are not ready for this level";
  if (!f.playing || f.paused || f.resetting) return "not in active gameplay";
  if (f.multiplayer) return "multiplayer is not supported by the prototype";
  if (f.recovery) return "map is running in crash recovery mode";
  if (f.reflections) return "scene has realtime reflection probes";
  if (f.midEffects || f.secondaryCameras) return "map uses mid-frame effects or secondary cameras";
  if (f.sharedEffectTargets) return "post effects read or write shared camera/screen targets";
  if (f.externalCommands) return "main camera has command buffers that cannot be skipped or replayed safely";
  if (needsSeparateEye && !f.clearsBackground) return "camera depends on previously rendered background";
  if (needsSeparateEye && f.croppedViewport) return "main camera uses a cropped or scaled viewport";
  if (!f.stereo || !f.twoLayerArray) return "expected a stereo two-layer texture array";
  if (needsSeparateEye && (!f.resolved || !f.copySupported)) return "resolved cross-type texture copies are unavailable";
  return {};
}
}
