#include "VivifyRuntimeInternal.hpp"
#include "VivifyRecovery.hpp"
#include <ctime>
#include "VivifyComponents.hpp"
#include "VivifySeparateEye.hpp"
#include "VivifyDepthCopy.hpp"
#include "VivifyCameraBindings.hpp"
#include "VivifyRenderTextureDescriptor.hpp"
#include "VivifyTextureBindings.hpp"
#include "VivifyBlitPolicy.hpp"
#include "UnityEngine/Rendering/CameraEvent.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/RenderTextureFormat.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/VRTextureUsage.hpp"
#include "UnityEngine/RenderTextureMemoryless.hpp"

namespace Vivify {

bool Runtime::CanUseNativeStereoEffects() const {
  for (auto const* effects : {&_preEffects, &_postEffects}) for (auto const& effect : *effects) {
    auto* material = effect.data.material;
    if (!SeparateEye::CanUseNativeMaterial(material)) return false;
    if (material && (material->get_passCount() <= 0 ||
                     (effect.data.pass >= 0 && effect.data.pass >= material->get_passCount()))) return false;
  }
  return true;
}

namespace {
// One slice of a texture array into a plain texture, or the reverse, through
// a draw rather than CommandBuffer.CopyTexture (0.15.11). CopyTexture needs
// both sides to have exactly the same size and format, and between the eye
// texture and a temporary they did not always: the per-eye effects of 0.15.5
// could take the game down.
void BlitSlice(UnityEngine::Rendering::CommandBuffer* cb, UnityEngine::Rendering::RenderTargetIdentifier source,
               int sourceSlice, UnityEngine::Rendering::RenderTargetIdentifier dest, int destSlice,
               UnityEngine::Material* material = nullptr, int pass = -1) {
  cb->Blit_Identifier(byref(source), byref(dest), material, pass, UnityEngine::Vector2(1.0f, 1.0f),
                      UnityEngine::Vector2(0.0f, 0.0f), sourceSlice, destSlice);
}

int ScreenParamsId() {
  static int const id = UnityEngine::Shader::PropertyToID(u"_ScreenParams");
  return id;
}
void RecordScreenParams(UnityEngine::Rendering::CommandBuffer* cb, int width, int height) {
  auto const values = BlitPolicy::ScreenParams(width, height);
  cb->SetGlobalVector(ScreenParamsId(), UnityEngine::Vector4(values[0], values[1], values[2], values[3]));
}
void RecordScreenParams(UnityEngine::Rendering::CommandBuffer* cb, UnityEngine::RenderTexture* target) {
  RecordScreenParams(cb, target->get_width(), target->get_height());
}

struct BlitResources {
  UnityEngine::Rendering::CommandBuffer* cb;
  std::vector<UnityEngine::RenderTexture*> temporaries;
  SafePtr<UnityEngine::RenderTexture> active = UnityEngine::RenderTexture::get_active().unsafePtr();
  UnityEngine::Vector4 screenParams = UnityEngine::Shader::GetGlobalVector(ScreenParamsId());
  bool completed = false;
  explicit BlitResources(UnityEngine::Rendering::CommandBuffer* commands) : cb(commands) {}
  ~BlitResources() noexcept {
    // Discard partially recorded work before releasing any texture it names.
    try { cb->Clear(); } catch (...) { PaperLogger.error("Vivify: clearing failed blit commands failed"); }
    try {
      if (!completed) UnityEngine::Shader::SetGlobalVector(ScreenParamsId(), screenParams);
      UnityEngine::RenderTexture::set_active(active && IsManagedAlive(active.ptr()) ? active.ptr() : nullptr);
    } catch (...) { PaperLogger.error("Vivify: restoring blit state failed"); }
    for (auto* texture : temporaries) {
      try { if (IsManagedAlive(texture)) UnityEngine::RenderTexture::ReleaseTemporary(texture); }
      catch (...) { PaperLogger.error("Vivify: releasing a blit temporary failed"); }
    }
  }
};
UnityEngine::Rendering::RenderTargetIdentifier ToTargetId(UnityEngine::RenderTexture* texture) {
  auto id = UnityEngine::Rendering::RenderTargetIdentifier::op_Implicit___UnityEngine__Rendering__RenderTargetIdentifier(texture);
  if (IsManagedAlive(texture) && texture->get_descriptor().get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray)
    id.m_DepthSlice = -1;
  return id;
}
// Unity's convenience CommandBuffer.Blit overload supplies destDepthSlice=0,
// even for a layered target. Preserve both eye layers explicitly. Ordinary
// 2D texture-camera blits retain their original path.
void RecordBlit(UnityEngine::Rendering::CommandBuffer* cb,
                UnityEngine::Rendering::RenderTargetIdentifier source,
                UnityEngine::Rendering::RenderTargetIdentifier destination,
                UnityEngine::Material* material = nullptr, int pass = -1) {
  if (destination.m_DepthSlice == -1) {
    BlitSlice(cb, source, -1, destination, -1, material, pass);
  } else if (material) cb->Blit(source, destination, material, pass);
  else cb->Blit(source, destination);
}
// An eye-resolution size scaled by the Effect Resolution setting.
int EffectSize(int size) {
  float const scale = GetEffectResolutionScale();
  if (scale >= 1.0f) return size;
  return std::max(1, static_cast<int>(static_cast<float>(size) * scale + 0.5f));
}
UnityEngine::Rendering::RenderTargetIdentifier CameraTargetId() {
  auto id = UnityEngine::Rendering::RenderTargetIdentifier::op_Implicit___UnityEngine__Rendering__RenderTargetIdentifier(
      UnityEngine::Rendering::BuiltinRenderTextureType::CameraTarget);
  auto* camera = UnityEngine::Camera::get_current().unsafePtr();
  if (IsManagedAlive(camera) && camera->get_stereoEnabled()) id.m_DepthSlice = -1;
  return id;
}
}

bool Runtime::TryApplyStereoBlits(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest,
                                 std::string const& cameraName, int phase) {
  auto descriptor = src->get_descriptor();
  if (!GetExperimentalSeparateEye() || SeparateEye::IsRenderingEye() || cameraName != kMainCameraId ||
      HasSharedEyeEffectTargets() || descriptor.get_dimension() != UnityEngine::Rendering::TextureDimension::Tex2DArray ||
      descriptor.get_volumeDepth() != 2 || descriptor.get_msaaSamples() != 1) return false;
  auto* camera = UnityEngine::Camera::get_current().unsafePtr();
  if (!IsAlive(camera) && IsAlive(_lastMainCameraGO)) camera = _lastMainCameraGO->GetComponent<UnityEngine::Camera*>();
  if (!IsAlive(camera) || camera->get_gameObject().unsafePtr() != _lastMainCameraGO) return false;
  // A PC effect can declare sampler2D even in its stereo program. An array
  // bound to it supplies default/unrelated pixels (Dealer becomes solid yellow).
  // Use the explicit-eye blit donors on ordinary slices of the native image.
  struct Temporary {
    SafePtr<UnityEngine::RenderTexture> value;
    explicit Temporary(UnityEngine::RenderTextureDescriptor d)
        : value(UnityEngine::RenderTexture::GetTemporary(d).unsafePtr()) {
      if (!value || !IsManagedAlive(value.ptr())) throw std::runtime_error("stereo effect texture allocation failed");
    }
    ~Temporary() { if (value && IsManagedAlive(value.ptr())) UnityEngine::RenderTexture::ReleaseTemporary(value.ptr()); }
    UnityEngine::RenderTexture* get() const { return value.ptr(); }
  };
  struct EyeScope { ~EyeScope() { SeparateEye::EndBlitEye(); } } eyeScope;
  CameraBindings bindings;
  try {
    if (!SeparateEye::BeginBlitEye(camera, 0)) return false;
    bindings.Capture(camera);
    descriptor.set_vrUsage(UnityEngine::VRTextureUsage::None);
    descriptor.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
    descriptor.set_useDynamicScale(false);
    descriptor.set_useMipMap(false); descriptor.set_autoGenerateMips(false);
    descriptor.set_depthBufferBits(0);
    Temporary stereo(descriptor);
    descriptor.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
    descriptor.set_volumeDepth(1);
    Temporary input(descriptor), output(descriptor);
    constexpr char const* names[] = {"_CameraDepthTexture", "_CameraDepthNormalsTexture", "_CameraOpaqueTexture",
                                    "_CameraMotionVectorsTexture", "_LastCameraDepthTexture"};
    std::array<SafePtr<UnityEngine::RenderTexture>, 5> globals;
    std::array<std::unique_ptr<Temporary>, 5> slices;
    for (size_t i = 0; i != globals.size(); ++i) {
      if ((i == 0 || i == 4) && !MapShadersNeedSceneDepth().value_or(true)) continue;
      auto* texture = UnityEngine::Shader::GetGlobalTexture(StringW(std::string(names[i]))).unsafePtr();
      if (!IsAlive(texture)) continue;
      auto* rt = il2cpp_utils::try_cast<UnityEngine::RenderTexture>(texture).value_or(nullptr);
      if (!IsAlive(rt)) continue;
      auto d = rt->get_descriptor();
      if (d.get_dimension() != UnityEngine::Rendering::TextureDimension::Tex2DArray) continue;
      if (d.get_volumeDepth() != 2 || d.get_msaaSamples() != 1) throw std::runtime_error("unsupported stereo effect camera texture layout");
      globals[i] = rt;
      d.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D); d.set_volumeDepth(1);
      d.set_vrUsage(UnityEngine::VRTextureUsage::None); d.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
      d.set_useDynamicScale(false);
      slices[i] = std::make_unique<Temporary>(d);
    }
    for (int eye = 0; eye != 2; ++eye) {
      if (eye != 0 && !SeparateEye::BeginBlitEye(camera, eye)) throw std::runtime_error("right-eye screen effect scope unavailable");
      for (size_t i = 0; i != globals.size(); ++i) if (slices[i]) {
        UnityEngine::Graphics::CopyTexture(globals[i].ptr(), eye, 0, slices[i]->get(), 0, 0);
        UnityEngine::Shader::SetGlobalTexture(StringW(std::string(names[i])), slices[i]->get());
      }
      UnityEngine::Graphics::CopyTexture(src, eye, 0, input.get(), 0, 0);
      ApplyBlits(input.get(), output.get(), cameraName, phase);
      UnityEngine::Graphics::CopyTexture(output.get(), 0, 0, stereo.get(), eye, 0);
      SeparateEye::EndBlitEye();
    }
    if (!bindings.Restore()) { SeparateEye::RejectCameraBindings(); return false; }
    ApplyBlits(stereo.get(), dest, cameraName, -1);
    static bool reported = false;
    if (!reported) { PaperLogger.info("Vivify stereo screen effects: explicit-eye image processing; reusing the native multiview scene"); reported = true; }
    return true;
  } catch (std::exception const& ex) {
    static bool reported = false;
    if (!reported) { PaperLogger.error("Vivify stereo screen effect processing failed: {}", ex.what()); reported = true; }
  } catch (Il2CppExceptionWrapper const& ex) {
    static bool reported = false;
    if (!reported) { PaperLogger.error("Vivify stereo screen effect processing failed: {}", il2cpp_utils::ExceptionToString(ex.ex)); reported = true; }
  } catch (...) {
    static bool reported = false;
    if (!reported) { PaperLogger.error("Vivify stereo screen effect processing failed with a native exception"); reported = true; }
  }
  return false;
}

void Runtime::ApplyBlits(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest,
                         std::string const& cameraName, int phase) {
  if (!IsAlive(src) || (dest != nullptr && !IsAlive(dest))) return;
  bool const passthroughOnly = phase < 0 || GetDisableAllBlits() || _blitGuardAllOff || _isResetting || _pauseMenuActive ||
                               _currentBeatmapData == nullptr ||
                               (phase == 1 ? _preEffects.empty()
                                           : (phase == 2 ? _postEffects.empty()
                                                         : (_preEffects.empty() && _postEffects.empty())));
  auto destId = (dest != nullptr) ? ToTargetId(dest) : CameraTargetId();
  if (passthroughOnly) {
    auto* cb = ImmediateCommandBuffer();
    RecordBlit(cb, ToTargetId(src), destId);
    UnityEngine::Graphics::ExecuteCommandBuffer(cb);
    cb->Clear();
    return;
  }
  if (TryApplyStereoBlits(src, dest, cameraName, phase)) return;
  auto* main = EnsureCachedBlitTexture(_mainBlitTexture, _mainBlitAllocation, src);
  auto* scratch = EnsureCachedBlitTexture(_scratchBlitTexture, _scratchBlitAllocation, src);
  if (!IsAlive(main) || !IsAlive(scratch)) {
    auto* cb = ImmediateCommandBuffer();
    RecordBlit(cb, ToTargetId(src), destId);
    UnityEngine::Graphics::ExecuteCommandBuffer(cb);
    cb->Clear();
    return;
  }

  auto* cb = ImmediateCommandBuffer();
  BlitResources resources(cb);
  auto& selfBlitTemps = resources.temporaries;

  RecordBlit(cb, ToTargetId(src), ToTargetId(main));

  // _ScreenParams for the effects (0.15.8): the size of the picture they work
  // on. It is a built-in that only a camera's own setup writes, so inside
  // this command buffer it is whatever the last camera to render left there
  // -- a map camera or a probe drawing into a small texture leaves a small
  // size. Breezer's VHS effect snaps its sampling to a grid of _ScreenParams
  // pixels; with a wrong size the whole view samples a handful of points, or
  // one, and the screen goes a single colour.
  _blitGuardScreenParams = resources.screenParams;
  auto setScreenParams = [&](UnityEngine::RenderTexture* target) {
    RecordScreenParams(cb, target);
  };
  setScreenParams(main);

  // Flat-screen guard (0.15.7): now and then, a 16x16 sample of the picture
  // before and after this call's effects (BlitGuard* below).
  std::vector<UnityEngine::Material*> guardMaterials;
  if (GetFlatScreenGuard() && !GetExperimentalSeparateEye()) {
    auto collect = [&](std::vector<ActiveBlitEffect> const& effects) {
      for (auto const& effect : effects) {
        if (effect.data.material != nullptr && effect.data.source == kMainCameraId &&
            CanUseBlitMaterial(effect.data.material, effect.data.pass)) {
          guardMaterials.push_back(effect.data.material);
        }
      }
    };
    if (phase == 0 || phase == 1) collect(_preEffects);
    if (phase == 0 || phase == 2) collect(_postEffects);
  }
  bool const guardProbe = !guardMaterials.empty() && BlitGuardDue(guardMaterials) && EnsureBlitGuardTextures();
  UnityEngine::RenderTexture* guardEye = nullptr;
  auto emitProbe = [&](UnityEngine::RenderTexture* from, UnityEngine::RenderTexture* probe) {
    auto desc = from->get_descriptor();
    if (desc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray) {
      if (guardEye == nullptr) {
        desc.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
        desc.set_volumeDepth(1);
        desc.set_vrUsage(UnityEngine::VRTextureUsage::None);
        desc.set_msaaSamples(1);
        desc.set_depthBufferBits(0);
        guardEye = UnityEngine::RenderTexture::GetTemporary(desc).unsafePtr();
        if (!IsManagedAlive(guardEye)) {
          guardEye = nullptr;
          return;
        }
        selfBlitTemps.emplace_back(guardEye);
      }
      auto fromId = ToTargetId(from);
      auto eyeId = ToTargetId(guardEye);
      BlitSlice(cb, fromId, 0, eyeId, -1);
      cb->Blit(eyeId, ToTargetId(probe));
    } else {
      cb->Blit(ToTargetId(from), ToTargetId(probe));
    }
  };
  if (guardProbe) emitProbe(main, _blitGuardIn);

  auto* mainCurrent = main;
  auto* mainScratch = scratch;

  auto emitBlit = [&](UnityEngine::RenderTexture* sourceTexture, UnityEngine::RenderTexture* dst,
                      UnityEngine::Material* material, int pass) {
    auto const srcId = ToTargetId(sourceTexture);
    auto const dstId = ToTargetId(dst);
    setScreenParams(dst);
    material = BlitMaterialFor(material, dst);
    // A Quest-built effect shader with no two-eye program (0.15.5): drawn
    // into the two-eye texture it draws nothing, and the frame after it is
    // whatever the scratch texture last held. Run it once per eye through
    // single textures instead.
    if (material != nullptr && NeedsPerEyeBlit(material, dst)) {
      auto sourceDesc = sourceTexture->get_descriptor();
      bool const sourceArray = sourceDesc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray;
      bool const sourceFlat = sourceDesc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2D;
      auto const rightElement = BlitPolicy::SourceElement(sourceFlat, sourceArray, sourceDesc.get_volumeDepth(),
                                                         sourceDesc.get_msaaSamples(), 1);
      if (!rightElement) {
        RecordBlit(cb, srcId, dstId, material, pass);
        return;
      }
      auto desc = dst->get_descriptor();
      desc.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
      desc.set_volumeDepth(1);
      desc.set_vrUsage(UnityEngine::VRTextureUsage::None);
      desc.set_msaaSamples(1);
      desc.set_depthBufferBits(0);
      desc.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
      // A named screen can differ in size and format from the main view.
      // Copy source layers into matching storage; the blit then scales them.
      auto inputDesc = sourceDesc;
      inputDesc.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
      inputDesc.set_volumeDepth(1); inputDesc.set_vrUsage(UnityEngine::VRTextureUsage::None);
      inputDesc.set_depthBufferBits(0); inputDesc.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
      auto* eyeIn = sourceArray ? UnityEngine::RenderTexture::GetTemporary(inputDesc).unsafePtr() : nullptr;
      if (IsManagedAlive(eyeIn)) selfBlitTemps.emplace_back(eyeIn);
      auto* eyeOut = UnityEngine::RenderTexture::GetTemporary(desc).unsafePtr();
      if (IsManagedAlive(eyeOut)) selfBlitTemps.emplace_back(eyeOut);
      if ((!sourceArray || IsManagedAlive(eyeIn)) && IsManagedAlive(eyeOut)) {
        auto eyeInId = sourceArray ? ToTargetId(eyeIn) : srcId;
        auto eyeOutId = ToTargetId(eyeOut);
        auto source = srcId;
        auto target = dstId;
        // The one-eye program has to be the one chosen for these blits. Where
        // the shader also has a two-eye variant (Fall Away's overlay), Unity
        // would pick that while the stereo keyword is on, so what is queued
        // so far is run, the keyword goes off for the two eye blits, and
        // comes back on after (0.15.8).
        UnityEngine::Graphics::ExecuteCommandBuffer(cb);
        cb->Clear();
        bool const stereoWasOn = UnityEngine::Shader::IsKeywordEnabled(StringW("STEREO_MULTIVIEW_ON"));
        BlitPolicy::DrawSingleView(stereoWasOn, [](bool enabled) {
          if (enabled) UnityEngine::Shader::EnableKeyword(StringW("STEREO_MULTIVIEW_ON"));
          else UnityEngine::Shader::DisableKeyword(StringW("STEREO_MULTIVIEW_ON"));
        }, [&] {
          // A converted map's multiview material cannot draw into an eye's
          // ordinary texture. Resolve its single-view companion inside the
          // keyword scope while preserving any active explicit-eye donor.
          auto* eyeMaterial = BlitMaterialFor(material, eyeOut);
          for (int eye = 0; eye < 2; eye++) {
            if (sourceArray) BlitSlice(cb, source, eye, eyeInId, -1);
            if (pass >= 0) {
              cb->Blit(eyeInId, eyeOutId, eyeMaterial, pass);
            } else {
              cb->Blit(eyeInId, eyeOutId, eyeMaterial);
            }
            BlitSlice(cb, eyeOutId, -1, target, eye);
          }
          UnityEngine::Graphics::ExecuteCommandBuffer(cb);
          cb->Clear();
        });
        return;
      }
    }
    if (material == nullptr) {
      RecordBlit(cb, srcId, dstId);
    } else if (pass >= 0) {
      RecordBlit(cb, srcId, dstId, material, pass);
    } else {
      RecordBlit(cb, srcId, dstId, material);
    }
  };
  auto renderEffects = [&](std::vector<ActiveBlitEffect> const& effects) {

    for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
      auto const& data = it->data;
      if (data.material != nullptr && !CanUseBlitMaterial(data.material, data.pass)) continue;
      bool const sourceIsMain = data.source == kMainCameraId;
      UnityEngine::RenderTexture* blitSrc = nullptr;
      if (sourceIsMain) {
        blitSrc = mainCurrent;
      } else if (auto found = _declaredTextures.find(data.source); found != _declaredTextures.end()) {
        blitSrc = found->second.texture;
      } else if (auto found = _secondaryCameras.find(data.source); found != _secondaryCameras.end()) {
        blitSrc = SecondaryCameraColorRT(found->second);
      }
      if (!IsAlive(blitSrc)) continue;
      for (auto const& targetName : data.targets) {
        if (targetName == kMainCameraId) {
          if (sourceIsMain) {
            emitBlit(blitSrc, mainScratch, data.material, data.pass);
            std::swap(mainCurrent, mainScratch);

            blitSrc = mainCurrent;
          } else {

            emitBlit(blitSrc, mainCurrent, data.material, data.pass);
          }
        } else if (auto found = _declaredTextures.find(targetName); found != _declaredTextures.end()) {
          auto* target = found->second.texture;
          if (!IsAlive(target)) continue;
          if (target == blitSrc) {

            auto desc = target->get_descriptor();
            auto temp = UnityEngine::RenderTexture::GetTemporary(desc);
            auto* tempPtr = temp.unsafePtr();
            if (!IsManagedAlive(tempPtr)) continue;
            selfBlitTemps.emplace_back(tempPtr);
            emitBlit(blitSrc, tempPtr, data.material, data.pass);
            RecordBlit(cb, ToTargetId(tempPtr), ToTargetId(target));
          } else {
            emitBlit(blitSrc, target, data.material, data.pass);
          }
        }
      }
    }
  };
  if (phase == 0 || phase == 1) {
    renderEffects(_preEffects);
  }
  if (phase == 0 || phase == 2) {
    renderEffects(_postEffects);
  }

  if (guardProbe) emitProbe(mainCurrent, _blitGuardOut);
  setScreenParams(src);
  RecordBlit(cb, ToTargetId(mainCurrent), destId);
  UnityEngine::Graphics::ExecuteCommandBuffer(cb);
  cb->Clear();
  if (guardProbe) BlitGuardEvaluate(guardMaterials, src);
  resources.completed = true;
}

// Flat-screen guard (0.15.7). Reported: a map's screen effect turns the whole
// view one colour, and the map is only playable with every effect off (Why's
// Upstream 0.15.11's normal-renderer guard samples 16 blocks every 2 s.
// The explicit-eye backend keeps guard sampling inactive for authored fades.
namespace {
constexpr int kGuardSize = 16;

}  // namespace

bool Runtime::EnsureBlitGuardTextures() {
  auto make = [this](UnityEngine::RenderTexture*& texture) {
    if (IsAlive(texture) && texture->IsCreated()) return true;
    ReleaseRenderTexture(texture);
    texture = UnityEngine::RenderTexture::New_ctor(kGuardSize, kGuardSize, 0,
                                                   UnityEngine::RenderTextureFormat::ARGB32);
    if (!IsAlive(texture)) {
      texture = nullptr;
      return false;
    }
    if (!texture->Create()) {
      ReleaseRenderTexture(texture);
      return false;
    }
    return true;
  };
  if (!make(_blitGuardIn) || !make(_blitGuardOut)) return false;
  if (!IsAlive(_blitGuardReader)) {
    _blitGuardReader = UnityEngine::Texture2D::New_ctor(kGuardSize, kGuardSize, UnityEngine::TextureFormat::RGBA32,
                                                        false);
  }
  return IsAlive(_blitGuardReader);
}

// Whether this call should sample: every 2 s while an effect is on (0.15.11).
bool Runtime::BlitGuardDue(std::vector<UnityEngine::Material*> const& materials) {
  if (materials.empty() || _blitGuardAllOff) return false;
  float const now = UnityEngine::Time::get_unscaledTime();
  if (now >= _blitGuardLastSample && now - _blitGuardLastSample < 2.0f) return false;
  _blitGuardLastSample = now;
  return true;
}

namespace {
// The colours of the 16 blocks (4 x 4) of a 16 x 16 sample, as how many
// clearly different colours there are among them (up to 3).
int DistinctBlockColours(UnityEngine::RenderTexture* probe, UnityEngine::Texture2D* reader, int (&first)[3]) {
  if (!IsManagedAlive(probe) || !IsManagedAlive(reader)) return -1;
  auto previous = UnityEngine::RenderTexture::get_active();
  UnityEngine::RenderTexture::set_active(probe);
  reader->ReadPixels(UnityEngine::Rect(0.0f, 0.0f, static_cast<float>(kGuardSize), static_cast<float>(kGuardSize)),
                     0, 0, false);
  UnityEngine::RenderTexture::set_active(previous);
  auto pixels = reader->GetPixels32();
  if (!pixels || pixels.size() < kGuardSize * kGuardSize) return -1;
  std::vector<std::array<int, 3>> colours;
  for (int by = 0; by < 4; by++) {
    for (int bx = 0; bx < 4; bx++) {
      int sum[3] = {0, 0, 0};
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          auto const& p = pixels[(by * 4 + y) * kGuardSize + bx * 4 + x];
          sum[0] += p.r;
          sum[1] += p.g;
          sum[2] += p.b;
        }
      }
      std::array<int, 3> const block = {sum[0] / 16, sum[1] / 16, sum[2] / 16};
      bool known = false;
      for (auto const& colour : colours) {
        if (std::abs(colour[0] - block[0]) <= 12 && std::abs(colour[1] - block[1]) <= 12 &&
            std::abs(colour[2] - block[2]) <= 12) {
          known = true;
          break;
        }
      }
      if (!known) colours.push_back(block);
    }
  }
  for (int c = 0; c < 3; c++) first[c] = colours.empty() ? 0 : colours.front()[c];
  return static_cast<int>(std::min<size_t>(colours.size(), 3));
}
}  // namespace

// Flat-screen guard (0.15.11, as asked): every 2 s while an effect is on, the
// picture coming out of the effects is measured in 16 blocks. If they are
// all one colour, or only two, every screen effect is switched off for the
// rest of the level -- unless the picture going in was just as plain, in
// which case the scene itself is dark or flat and the effects are not to
// blame.
void Runtime::BlitGuardEvaluate(std::vector<UnityEngine::Material*> const& materials, UnityEngine::RenderTexture* src) {
  int inColour[3];
  int outColour[3];
  int const in = DistinctBlockColours(_blitGuardIn, _blitGuardReader, inColour);
  int const out = DistinctBlockColours(_blitGuardOut, _blitGuardReader, outColour);
  if (in < 0 || out < 0) return;
  std::string names;
  for (auto* material : materials) {
    auto* shader = IsAlive(material) ? material->get_shader().unsafePtr() : nullptr;
    names += (names.empty() ? "'" : ", '") + ShaderNameForLog(shader) + "'";
  }
  bool const trip = out <= 2 && in > 2;
  if (trip || !_blitGuardLogged) {
    _blitGuardLogged = true;
    auto desc = src->get_descriptor();
    PaperLogger.info("Vivify Blit check: effect(s) {} -- {} colour(s) among 16 blocks going in, {} coming out "
                     "(first rgb=({},{},{})); screen {}x{} slices={}; _ScreenParams before the effects=({:.0f},{:.0f})",
                     names, in > 2 ? std::string("3+") : std::to_string(in),
                     out > 2 ? std::string("3+") : std::to_string(out), outColour[0], outColour[1], outColour[2],
                     desc.get_width(), desc.get_height(), desc.get_volumeDepth(), _blitGuardScreenParams.x,
                     _blitGuardScreenParams.y);
  }
  if (!trip) return;
  _blitGuardAllOff = true;
  _preEffects.clear();
  _postEffects.clear();
  for (auto& bucket : _midEffects) bucket.clear();
  PaperLogger.warn("Vivify Blit: the screen came out as {} colour(s) across all 16 blocks; every screen effect is "
                   "off for the rest of this level (Flat-Screen Effect Guard in settings)", out);
  FlushVivifyLogs();
}

void Runtime::CacheMainRenderDescriptor(UnityEngine::RenderTexture* src) {
  if (!IsAlive(src)) return;
  // Eye callbacks process ordinary 2D images, but that must not resize shared
  // main-camera feedback arrays or replace the real XR descriptor.
  if (SeparateEye::IsRenderingEye()) return;
  auto* camera = UnityEngine::Camera::get_current().unsafePtr();
  if (IsAlive(camera) && IsAlive(_lastMainCameraGO) && camera->get_gameObject().unsafePtr() != _lastMainCameraGO) return;
  auto desc = src->get_descriptor();

  desc.set_msaaSamples(1);
  // Keep the real camera layout and precision for CreateScreenTexture.
  // Individual effect scratch targets apply their own scale/depth policy.
  _cachedMainDescriptor = desc;
  _hasMainDescriptor = true;
  PrepareDeclaredTextures();
}

bool Runtime::EnsureMidRenderTextures() {
  if (!_hasMainDescriptor) return false;
  auto desc = _cachedMainDescriptor;
  desc.set_depthBufferBits(0);
  desc.set_width(EffectSize(desc.get_width()));
  desc.set_height(EffectSize(desc.get_height()));
  auto const requested = RenderTexturePolicy::Describe(desc);
  bool const alive = IsAlive(_midMainRT) && IsAlive(_midScratchRT);
  bool const mismatch = RenderTexturePolicy::NeedsAllocation(_midAllocation, requested, alive,
                                                             alive && _midMainRT->IsCreated() && _midScratchRT->IsCreated());
  if (!mismatch) return true;
  ReleaseMidRenderTextures();
  _midMainRT = UnityEngine::RenderTexture::New_ctor(desc);
  _midScratchRT = UnityEngine::RenderTexture::New_ctor(desc);
  if (!IsAlive(_midMainRT) || !IsAlive(_midScratchRT) || !_midMainRT->Create() || !_midScratchRT->Create()) {
    ReleaseMidRenderTextures();
    return false;
  }
  ClearRenderTexture(_midMainRT);
  ClearRenderTexture(_midScratchRT);
  _midAllocation = requested;
  return true;
}

void Runtime::ReleaseMidRenderTextures() {
  if (IsAlive(_midMainRT)) {
    _midMainRT->Release();
    UnityEngine::Object::Destroy(_midMainRT);
  }
  if (IsAlive(_midScratchRT)) {
    _midScratchRT->Release();
    UnityEngine::Object::Destroy(_midScratchRT);
  }
  _midMainRT = nullptr;
  _midScratchRT = nullptr;
  _midAllocation.reset();
}

UnityEngine::Rendering::CommandBuffer* Runtime::ImmediateCommandBuffer() {
  if (!_immediateCommandBuffer) {
    _immediateCommandBuffer.emplace(UnityEngine::Rendering::CommandBuffer::New_ctor());
  }
  auto* cb = _immediateCommandBuffer.ptr();
  cb->Clear();
  return cb;
}

UnityEngine::Rendering::CommandBuffer* Runtime::BuildMidRenderCommandBuffer(
    std::vector<ActiveBlitEffect> const& effects, int bucket) {
  using RTI = UnityEngine::Rendering::RenderTargetIdentifier;
  auto& pooled = _midCommandBufferPool[bucket];
  if (!pooled) pooled.emplace(UnityEngine::Rendering::CommandBuffer::New_ctor());
  auto* cb = pooled.ptr();
  cb->Clear();
  RTI srcId{};
  srcId._ctor(UnityEngine::Rendering::BuiltinRenderTextureType::CurrentActive);
  RTI dstId{};
  dstId = CameraTargetId();
  RecordBlit(cb, srcId, ToTargetId(_midMainRT));

  auto* mainCurrent = _midMainRT;
  auto* mainScratch = _midScratchRT;

  auto emitBlit = [&](RTI blitSrc, UnityEngine::RenderTexture* dst, UnityEngine::Material* material, int pass) {
    RTI const blitDst = ToTargetId(dst);
    RecordScreenParams(cb, dst);
    material = BlitMaterialFor(material, dst);
    if (material == nullptr) {
      RecordBlit(cb, blitSrc, blitDst);
    } else if (pass >= 0) {
      RecordBlit(cb, blitSrc, blitDst, material, pass);
    } else {
      RecordBlit(cb, blitSrc, blitDst, material);
    }
  };

  for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
    auto const& data = it->data;
    if (data.material != nullptr && !CanUseBlitMaterial(data.material, data.pass)) continue;
    bool const sourceIsMain = data.source == kMainCameraId;
    UnityEngine::RenderTexture* blitSrcTexture = nullptr;
    if (sourceIsMain) {
      blitSrcTexture = mainCurrent;
    } else if (auto found = _declaredTextures.find(data.source); found != _declaredTextures.end()) {
      blitSrcTexture = found->second.texture;
    } else if (auto found = _secondaryCameras.find(data.source); found != _secondaryCameras.end()) {
      blitSrcTexture = SecondaryCameraColorRT(found->second);
    }
    if (!IsAlive(blitSrcTexture)) continue;
    auto blitSrcId = ToTargetId(blitSrcTexture);
    for (auto const& targetName : data.targets) {
      if (targetName == kMainCameraId) {
        if (sourceIsMain) {
          emitBlit(blitSrcId, mainScratch, data.material, data.pass);
          std::swap(mainCurrent, mainScratch);
          blitSrcTexture = mainCurrent;
          blitSrcId = ToTargetId(mainCurrent);
        } else {

          emitBlit(blitSrcId, mainCurrent, data.material, data.pass);
        }
      } else if (auto found = _declaredTextures.find(targetName); found != _declaredTextures.end()) {
        auto* target = found->second.texture;
        if (!IsAlive(target)) continue;
        if (target == blitSrcTexture) {

          auto temp = UnityEngine::RenderTexture::GetTemporary(target->get_descriptor());
          auto* tempPtr = temp.unsafePtr();
          if (!IsManagedAlive(tempPtr)) continue;
          _midSelfBlitTemps.emplace_back(tempPtr);
          emitBlit(blitSrcId, tempPtr, data.material, data.pass);
          RecordBlit(cb, ToTargetId(tempPtr), ToTargetId(target));
        } else {
          emitBlit(blitSrcId, target, data.material, data.pass);
        }
      }
    }
  }

  RecordScreenParams(cb, _cachedMainDescriptor.get_width(), _cachedMainDescriptor.get_height());
  RecordBlit(cb, ToTargetId(mainCurrent), dstId);

  // CommandBuffer.Blit binds its destination as a colour-only render target, so
  // once this buffer has run the camera is left rendering into the camera
  // target with no depth attachment for the rest of the frame. Everything the
  // camera still has to draw after this CameraEvent -- which for
  // BeforeForwardAlpha/AfterForwardOpaque and earlier is all the transparent
  // geometry: arcs, saber trails, note debris -- then has no depth buffer to
  // test or write against and drops out. That is why arcs disappear only on
  // maps that use a mid-render Blit.
  //
  // SetRenderTarget(rt) with a single identifier re-binds rt as BOTH the colour
  // and the depth surface with LoadAction.Load, so the camera's own depth
  // contents survive and the rest of the frame renders normally.
  cb->SetRenderTarget(CameraTargetId());
  return cb;
}

void Runtime::AddMidRenderCommandBuffers(UnityEngine::Camera* camera) {

  RemoveMidRenderCommandBuffers();
  if (!IsAlive(camera) || GetDisableAllBlits() || _isResetting || _pauseMenuActive ||
      _currentBeatmapData == nullptr || !HasMidRenderEffects()) {
    return;
  }

  if (!EnsureMidRenderTextures()) return;

  static constexpr std::array<int32_t, kMidRenderOrderCount> kOrderEvents = {
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeSkybox),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterSkybox),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeForwardOpaque),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterForwardOpaque),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeForwardAlpha),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterForwardAlpha),
  };
  for (int bucket = 0; bucket < kMidRenderOrderCount; bucket++) {
    auto const& effects = _midEffects[bucket];
    if (effects.empty()) continue;
    auto* cb = BuildMidRenderCommandBuffer(effects, bucket);
    if (cb == nullptr) continue;
    camera->AddCommandBuffer(UnityEngine::Rendering::CameraEvent(kOrderEvents[bucket]), cb);

    _activeMidCommandBuffers.emplace_back(kOrderEvents[bucket],
                                          SafePtr<UnityEngine::Rendering::CommandBuffer>(cb));
  }
  _midCommandBufferCamera = camera;
}

void Runtime::RemoveMidRenderCommandBuffers() {
  for (auto& [cameraEvent, cb] : _activeMidCommandBuffers) {
    if (!cb) continue;
    if (IsAlive(_midCommandBufferCamera)) {
      _midCommandBufferCamera->RemoveCommandBuffer(UnityEngine::Rendering::CameraEvent(cameraEvent), cb.ptr());
    }
    // Pooled (BuildMidRenderCommandBuffer): emptied for the next frame, not disposed.
    cb->Clear();
  }
  _activeMidCommandBuffers.clear();
  _midCommandBufferCamera = nullptr;

  for (auto* temp : _midSelfBlitTemps) {
    if (IsAlive(temp)) UnityEngine::RenderTexture::ReleaseTemporary(temp);
  }
  _midSelfBlitTemps.clear();
}

UnityEngine::RenderTexture* Runtime::EnsureCachedBlitTexture(UnityEngine::RenderTexture*& texture,
                                                             std::optional<RenderTexturePolicy::Descriptor>& allocation,
                                                             UnityEngine::RenderTexture* src) {
  auto desc = src->get_descriptor();
  desc.set_msaaSamples(1);
  desc.set_depthBufferBits(0);
  desc.set_width(EffectSize(desc.get_width()));
  desc.set_height(EffectSize(desc.get_height()));
  auto const requested = RenderTexturePolicy::Describe(desc);
  bool const alive = IsAlive(texture);
  if (!RenderTexturePolicy::NeedsAllocation(allocation, requested, alive, alive && texture->IsCreated())) return texture;
  ReleaseRenderTexture(texture);
  allocation.reset();
  texture = UnityEngine::RenderTexture::New_ctor(desc);
  if (!IsAlive(texture)) { texture = nullptr; return nullptr; }
  if (!texture->Create()) { ReleaseRenderTexture(texture); return nullptr; }
  allocation = requested;
  ClearRenderTexture(texture);
  return texture;
}

void Runtime::ReleaseRenderTexture(UnityEngine::RenderTexture*& texture) {
  if (IsAlive(texture)) {
    texture->Release();
    UnityEngine::Object::Destroy(texture);
  }
  texture = nullptr;
}

void Runtime::ReleaseCachedBlitTextures() {
  ReleaseRenderTexture(_mainBlitTexture);
  ReleaseRenderTexture(_scratchBlitTexture);
  ReleaseRenderTexture(_blitGuardIn);
  ReleaseRenderTexture(_blitGuardOut);
  _mainBlitAllocation.reset();
  _scratchBlitAllocation.reset();
}

bool Runtime::CanUseBlitMaterial(UnityEngine::Material* material, int pass) const {
  material = SeparateEye::BlitMaterial(material);
  if (!IsAlive(material)) {
    return false;
  }
  if (!GetExperimentalSeparateEye() && _blitGuardKilled.contains(material)) return false;
  // Validate the requested pass on every call. One invalid pass must not
  // poison other passes, and a cached valid pass must not authorize another.
  int const passCount = material->get_passCount();
  if (passCount <= 0 || pass < -1 || pass >= passCount) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify blit material invalid pass: material='{}' pass={} passCount={}",
                       ToStdString(material->get_name()), pass, passCount);
    }
    return false;
  }
  auto* shader = material->get_shader().unsafePtr();
  bool const supported = IsAlive(shader) && shader->get_isSupported();
  bool const fallback = _fallbackShadedMaterials.contains(material);
  if (auto it = _blitMaterialValidCache.find(material); it != _blitMaterialValidCache.end()) {
    auto const& cached = it->second;
    if (cached.shader.ptr() == shader && cached.supported == supported && cached.fallback == fallback) return cached.valid;
  }
  auto shaderName = ShaderNameForLog(shader);
  bool const valid = supported && !fallback && !IsInternalErrorShaderName(shaderName);
  auto& cached = _blitMaterialValidCache[material];
  cached.material = material; cached.shader = shader;
  cached.supported = supported; cached.fallback = fallback; cached.valid = valid;
  if (!valid && (fallback || GetVivifyDebugLogging())) {
    PaperLogger.warn("Vivify blit material unavailable: material='{}' shader='{}' supported={} standIn={}",
                     ToStdString(material->get_name()), shaderName, supported, fallback);
  }
  return valid;
}

bool Runtime::SameBlitData(BlitMaterialData const& left, BlitMaterialData const& right) {
  return left.material == right.material &&
         left.priority == right.priority &&
         left.source == right.source &&
         left.targets == right.targets &&
         left.pass == right.pass;
}

PostProcessingOrder Runtime::ParsePostProcessingOrder(rapidjson::Value const& json) {
  // Friendlier spellings, from webbs7524-wq's Vivify-Quest-2: a boolean
  // "beforeMainEffect"/"afterMainEffect", and "phase"/"timing" as synonyms for
  // "order" with "before"/"pre" as short forms. PC Vivify only reads "order",
  // so a map written against it is unaffected; these only widen what a
  // hand-written Quest map is allowed to say.
  if (auto beforeMain = ReadBool(json, "beforeMainEffect"); beforeMain.has_value()) {
    return *beforeMain ? PostProcessingOrder::BeforeMainEffect : PostProcessingOrder::AfterMainEffect;
  }
  if (auto afterMain = ReadBool(json, "afterMainEffect"); afterMain.has_value()) {
    return *afterMain ? PostProcessingOrder::AfterMainEffect : PostProcessingOrder::BeforeMainEffect;
  }
  auto order = ReadStringView(json, "order");
  if (!order.has_value()) order = ReadStringView(json, "phase");
  if (!order.has_value()) order = ReadStringView(json, "timing");
  if (!order.has_value()) return PostProcessingOrder::AfterMainEffect;
  std::string normalized = NormalizeAssetKey(*order);
  if (normalized == "beforemaineffect" || normalized == "beforemain" || normalized == "before" ||
      normalized == "pre") {
    return PostProcessingOrder::BeforeMainEffect;
  }
  if (normalized == "beforeskybox") return PostProcessingOrder::BeforeSkybox;
  if (normalized == "afterskybox") return PostProcessingOrder::AfterSkybox;
  if (normalized == "beforeopaque") return PostProcessingOrder::BeforeOpaque;
  if (normalized == "afteropaque") return PostProcessingOrder::AfterOpaque;
  if (normalized == "beforealpha") return PostProcessingOrder::BeforeAlpha;
  if (normalized == "afteralpha") return PostProcessingOrder::AfterAlpha;
  return PostProcessingOrder::AfterMainEffect;
}

void Runtime::AddOrUpdateBlitEffect(std::vector<ActiveBlitEffect>& effects, ActiveBlitEffect effect) {
  auto existing = std::find_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& active) {
    return SameBlitData(active.data, effect.data);
  });
  if (existing != effects.end()) {
    if (effect.data.frame.has_value()) {
      existing->data.frame = effect.data.frame;
      existing->expireTime = 0.0f;
    } else {
      existing->data.frame.reset();
      existing->expireTime = effect.expireTime <= 0.0f ? 0.0f : std::max(existing->expireTime, effect.expireTime);
    }
  } else {
    effects.emplace_back(std::move(effect));
  }
  std::stable_sort(effects.begin(), effects.end(), [](ActiveBlitEffect const& left, ActiveBlitEffect const& right) {
    return left.data.priority < right.data.priority;
  });
  while (effects.size() > kMaxActiveBlitEffects) {
    effects.erase(effects.begin());
  }
}

// "asset" is what PC Vivify reads. The others are accepted for hand-written
// Quest maps (from webbs7524-wq's Vivify-Quest-2), in this order of precedence.
std::optional<std::string_view> Runtime::ReadBlitAssetName(rapidjson::Value const& json) {
  for (auto key : {"asset"sv, "material"sv, "effect"sv, "postProcessMaterial"sv, "postProcessingMaterial"sv}) {
    if (auto value = ReadStringView(json, key)) return value;
  }
  return std::nullopt;
}

// Removes the active effects a clearing Blit event describes. Each field the
// event actually names narrows the match; an event naming nothing but
// "clear": true empties that order's whole list.
void Runtime::RemoveMatchingBlitEffects(std::vector<ActiveBlitEffect>& effects, rapidjson::Value const& json,
                                        BlitMaterialData const& filter, bool filterAsset) {
  bool const filterSource = ReadValuePtr(json, "source") != nullptr;
  bool const filterTargets = ReadValuePtr(json, "destination") != nullptr;
  bool const filterPriority = ReadValuePtr(json, "priority") != nullptr;
  bool const filterPass = ReadValuePtr(json, "pass") != nullptr;
  effects.erase(std::remove_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& active) {
    if (filterAsset && active.data.asset != filter.asset) return false;
    if (filterSource && active.data.source != filter.source) return false;
    if (filterTargets && active.data.targets != filter.targets) return false;
    if (filterPriority && active.data.priority != filter.priority) return false;
    if (filterPass && active.data.pass != filter.pass) return false;
    return true;
  }), effects.end());
}

void Runtime::HandleBlit(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json) {
  auto asset = ReadBlitAssetName(json);
  // "clear"/"remove": true, or "enabled": false, turns the event into a stop
  // for effects already running rather than a new one. Without it the only way
  // to end a long Blit early was to author its duration exactly.
  bool const clear = ReadBool(json, "clear").value_or(false) || ReadBool(json, "remove").value_or(false) ||
                     !ReadBool(json, "enabled").value_or(true);
  std::string normalizedAsset = asset.has_value() ? NormalizeAssetKey(*asset) : std::string();
  if (GetDisableAllBlits()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit skipped at time {}: Disable All Blits enabled asset='{}'",
                       customEventData->time, asset.has_value() ? std::string(*asset) : std::string("<none>"));
    }
    return;
  }
  if (asset.has_value() && GetDisableBeat0FilmgrainBlit() && std::fabs(customEventData->time) < 0.01f &&
      normalizedAsset.find("filmgrain") != std::string::npos) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit skipped at time {}: beat-0 filmgrain isolation asset='{}'",
                       customEventData->time, std::string(*asset));
    }
    return;
  }
  // Crash recovery (0.15.2): an effect that was starting when this map last
  // went down is skipped; every other effect is recorded as it starts, so the
  // next launch knows which one to blame if this run goes down too.
  if (asset.has_value() && !clear) {
    if (IsRecoverySkippedBlit(normalizedAsset)) {
      PaperLogger.warn("Vivify Blit skipped at time {}: '{}' was starting when this map last went down (crash "
                       "recovery; 'Forget Crash Recovery' in the advanced settings brings it back)",
                       customEventData->time, std::string(*asset));
      return;
    }
    Recovery::BlitStarted(normalizedAsset, CurrentSongTime(), static_cast<int64_t>(std::time(nullptr)));
  }
  float const durationBeats = ReadFloat(json, "duration").value_or(0.0f);
  float const duration = DurationBeatsToSeconds(durationBeats);
  UnityEngine::Material* material = nullptr;
  if (asset.has_value() && !clear) {
    material = GetAssetAs<UnityEngine::Material>(*asset);
    if (material == nullptr) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify Blit material load failed at time {}: asset='{}'",
                         customEventData->time, std::string(*asset));
      }
      return;
    }
    RepairMaterialShader(material, *asset);
    LogMaterialShader("Blit", *asset, material);
    auto properties = ParseMaterialProperties(json);
    if (!properties.empty()) {
      Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"));
      float const startTime = customEventData->time;
      float const currentSongTime = CurrentSongTime();
      bool const completed = duration <= 0.0f || startTime + duration <= currentSongTime;
      float const initialProgress = completed ? 1.0f : 0.0f;
      for (auto const& property : properties) ApplyMaterialProperty(material, property, initialProgress);
      if (!completed) {
        QueueMaterialPropertyAnimation(material, properties, startTime, duration, easing);
      }
    }
  }
  int const priority = ReadInt(json, "priority").value_or(0);
  int const pass = ReadInt(json, "pass").value_or(-1);
  std::string sourceStr{ReadStringView(json, "source").value_or(kMainCameraId)};
  std::vector<std::string> targets = ReadStringListOrSingle(json, "destination");
  if (targets.empty()) targets.emplace_back(kMainCameraId);
  PostProcessingOrder const order = ParsePostProcessingOrder(json);
  auto& effects = [&]() -> std::vector<ActiveBlitEffect>& {
    switch (order) {
      case PostProcessingOrder::BeforeMainEffect: return _preEffects;
      case PostProcessingOrder::AfterMainEffect: return _postEffects;
      default: return _midEffects[static_cast<int>(order)];
    }
  }();
  BlitMaterialData bd{material, priority, std::move(sourceStr), std::move(targets), pass,
                      std::nullopt, std::move(normalizedAsset), customEventData->time};
  if (clear) {
    size_t const before = effects.size();
    RemoveMatchingBlitEffects(effects, json, bd, asset.has_value());
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit clear: time={} asset='{}' order={} removed={}", customEventData->time,
                       asset.has_value() ? std::string(*asset) : std::string("<any>"), static_cast<int>(order),
                       before - effects.size());
    }
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify Blit event: time={} asset='{}' source='{}' targets={} priority={} pass={} order={} durationBeats={}",
                     customEventData->time,
                     asset.has_value() ? std::string(*asset) : std::string("<none>"),
                     bd.source, bd.targets.size(), priority, pass,
                     static_cast<int>(order), durationBeats);
  }
  float const songTime = CurrentSongTime();
  if (durationBeats == 0.0f) {

    bd.frame = static_cast<int>(UnityEngine::Time::get_frameCount());
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), 0.0f});
  } else if (durationBeats > 0.0f && songTime <= customEventData->time + duration) {
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), customEventData->time + duration});
  } else if (durationBeats < 0.0f) {
    // A negative duration keeps the effect until a clearing event (or the end
    // of the map) removes it. It is stored with an expiry at infinity rather
    // than 0, because 0 is how a single-frame effect is marked.
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), std::numeric_limits<float>::infinity()});
  }

}

// A blit draws into either the eye texture -- a two-slice array, one pass for
// both eyes, which takes the multiview programs a converted map's shaders
// have -- or into a single texture (screen textures, temporaries), where a
// multiview program draws nothing. For the latter, a converted map's blit
// material is swapped for a copy on the single-view build of its shader from
// the blit companion (VivifyAssets.cpp). Quest-built shaders have both
// programs already and are never in _singleViewShaders.
UnityEngine::Material* Runtime::BlitMaterialFor(UnityEngine::Material* material,
                                                UnityEngine::RenderTexture* destination) {
  auto* separate = SeparateEye::BlitMaterial(material);
  if (separate != material) return separate;
  if (material == nullptr || _singleViewShaders.empty() || !IsAlive(material) || !IsAlive(destination)) {
    return material;
  }
  auto descriptor = destination->get_descriptor();
  if (descriptor.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray ||
      descriptor.get_volumeDepth() > 1) {
    return material;
  }
  auto* shader = material->get_shader().unsafePtr();
  if (!IsAlive(shader)) return material;
  auto const singleView = _singleViewShaders.find(ShaderNameForLog(shader));
  if (singleView == _singleViewShaders.end() || !IsAlive(singleView->second)) return material;
  auto& copy = _singleViewBlitMaterials[material];
  if (!IsAlive(copy)) {
    copy = UnityEngine::Material::New_ctor(material);
    copy->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
    PaperLogger.info("Vivify Blit: material '{}' draws into single-screen textures with the single-view build "
                     "of '{}'", ToStdString(material->get_name()), singleView->first);
  }
  // Event and camera callbacks can change properties between blits.
  // Refresh on use without sweeping inactive cached copies. A live source
  // shader change must also select its new companion on this cached object;
  // CopyPropertiesFromMaterial deliberately does not copy the shader.
  copy->CopyPropertiesFromMaterial(material);
  if (copy->get_shader().unsafePtr() != singleView->second) copy->set_shader(singleView->second);
  return copy;
}

// Whether a blit with this material into this texture has to be run one eye
// at a time: the texture is the two-eye array and the material's shader is a
// Quest-built one whose programs are all single-view (found by the bundle scan
// at selection). RSIH's end-of-song "TV shutdown" is one: its shader has no
// stereo code, so Unity compiled no multiview program for it.
bool Runtime::NeedsPerEyeBlit(UnityEngine::Material* material, UnityEngine::RenderTexture* destination) {
  if ((_singleViewOnlyNativeShaders.empty() && _stereoDropsTextureShaders.empty()) || !IsAlive(material) ||
      !IsAlive(destination)) {
    return false;
  }
  auto descriptor = destination->get_descriptor();
  if (descriptor.get_dimension() != UnityEngine::Rendering::TextureDimension::Tex2DArray ||
      descriptor.get_volumeDepth() != 2) {
    return false;
  }
  auto* shader = material->get_shader().unsafePtr();
  if (!IsAlive(shader)) return false;
  std::string const name = ShaderNameForLog(shader);
  bool const dropsTexture = _stereoDropsTextureShaders.contains(name);
  if (!_singleViewOnlyNativeShaders.contains(name) && !dropsTexture) return false;
  if (_perEyeBlitLogged.insert(name).second) {
    PaperLogger.info("Vivify Blit: '{}' {} in this map's Quest bundle; running its one-eye program once per eye",
                     name, dropsTexture ? "leaves a texture out of its two-eye program" : "has no two-eye program");
  }
  return true;
}

void Runtime::UpdateBlitEffects() {
  if (GetDisableAllBlits() || _blitGuardAllOff) {
    if ((!_preEffects.empty() || !_postEffects.empty() || HasMidRenderEffects()) && GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit effects cleared: Disable All Blits enabled pre={} post={}",
                       _preEffects.size(), _postEffects.size());
    }
    _preEffects.clear();
    _postEffects.clear();
    for (auto& bucket : _midEffects) bucket.clear();
    return;
  }
  float const songTime = CurrentSongTime();
  int const frame = static_cast<int>(UnityEngine::Time::get_frameCount());
  bool const dropBeat0Filmgrain = GetDisableBeat0FilmgrainBlit();
  auto cleanup = [&](std::vector<ActiveBlitEffect>& effects) {
    effects.erase(std::remove_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& e) {
      if (dropBeat0Filmgrain && std::fabs(e.data.eventBeat) < 0.01f &&
          e.data.asset.find("filmgrain") != std::string::npos) {
        return true;
      }
      if (e.data.frame.has_value()) return e.data.frame.value() != frame;
      return e.expireTime > 0.0f && songTime >= e.expireTime;
    }), effects.end());
  };
  cleanup(_preEffects);
  cleanup(_postEffects);
  for (auto& bucket : _midEffects) cleanup(bucket);
}

void Runtime::HandleCreateScreenTexture(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  if (!id.has_value()) return;
  std::string name(*id);
  DestroyDeclaredTextureById(name);
  DeclaredTextureData dt;
  dt.name = name;
  dt.propertyId = UnityEngine::Shader::PropertyToID(StringW(name));
  dt.xRatio = ReadFloat(json, "xRatio").value_or(1.0f);
  dt.yRatio = ReadFloat(json, "yRatio").value_or(1.0f);
  if (auto w = ReadInt(json, "width")) dt.width = *w;
  if (auto h = ReadInt(json, "height")) dt.height = *h;
  if (auto fmt = ReadStringView(json, "colorFormat"))
    dt.format = [&]() -> std::optional<UnityEngine::RenderTextureFormat> {
      auto s = *fmt;
      if (s == "ARGB32") return UnityEngine::RenderTextureFormat::ARGB32;
      if (s == "Depth") return UnityEngine::RenderTextureFormat::Depth;
      if (s == "ARGBHalf") return UnityEngine::RenderTextureFormat::ARGBHalf;
      if (s == "Shadowmap") return UnityEngine::RenderTextureFormat::Shadowmap;
      if (s == "RGB565") return UnityEngine::RenderTextureFormat::RGB565;
      if (s == "ARGB4444") return UnityEngine::RenderTextureFormat::ARGB4444;
      if (s == "ARGB1555") return UnityEngine::RenderTextureFormat::ARGB1555;
      if (s == "Default") return UnityEngine::RenderTextureFormat::Default;
      if (s == "ARGBFloat") return UnityEngine::RenderTextureFormat::ARGBFloat;
      if (s == "RGFloat") return UnityEngine::RenderTextureFormat::RGFloat;
      if (s == "RFloat") return UnityEngine::RenderTextureFormat::RFloat;
      if (s == "RHalf") return UnityEngine::RenderTextureFormat::RHalf;
      if (s == "R8") return UnityEngine::RenderTextureFormat::R8;
      if (s == "DefaultHDR") return UnityEngine::RenderTextureFormat::DefaultHDR;
      // The integer and packed formats Unity's enum also names. Maps that pack
      // IDs or bit masks into a screen texture ask for these, and an unknown
      // name used to fall through to ARGB32 -- which quantises exactly the data
      // those maps need to read back intact. SupportedRenderTextureFormat still
      // downgrades any this GPU cannot render to.
      if (s == "ARGB2101010") return UnityEngine::RenderTextureFormat::ARGB2101010;
      if (s == "ARGB64") return UnityEngine::RenderTextureFormat::ARGB64;
      if (s == "RGHalf") return UnityEngine::RenderTextureFormat::RGHalf;
      if (s == "ARGBInt") return UnityEngine::RenderTextureFormat::ARGBInt;
      if (s == "RGInt") return UnityEngine::RenderTextureFormat::RGInt;
      if (s == "RInt") return UnityEngine::RenderTextureFormat::RInt;
      if (s == "BGRA32") return UnityEngine::RenderTextureFormat::BGRA32;
      if (s == "RGB111110Float") return UnityEngine::RenderTextureFormat::RGB111110Float;
      if (s == "RG32") return UnityEngine::RenderTextureFormat::RG32;
      if (s == "RGBAUShort") return UnityEngine::RenderTextureFormat::RGBAUShort;
      if (s == "RG16") return UnityEngine::RenderTextureFormat::RG16;
      if (s == "R16") return UnityEngine::RenderTextureFormat::R16;
      return std::nullopt;
    }();
  if (auto fm = ReadStringView(json, "filterMode"))
    dt.filterMode = [&]() -> std::optional<UnityEngine::FilterMode> {
      auto s = *fm;
      if (s == "Point") return UnityEngine::FilterMode::Point;
      if (s == "Bilinear") return UnityEngine::FilterMode::Bilinear;
      if (s == "Trilinear") return UnityEngine::FilterMode::Trilinear;
      return std::nullopt;
    }();
  if (!std::isfinite(dt.xRatio) || dt.xRatio <= 0.0f) dt.xRatio = 1.0f;
  if (!std::isfinite(dt.yRatio) || dt.yRatio <= 0.0f) dt.yRatio = 1.0f;
  if (dt.format) dt.format = SupportedRenderTextureFormat(*dt.format, "CreateScreenTexture:" + name);
  _declaredTextures[name] = std::move(dt);
  // Like PC Vivify, allocation needs the camera's actual image descriptor.
  // A window-sized ARGB32 Tex2D guess loses HDR and the right-eye layer.
  PrepareDeclaredTextures();
}

void Runtime::PrepareDeclaredTextures() {
  if (!_hasMainDescriptor) return;
  for (auto& [name, data] : _declaredTextures) {
    auto desc = _cachedMainDescriptor;
    desc.set_msaaSamples(1);
    desc.set_width(RenderTexturePolicy::DeclaredDimension(data.width, EffectSize(desc.get_width()), data.xRatio, kMaxRenderTextureSize));
    desc.set_height(RenderTexturePolicy::DeclaredDimension(data.height, EffectSize(desc.get_height()), data.yRatio, kMaxRenderTextureSize));
    if (data.format) desc.set_colorFormat(*data.format);
    auto const requested = RenderTexturePolicy::Describe(desc);
    bool const alive = IsAlive(data.texture);
    if (RenderTexturePolicy::NeedsAllocation(data.allocation, requested, alive, alive && data.texture->IsCreated())) {
      ReleaseDeclaredTextureData(data);
      data.texture = UnityEngine::RenderTexture::New_ctor(desc);
      if (IsAlive(data.texture) && data.filterMode) data.texture->set_filterMode(*data.filterMode);
      if (!IsAlive(data.texture) || !data.texture->Create()) {
        ReleaseDeclaredTextureData(data);
        continue;
      }
      data.allocation = requested;
      ClearRenderTexture(data.texture);
      if (GetVivifyDebugLogging()) {
        PaperLogger.info("Vivify CreateScreenTexture: id='{}' size={}x{} layers={} dimension={} graphicsFormat={} filter={}",
                         name, desc.get_width(), desc.get_height(), desc.get_volumeDepth(), desc.get_dimension().value__,
                         desc.get_graphicsFormat().value__, data.filterMode ? data.filterMode->value__ : -1);
      }
    }
    // Rebind when the rendering camera resumes; another camera may have
    // replaced shader globals. Existing feedback contents remain untouched.
    if (IsAlive(data.texture)) UnityEngine::Shader::SetGlobalTexture(data.propertyId, data.texture);
  }
}

void Runtime::ReleaseDeclaredTextureData(DeclaredTextureData& data) {
  if (data.propertyId != 0) ClearOwnedTextureBinding(data.propertyId, data.texture);
  if (IsAlive(data.texture)) {
    data.texture->Release();
    UnityEngine::Object::Destroy(data.texture);
  }
  data.texture = nullptr;
  data.allocation.reset();
}

bool Runtime::DestroyDeclaredTextureById(std::string const& id) {
  auto it = _declaredTextures.find(id);
  if (it == _declaredTextures.end()) return false;
  ReleaseDeclaredTextureData(it->second);
  _declaredTextures.erase(it);
  return true;
}

void Runtime::HandleCreateCamera(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  if (!id.has_value()) return;
  std::string name(*id);
  DestroySecondaryCameraById(name);
  if (GetDisableCreateCameraDepth()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify CreateCamera skipped: isolation toggle enabled id='{}'", name);
    }
    return;
  }
  SecondaryCameraData cam;
  cam.name = name;
  if (auto tex = ReadStringView(json, "texture")) {
    cam.textureName = std::string(*tex);
    cam.texturePropertyId = UnityEngine::Shader::PropertyToID(StringW(*tex));
  }
  if (auto dtex = ReadStringView(json, "depthTexture")) {
    cam.depthTextureName = std::string(*dtex);
    cam.depthTexturePropertyId = UnityEngine::Shader::PropertyToID(StringW(*dtex));
  }

  if (auto stored = _cameraProperties.find(name); stored != _cameraProperties.end()) {
    cam.properties = stored->second;
  }
  auto* propsVal = ReadValuePtr(json, "properties");
  if (propsVal != nullptr && propsVal->IsObject()) {
    ParseCameraPropertyData(*propsVal, cam.properties);
  }
  _cameraProperties[name] = cam.properties;
  auto mainCam = UnityEngine::Camera::get_main();
  auto* mainCamPtr = mainCam.unsafePtr();
  auto* go = UnityEngine::GameObject::New_ctor(StringW("VivifyCamera_" + name));
  if (IsAlive(mainCamPtr)) {
    go->get_transform()->SetParent(mainCam->get_transform(), false);
  }
  cam.camera = go->AddComponent<UnityEngine::Camera*>();
  if (IsAlive(mainCamPtr)) {
    cam.camera->CopyFrom(mainCamPtr);
    SyncCameraBloom(mainCamPtr, cam.camera, nullptr, false);
  }
  cam.camera->set_depth(IsAlive(mainCamPtr) ? mainCam->get_depth() - 1 : -2.0f);
  EnsureMultipassKeywordController(go);
  int w = 1024, h = 512;
  if (IsAlive(mainCamPtr)) {
    w = std::max(1, mainCam->get_pixelWidth());
    h = std::max(1, mainCam->get_pixelHeight());
  }
  bool const wantColor = cam.texturePropertyId.has_value();
  bool const wantDepth = cam.depthTexturePropertyId.has_value();

  if (!wantColor && !wantDepth) {
    cam.camera->set_enabled(false);
  } else {
    // Deliberately no targetTexture and no SetTargetBuffers here.
    //
    // Upstream Vivify puts the capture in SecondaryCameraController and notes
    // why: assigning a target texture disables stereo on the camera. This port
    // did assign one, which on a stereo headset is exactly the wrong thing --
    // and for a camera that only asked for a depth texture it also skipped
    // creating the controller altogether, so nothing ever captured anything.
    // The camera now renders normally (one frame ahead of the main camera) and
    // the controller copies what it needs out of OnRenderImage, per eye.
    cam.camera->set_targetTexture(nullptr);
    cam.camera->set_enabled(true);

    auto* controller = go->AddComponent<SecondaryCameraController*>();
    if (IsAlive(controller)) {
      controller->colorKey = cam.texturePropertyId;
      controller->depthKey = cam.depthTexturePropertyId;
    } else {
      PaperLogger.warn("Vivify CreateCamera '{}': could not attach SecondaryCameraController", name);
    }
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify CreateCamera: id='{}' colorTexture='{}' depthTexture='{}' size={}x{} mainEffect={}",
                     name, cam.textureName.value_or("<none>"), cam.depthTextureName.value_or("<none>"),
                     w, h, BoolText(cam.properties.mainEffect.value_or(true)));
  }
  // A camera asked for a depth texture is useless without Unity generating one,
  // and _CameraDepthTexture only exists when the camera's depthTextureMode says
  // so. Upstream leaves this to the map declaring depthTextureMode itself;
  // forcing the bit on costs nothing when it was already set and avoids an
  // empty depth texture when the map forgot (or set it on a different camera).
  if (wantDepth && !GetDisableCreateCameraDepth() && IsAlive(cam.camera)) {
    int const mode = cam.camera->get_depthTextureMode().value__;
    int const withDepth = mode | UnityEngine::DepthTextureMode::Depth.value__;
    if (withDepth != mode) {
      cam.camera->set_depthTextureMode(UnityEngine::DepthTextureMode(withDepth));
    }
  }

  // The depth requirement is part of this camera's baseline, so clearing
  // a later override cannot disable an outstanding depth-texture capture.
  ApplyCameraProperties(cam.camera, cam.properties);
  ApplyCameraGameObjectProperties(cam.camera, cam.properties);

  _secondaryCameras[name] = std::move(cam);
}

void Runtime::ReleaseSecondaryCameraData(SecondaryCameraData& data) {
  _cameraDefaults.erase(data.camera);
  // Captures live in the controller's per-eye maps. Unbind them before
  // destroying the GameObject; its OnDestroy also covers external destruction.
  if (IsAlive(data.camera)) {
    auto* go = data.camera->get_gameObject().unsafePtr();
    auto* controller = IsAlive(go) ? go->GetComponent<SecondaryCameraController*>() : nullptr;
    if (IsAlive(controller)) controller->ReleaseGlobalBindings();
  }
  if (data.texturePropertyId.has_value()) {
    ClearOwnedTextureBinding(data.texturePropertyId.value(), data.colorRT);
  }
  if (data.depthTexturePropertyId.has_value()) {
    ClearOwnedTextureBinding(data.depthTexturePropertyId.value(), data.depthRT);
  }
  if (IsAlive(data.colorRT)) {
    data.colorRT->Release();
    UnityEngine::Object::Destroy(data.colorRT);
  }
  if (IsAlive(data.depthRT)) {
    data.depthRT->Release();
    UnityEngine::Object::Destroy(data.depthRT);
  }
  if (IsAlive(data.camera)) {
    UnityEngine::Object::Destroy(data.camera->get_gameObject());
  }
  data.colorRT = nullptr;
  data.depthRT = nullptr;
  data.camera = nullptr;
}

UnityEngine::RenderTexture* Runtime::SecondaryCameraColorRT(SecondaryCameraData const& cam) {
  if (IsAlive(cam.colorRT)) return cam.colorRT;
  if (!IsAlive(cam.camera)) return nullptr;
  auto* go = cam.camera->get_gameObject().unsafePtr();
  if (!IsAlive(go)) return nullptr;
  auto* controller = go->GetComponent<SecondaryCameraController*>();
  if (!IsAlive(controller) || controller->colorTextures.empty()) return nullptr;
  int eye = 0;
  if (auto* mainCam = IsAlive(_lastMainCameraGO) ? _lastMainCameraGO->GetComponent<UnityEngine::Camera*>() : nullptr;
      IsAlive(mainCam)) {
    eye = mainCam->get_stereoActiveEye().value__;
  }
  if (auto it = controller->colorTextures.find(eye); it != controller->colorTextures.end() && IsAlive(it->second)) {
    return it->second;
  }
  return controller->colorTextures.begin()->second;
}

// Rebinds every secondary camera's captured textures as global shader
// properties for the eye about to be rendered, once per frame -- the same thing
// upstream does from PostProcessingController.OnPreRender.
//
// This used to bail out on `!cam.texturePropertyId.has_value()`, so a camera
// that declared only a depth texture -- which is precisely what a raymarching
// map creates, since it wants scene depth rather than a colour copy -- was
// skipped entirely and its depth texture was never bound to the name the map's
// shader samples.
void Runtime::BindSecondaryCameraTextures() {
  PrepareDeclaredTextures();
  if (_secondaryCameras.empty()) return;
  auto* mainCam = IsAlive(_lastMainCameraGO) ? _lastMainCameraGO->GetComponent<UnityEngine::Camera*>() : nullptr;
  int const eye = IsAlive(mainCam) ? mainCam->get_stereoActiveEye().value__ : 0;

  for (auto& [n, cam] : _secondaryCameras) {
    if (!IsAlive(cam.camera)) continue;
    if (!cam.texturePropertyId.has_value() && !cam.depthTexturePropertyId.has_value()) continue;

    auto* go = cam.camera->get_gameObject().unsafePtr();
    if (!IsAlive(go)) continue;
    auto* controller = go->GetComponent<SecondaryCameraController*>();
    if (!IsAlive(controller)) continue;

    auto bind = [eye](std::optional<int> const& propertyId,
                      std::unordered_map<int, UnityEngine::RenderTexture*> const& textures) {
      if (!propertyId.has_value() || textures.empty()) return;
      auto it = textures.find(eye);
      if (it == textures.end() || !IsManagedAlive(it->second)) {
        // Before both eyes have been rendered once there may only be one
        // texture; binding it is better than leaving the property unset.
        it = textures.begin();
        if (!IsManagedAlive(it->second)) return;
      }
      UnityEngine::Shader::SetGlobalTexture(propertyId.value(),
                                            static_cast<UnityEngine::Texture*>(it->second));
    };

    bind(cam.texturePropertyId, controller->colorTextures);
    bind(cam.depthTexturePropertyId, controller->depthTextures);
  }
}

void Runtime::RestoreSecondaryCullingLayers() {
  for (auto& [name, cam] : _secondaryCameras) {
    if (!IsAlive(cam.camera)) continue;
    auto* go = cam.camera->get_gameObject().unsafePtr();
    if (!IsAlive(go)) continue;
    auto* culling = go->GetComponent<CullingCameraController*>();
    if (IsAlive(culling)) {
      culling->CullingPostRender();
    }
  }
}

bool Runtime::DestroySecondaryCameraById(std::string const& id) {
  auto it = _secondaryCameras.find(id);
  if (it == _secondaryCameras.end()) return false;
  ReleaseSecondaryCameraData(it->second);
  _secondaryCameras.erase(it);
  _cameraProperties.erase(id);
  return true;
}

void Runtime::CaptureSecondaryDepth(UnityEngine::RenderTexture* destination) {
  try { if (DepthCopy::Copy(destination)) return; }
  catch (...) { PaperLogger.error("Vivify secondary-camera depth-copy draw threw"); }
  ClearRenderTexture(destination);
  NoteVisualProblem("Secondary-camera depth capture failed: PC depth-copy shader or slice binding unavailable.");
}

void Runtime::ConfigureMirrorCamera(UnityEngine::Camera* camera) {
  if (!_currentBeatmapData || _isResetting || !IsAlive(camera)) return;
  auto* go = camera->get_gameObject().unsafePtr();
  if (!IsAlive(go)) return;
  EnsureCullingController(go);
  if (!IsAlive(go->GetComponent<SingleViewCameraShaders*>())) go->AddComponent<SingleViewCameraShaders*>();
  // PC's mirror CameraPropertyController has no separate id, so it inherits
  // the map's main-camera properties while keeping its reflected matrices.
  if (auto properties = _cameraProperties.find(std::string(kMainCameraId)); properties != _cameraProperties.end()) {
    ApplyCameraProperties(camera, properties->second);
    ApplyCameraGameObjectProperties(camera, properties->second);
  }
}

bool Runtime::RenderSecondaryCameraMainEffect(UnityEngine::Camera* camera, UnityEngine::RenderTexture* src,
                                             UnityEngine::RenderTexture* dest) {
  if (_isResetting || _pauseMenuActive || !_currentBeatmapData || !IsAlive(camera) || !IsAlive(src) || !IsAlive(dest)) return false;
  auto at = std::find_if(_secondaryCameras.begin(), _secondaryCameras.end(),
                         [camera](auto const& item) { return item.second.camera == camera; });
  if (at == _secondaryCameras.end() || !at->second.properties.mainEffect.value_or(true)) return false;
  auto* mainEffectController = IsAlive(_lastMainCameraGO)
      ? _lastMainCameraGO->GetComponent<GlobalNamespace::MainEffectController*>() : nullptr;
  if (!IsManagedAlive(mainEffectController) ||
      mainEffectController->_mainEffectContainer.ptr() == nullptr ||
      !IsManagedAlive(mainEffectController->_mainEffectContainer)) {
    return false;
  }
  auto mainEffectSO = mainEffectController->_mainEffectContainer->get_mainEffect();
  if (mainEffectSO.ptr() == nullptr || !IsManagedAlive(mainEffectSO) || !mainEffectSO->get_hasPostProcessEffect()) {
    return false;
  }
  // Match PC MainEffectRenderer: execute while THIS secondary camera's
  // depth, projection and bloom data are current. Filtering it later from
  // the main camera also processed stale captures repeatedly.
  mainEffectController->OnPreRender();
  try { mainEffectSO->Render(src, dest, mainEffectController->____fadeValue); }
  catch (...) { mainEffectController->OnPostRender(); throw; }
  mainEffectController->OnPostRender();
  return true;
}

std::optional<UnityEngine::DepthTextureMode> Runtime::ParseDepthTextureModeName(std::string_view s) {
  if (s == "None") return UnityEngine::DepthTextureMode::None;
  if (s == "Depth") return UnityEngine::DepthTextureMode::Depth;
  if (s == "DepthNormals") return UnityEngine::DepthTextureMode::DepthNormals;
  if (s == "MotionVectors") return UnityEngine::DepthTextureMode::MotionVectors;
  return std::nullopt;
}

std::optional<UnityEngine::DepthTextureMode> Runtime::ParseDepthTextureModeValue(rapidjson::Value const& value) {
  if (value.IsString()) {
    return ParseDepthTextureModeName(std::string_view(value.GetString(), value.GetStringLength()));
  }
  if (!value.IsArray()) {
    return std::nullopt;
  }
  int flags = 0;
  bool parsedAny = false;
  for (auto const& item : value.GetArray()) {
    if (!item.IsString()) continue;
    auto mode = ParseDepthTextureModeName(std::string_view(item.GetString(), item.GetStringLength()));
    if (!mode.has_value()) continue;
    flags |= mode->value__;
    parsedAny = true;
  }
  if (!parsedAny) return std::nullopt;
  return UnityEngine::DepthTextureMode(flags);
}

void Runtime::ParseCameraPropertyData(rapidjson::Value const& json, CameraPropertyData& out) {
  if (auto* dtm = ReadValuePtr(json, "depthTextureMode")) {
    out.depthTextureMode = dtm->IsNull() ? std::nullopt : ParseDepthTextureModeValue(*dtm);
    if (GetDisableCreateCameraDepth()) out.depthTextureMode.reset();
  }
  if (auto* cf = ReadValuePtr(json, "clearFlags")) {
    out.clearFlags = std::nullopt;
    if (cf->IsString()) {
      std::string_view s(cf->GetString(), cf->GetStringLength());
      if (s == "Skybox") out.clearFlags = UnityEngine::CameraClearFlags::Skybox;
      else if (s == "Color" || s == "SolidColor") out.clearFlags = UnityEngine::CameraClearFlags::Color;
      else if (s == "Depth") out.clearFlags = UnityEngine::CameraClearFlags::Depth;
      else if (s == "Nothing") out.clearFlags = UnityEngine::CameraClearFlags::Nothing;
    }
  }
  if (auto* bgVal = ReadValuePtr(json, "backgroundColor")) {
    out.backgroundColor = bgVal->IsNull() ? std::nullopt : ReadColorArray(*bgVal);
  }
  if (auto* cullingVal = ReadValuePtr(json, "culling")) {
    if (cullingVal->IsObject()) {
      CullingMaskData culling;
      bool const v2 = _currentBeatmapData != nullptr && _currentBeatmapData->v2orEarlier;
      culling.tracks = ReadTracks(*cullingVal, v2);
      culling.whitelist = ReadBool(*cullingVal, "whitelist").value_or(false);
      // An empty whitelist still excludes everything. Only JSON null removes
      // the culling selection; an object with no tracks remains a selection.
      out.culling = std::move(culling);
    } else {
      out.culling.reset();
    }
  }
  if (auto* bp = ReadValuePtr(json, "bloomPrePass")) {
    out.bloomPrePass = bp->IsBool() ? std::optional(bp->GetBool()) : std::nullopt;
  }
  if (auto* me = ReadValuePtr(json, "mainEffect")) {
    out.mainEffect = me->IsBool() ? std::optional(me->GetBool()) : std::nullopt;
  }
}

void Runtime::ApplyCameraProperties(UnityEngine::Camera* camera, CameraPropertyData const& props) {
  if (!IsAlive(camera)) return;
  bool const isMainCamera = camera->get_gameObject().unsafePtr() == _lastMainCameraGO;
  CameraDefaults const* defaults = nullptr;
  if (!isMainCamera) {
    auto& saved = _cameraDefaults[camera];
    int const id = camera->GetInstanceID();
    if (!saved.camera || !IsAlive(saved.camera.ptr()) || saved.instanceId != id) {
      saved.camera = camera; saved.instanceId = id;
      saved.depthMode = camera->get_depthTextureMode().value__;
      saved.clearFlags = camera->get_clearFlags(); saved.backgroundColor = camera->get_backgroundColor();
    }
    defaults = &saved;
  }
  if (props.depthTextureMode.has_value()) {
    int mode = props.depthTextureMode->value__;

    if (isMainCamera && _mainCamOriginalDepthMode.has_value()) {
      mode |= _mainCamOriginalDepthMode.value();
    } else if (defaults) mode |= defaults->depthMode;
    camera->set_depthTextureMode(UnityEngine::DepthTextureMode(mode));
  } else if (isMainCamera && _mainCamOriginalDepthMode.has_value()) {
    camera->set_depthTextureMode(UnityEngine::DepthTextureMode(_mainCamOriginalDepthMode.value()));
  } else if (defaults) {
    camera->set_depthTextureMode(UnityEngine::DepthTextureMode(defaults->depthMode));
  }
  if (props.clearFlags.has_value()) {
    camera->set_clearFlags(props.clearFlags.value());
  } else if (isMainCamera && _mainCamOriginalClearFlags.has_value()) {
    camera->set_clearFlags(UnityEngine::CameraClearFlags(_mainCamOriginalClearFlags.value()));
  } else if (defaults) {
    camera->set_clearFlags(defaults->clearFlags);
  }
  if (props.backgroundColor.has_value()) {
    camera->set_backgroundColor(props.backgroundColor.value());
  } else if (isMainCamera && _mainCamOriginalBackgroundColor.has_value()) {
    camera->set_backgroundColor(_mainCamOriginalBackgroundColor.value());
  } else if (defaults) {
    camera->set_backgroundColor(defaults->backgroundColor);
  }
}

void Runtime::ApplyCameraGameObjectProperties(UnityEngine::Camera* camera, CameraPropertyData const& props) {
  if (!IsAlive(camera)) return;
  auto* cameraGO = camera->get_gameObject().unsafePtr();
  if (!IsAlive(cameraGO)) return;
  ApplyCullingProperty(cameraGO, props.culling);
  auto* bloomPrePass = cameraGO->GetComponent<GlobalNamespace::BloomPrePass*>();
  if (IsAlive(bloomPrePass)) {
    bloomPrePass->set_enabled(props.bloomPrePass.value_or(true));
  }
}

CullingCameraController* Runtime::EnsureCullingController(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return nullptr;
  auto* controller = gameObject->GetComponent<CullingCameraController*>();
  if (!IsAlive(controller)) {
    controller = gameObject->AddComponent<CullingCameraController*>();
  }
  return controller;
}

void Runtime::ApplyCullingProperty(UnityEngine::GameObject* cameraGO, std::optional<CullingMaskData> const& culling) {
  if (!IsAlive(cameraGO)) return;
  if (culling.has_value()) {
    auto* controller = EnsureCullingController(cameraGO);
    if (controller != nullptr) {
      controller->SetCullingData(culling->whitelist, culling->tracks);
      controller->set_enabled(true);
    }
  } else {
    auto* controller = cameraGO->GetComponent<CullingCameraController*>();
    if (IsAlive(controller)) {
      controller->ClearCullingData();
      controller->set_enabled(false);
    }
  }
}

void Runtime::HandleSetCameraProperty(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  std::string camId = id.has_value() ? std::string(*id) : std::string(kMainCameraId);
  auto* propsVal = ReadValuePtr(json, "properties");
  if (propsVal == nullptr || !propsVal->IsObject()) return;
  if (GetDisableCreateCameraDepth() && camId != kMainCameraId) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify SetCameraProperty skipped: CreateCamera/Depth isolation camera='{}'", camId);
    }
    return;
  }

  auto& stored = _cameraProperties[camId];
  ParseCameraPropertyData(*propsVal, stored);
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify SetCameraProperty: camera='{}' depthTextureMode={} clearFlags={} hasBackground={} culling={} bloomPrePass={} mainEffect={}",
                     camId,
                     stored.depthTextureMode.has_value() ? stored.depthTextureMode->value__ : -1,
                     stored.clearFlags.has_value() ? stored.clearFlags->value__ : -1,
                     BoolText(stored.backgroundColor.has_value()),
                     BoolText(stored.culling.has_value()),
                     stored.bloomPrePass.has_value() ? BoolText(*stored.bloomPrePass) : "unset",
                     stored.mainEffect.has_value() ? BoolText(*stored.mainEffect) : "unset");
  }
  if (camId == kMainCameraId) {
    _mainCameraPropsDirty = true;
    auto mainCam = UnityEngine::Camera::get_main();
    auto* mainCamPtr = mainCam.unsafePtr();
    if (IsAlive(mainCamPtr)) {
      CaptureMainCameraOriginals(mainCamPtr, mainCamPtr->get_gameObject().unsafePtr());
      ApplyCameraProperties(mainCamPtr, stored);
      ApplyCameraGameObjectProperties(mainCamPtr, stored);
    }
  } else {
    auto it = _secondaryCameras.find(camId);
    if (it != _secondaryCameras.end()) {

      ParseCameraPropertyData(*propsVal, it->second.properties);
      ApplyCameraProperties(it->second.camera, it->second.properties);
      ApplyCameraGameObjectProperties(it->second.camera, it->second.properties);
    }
  }
}

void Runtime::CaptureMainCameraOriginals(UnityEngine::Camera* mainCam, UnityEngine::GameObject* mainCamGO) {
  if (!IsAlive(mainCam) || mainCamGO != _lastMainCameraGO) return;
  if (!_mainCamOriginalDepthMode.has_value()) {
    _mainCamOriginalDepthMode = mainCam->get_depthTextureMode().value__;
  }
  if (!_mainCamOriginalClearFlags.has_value()) {
    _mainCamOriginalClearFlags = mainCam->get_clearFlags().value__;
  }
  if (!_mainCamOriginalBackgroundColor.has_value()) {
    _mainCamOriginalBackgroundColor = mainCam->get_backgroundColor();
  }
}

void Runtime::RestoreMainCameraOriginals(bool restoreAuxiliary) {
  if (restoreAuxiliary) {
    for (auto const& [_, saved] : _cameraDefaults) {
      if (!saved.camera || !IsAlive(saved.camera.ptr()) || saved.camera->GetInstanceID() != saved.instanceId) continue;
      saved.camera->set_depthTextureMode(UnityEngine::DepthTextureMode(saved.depthMode));
      saved.camera->set_clearFlags(saved.clearFlags);
      saved.camera->set_backgroundColor(saved.backgroundColor);
      ApplyCullingProperty(saved.camera->get_gameObject().unsafePtr(), std::nullopt);
      if (auto* bloom = saved.camera->GetComponent<GlobalNamespace::BloomPrePass*>(); IsAlive(bloom)) bloom->set_enabled(true);
    }
    _cameraDefaults.clear();
  }
  if (IsAlive(_lastMainCameraGO)) {
    auto* mainCam = _lastMainCameraGO->GetComponent<UnityEngine::Camera*>();
    if (IsAlive(mainCam)) {
      if (_mainCamOriginalDepthMode.has_value()) {
        mainCam->set_depthTextureMode(UnityEngine::DepthTextureMode(_mainCamOriginalDepthMode.value()));
      }
      if (_mainCamOriginalClearFlags.has_value()) {
        mainCam->set_clearFlags(UnityEngine::CameraClearFlags(_mainCamOriginalClearFlags.value()));
      }
      if (_mainCamOriginalBackgroundColor.has_value()) {
        mainCam->set_backgroundColor(_mainCamOriginalBackgroundColor.value());
      }
    }
    if (IsAlive(mainCam)) RestoreFarCulling(mainCam);
    if (IsAlive(mainCam) && _addedMainCamLayers != 0) {
      mainCam->set_cullingMask(
          static_cast<int>(static_cast<uint32_t>(mainCam->get_cullingMask()) & ~_addedMainCamLayers));
    }
    ApplyCullingProperty(_lastMainCameraGO, std::nullopt);
    auto* bloomPrePass = _lastMainCameraGO->GetComponent<GlobalNamespace::BloomPrePass*>();
    if (IsAlive(bloomPrePass)) {
      bloomPrePass->set_enabled(true);
    }
  }
  _mainCamOriginalDepthMode.reset();
  _mainCamOriginalClearFlags.reset();
  _mainCamOriginalBackgroundColor.reset();
  _addedMainCamLayers = 0;
  RestoreFieldOfView();
}

// Field Of View % (0.15.12): during a Vivify map the main camera's two eye
// projections are widened by this factor, the way a flat game's FOV slider
// works. Each eye's picture then holds more of the scene, shown smaller, so a
// map's effects that sit at the edge of a narrower headset's view come into
// it. The scene no longer matches head movement exactly, which some players
// feel; 100 % leaves the projection as the headset gives it. Culling is
// widened with it, or what the wider view takes in would not be drawn.
namespace {
UnityEngine::Matrix4x4 ScaledXY(UnityEngine::Matrix4x4 m, float sx, float sy) {
  m.m00 *= sx; m.m01 *= sx; m.m02 *= sx; m.m03 *= sx;
  m.m10 *= sy; m.m11 *= sy; m.m12 *= sy; m.m13 *= sy;
  return m;
}
}  // namespace

void Runtime::ApplyFieldOfView(UnityEngine::Camera* mainCam) {
  int const percent = GetFieldOfViewPercent();
  bool const wanted = percent > 100 && _currentBeatmapData != nullptr && !_isResetting && IsAlive(mainCam);
  if (!wanted) {
    if (_fovCamera != nullptr) RestoreFieldOfView();
    return;
  }
  if (_fovCamera != mainCam || _fovAppliedPercent != percent) {
    if (_fovCamera != nullptr) RestoreFieldOfView();
    _fovCamera = mainCam;
    _fovAppliedPercent = percent;
    _fovFramesChecked = 0;
    _fovReported = false;
    PaperLogger.info("Vivify field of view: {}% for this map", percent);
  } else if (!_fovReported && ++_fovFramesChecked >= 30) {
    // Whether the headset's runtime kept last frame's projection or put its
    // own back: "the setting does nothing" (0.15.15) is one or the other.
    float const now = mainCam->GetStereoProjectionMatrix(UnityEngine::Camera_StereoscopicEye(0)).m00;
    bool const kept = std::abs(now - _fovSetM00[0]) < 1e-4f;
    PaperLogger.info("Vivify field of view: the headset {} the widened projection (set m00 {:.4f}, now {:.4f})",
                     kept ? "kept" : "replaced", _fovSetM00[0], now);
    _fovReported = true;
  }
  // Set every frame from the headset's own projection: the runtime can hand
  // the camera a new one each frame (0.15.16; 0.15.13 set it once).
  mainCam->ResetStereoProjectionMatrices();
  float const scale = 100.0f / static_cast<float>(percent);
  UnityEngine::Matrix4x4 widened[2];
  for (int eye = 0; eye < 2; eye++) {
    auto const which = UnityEngine::Camera_StereoscopicEye(eye);
    widened[eye] = ScaledXY(mainCam->GetStereoProjectionMatrix(which), scale, scale);
    mainCam->SetStereoProjectionMatrix(which, widened[eye]);
    _fovSetM00[eye] = widened[eye].m00;
  }
  _fovLeftProjection = widened[0];
  mainCam->set_cullingMatrix(
      UnityEngine::Matrix4x4::op_Multiply(BothEyesCulling(widened[0], widened[1]), mainCam->get_worldToCameraMatrix()));
}

void Runtime::RestoreFieldOfView() {
  if (IsAlive(_fovCamera)) {
    _fovCamera->ResetStereoProjectionMatrices();
    _fovCamera->ResetCullingMatrix();
  }
  _fovCamera = nullptr;
  _fovAppliedPercent = 0;
}

void Runtime::RestoreFarCulling(UnityEngine::Camera* mainCam) {
  if (_appliedFarCulling != 0 && IsAlive(mainCam) && _mainCamOriginalCullDistances.has_value()) {
    auto const& original = _mainCamOriginalCullDistances.value();
    auto distances = ArrayW<float>(static_cast<il2cpp_array_size_t>(original.size()));
    for (size_t i = 0; i < original.size(); i++) distances[i] = original[i];
    mainCam->set_layerCullDistances(distances);
    mainCam->set_layerCullSpherical(_mainCamOriginalCullSpherical);
  }
  _appliedFarCulling = 0;
  _mainCamOriginalCullDistances.reset();
}

// Far Culling Distance: the camera skips any renderer farther than the chosen
// distance, on every layer (a layer the game already culls nearer keeps its
// own distance). Measured as a sphere around the camera, so turning the head
// does not pop things in and out.
//
// With Far Culling off (0), a Vivify map runs without the game's own per-layer
// cull distances (0.15.17). The game sets some, measured along the view
// direction rather than as a sphere: an object off to one side counts as near
// while the player looks ahead and goes out of range when they turn to it, so
// things vanished as the player looked at them -- 743 Aether's when looking
// right. Only the far clip plane culls then; the game's values come back when
// the level ends.
void Runtime::ApplyFarCulling(UnityEngine::Camera* mainCam) {
  bool const inMap = _currentBeatmapData != nullptr && !_isResetting;
  int const setting = inMap ? GetFarCullingDistance() : 0;
  // -1: the game's layer distances cleared for the map.
  int const wanted = !inMap ? 0 : (setting > 0 ? setting : -1);
  if (wanted == _appliedFarCulling || !IsAlive(mainCam)) return;
  if (wanted == 0) {
    RestoreFarCulling(mainCam);
    return;
  }
  if (!_mainCamOriginalCullDistances.has_value()) {
    std::vector<float> original(32, 0.0f);
    auto current = mainCam->get_layerCullDistances();
    if (current) {
      for (int i = 0; i < current.size() && i < 32; i++) original[i] = current[i];
    }
    _mainCamOriginalCullDistances = original;
    _mainCamOriginalCullSpherical = mainCam->get_layerCullSpherical();
    std::string layers;
    for (int i = 0; i < 32; i++) {
      if (original[i] > 0.0f) layers += fmt::format("{}{}={:.0f}m", layers.empty() ? "" : ", ", i, original[i]);
    }
    PaperLogger.info("Vivify main camera: the game's per-layer cull distances: {} ({})",
                     layers.empty() ? std::string("none") : layers,
                     _mainCamOriginalCullSpherical ? "spherical" : "along the view direction");
  }
  auto const& original = _mainCamOriginalCullDistances.value();
  if (wanted < 0) {
    auto cleared = ArrayW<float>(static_cast<il2cpp_array_size_t>(32));
    for (int i = 0; i < 32; i++) cleared[i] = 0.0f;
    mainCam->set_layerCullDistances(cleared);
    _appliedFarCulling = wanted;
    PaperLogger.info("Vivify main camera: per-layer cull distances off for this map (Far Culling is 0)");
    return;
  }
  auto distances = ArrayW<float>(static_cast<il2cpp_array_size_t>(32));
  for (int i = 0; i < 32; i++) {
    float const own = original[i];
    distances[i] = (own > 0.0f && own < static_cast<float>(wanted)) ? own : static_cast<float>(wanted);
  }
  mainCam->set_layerCullDistances(distances);
  mainCam->set_layerCullSpherical(true);
  _appliedFarCulling = wanted;
  PaperLogger.info("Vivify main camera: far culling at {}m", wanted);
}

void Runtime::RefreshCameraComponents(bool allowCameraApplier) {
  for (auto it = _cameraDefaults.begin(); it != _cameraDefaults.end();) {
    if (!it->second.camera || !IsAlive(it->second.camera.ptr())) it = _cameraDefaults.erase(it);
    else ++it;
  }
  auto mainCam = UnityEngine::Camera::get_main();
  auto* mainCamPtr = mainCam.unsafePtr();
  auto* mainCamGO = IsAlive(mainCamPtr) ? mainCam->get_gameObject().unsafePtr() : nullptr;
  bool const cameraChanged = mainCamGO != _lastMainCameraGO;
  if (cameraChanged) {
    SeparateEye::CancelFrame();
    RestoreMainCameraOriginals(false);
    _lastMainCameraGO = mainCamGO;
    _appliedFarCulling = 0;
    _mainCamOriginalCullDistances.reset();
    _mainCamOriginalDepthMode.reset();
    _mainCamOriginalClearFlags.reset();
    _mainCamOriginalBackgroundColor.reset();
    if (IsAlive(mainCamPtr)) {
      auto* camTransform = mainCamPtr->get_transform().unsafePtr();
      auto pos = IsAlive(camTransform) ? camTransform->get_position() : UnityEngine::Vector3(0.0f, 0.0f, 0.0f);
      auto fwd = IsAlive(camTransform) ? camTransform->get_forward() : UnityEngine::Vector3(0.0f, 0.0f, 0.0f);
      PaperLogger.info("Vivify main camera: name='{}' cullingMask=0x{:08x} stereoMode={} multipassSetting={} pos=({:.2f},{:.2f},{:.2f}) fwd=({:.2f},{:.2f},{:.2f})",
                       IsAlive(mainCamGO) ? ToStdString(mainCamGO->get_name()) : std::string("?"),
                       static_cast<uint32_t>(mainCamPtr->get_cullingMask()),
                       static_cast<int>(UnityEngine::XR::XRSettings::get_stereoRenderingMode().value__),
                       BoolText(GetMultipassRenderingEnabled()),
                       pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z);
    }
  }

  // Layers a map's prefabs use that the PC headset camera draws and this
  // camera does not (NoteHeadsetLayers): added for the level, taken back in
  // RestoreMainCameraOriginals.
  if (IsAlive(mainCamPtr) && _currentBeatmapData != nullptr && !_isResetting && _mapHeadsetLayers != 0) {
    uint32_t const current = static_cast<uint32_t>(mainCamPtr->get_cullingMask());
    uint32_t const missing = _mapHeadsetLayers & ~current;
    if (missing != 0) {
      mainCamPtr->set_cullingMask(static_cast<int>(current | missing));
      _addedMainCamLayers |= missing;
      PaperLogger.info("Vivify main camera: now draws layer(s) 0x{:08x} for this map's headset-only objects",
                       missing);
    }
  }

  ApplyFieldOfView(mainCamPtr);

  if (IsAlive(mainCamPtr)) {
    static int sLastLoggedCullingMask = -2;
    int const mask = mainCamPtr->get_cullingMask();
    if (mask != sLastLoggedCullingMask) {
      sLastLoggedCullingMask = mask;
      PaperLogger.info("Vivify main camera cullingMask is now 0x{:08x}", static_cast<uint32_t>(mask));
    }
  }

  RefreshMultipassRendering(mainCamGO);
  if (IsAlive(mainCamPtr)) {
    if (auto props = _cameraProperties.find(std::string(kMainCameraId)); props != _cameraProperties.end()) {
      CaptureMainCameraOriginals(mainCamPtr, mainCamGO);
      ApplyCameraProperties(mainCamPtr, props->second);
      ApplyCameraGameObjectProperties(mainCamPtr, props->second);
    }
    // Scene depth, the way PC Beat Saber always has it.
    //
    // Raymarchers, black holes, distortion and soft-intersection shaders all
    // sample _CameraDepthTexture to know where the world is. PC Beat Saber's
    // camera renders that texture every frame, so maps never ask for it; the
    // Quest build's camera does not, and a shader sampling it gets Unity's
    // empty stand-in -- every ray "hits" at once and the effect draws nothing.
    // It costs a depth pre-pass over the whole scene, so it is only on while a
    // Vivify map plays, only for maps whose shaders read it
    // (ScanBundleForSceneDepth; Hold My Hand's do not), and it can be turned
    // off in settings. A map that asks for depth itself (SetCameraProperty)
    // still gets it from that.
    auto const mapNeedsDepth = MapShadersNeedSceneDepth();
    if (_currentBeatmapData != nullptr && mapNeedsDepth.has_value() && !mapNeedsDepth.value() &&
        !_sceneDepthSkipLogged) {
      _sceneDepthSkipLogged = true;
      PaperLogger.info("Vivify main camera: no shader in this map reads the scene depth texture; not rendering it");
    }
    if (GetSceneDepthTexture() && mapNeedsDepth.value_or(true) && _currentBeatmapData != nullptr &&
        !_isResetting) {
      int const mode = mainCamPtr->get_depthTextureMode().value__;
      if ((mode & UnityEngine::DepthTextureMode::Depth.value__) == 0) {
        CaptureMainCameraOriginals(mainCamPtr, mainCamGO);
        mainCamPtr->set_depthTextureMode(UnityEngine::DepthTextureMode(mode | UnityEngine::DepthTextureMode::Depth.value__));
        PaperLogger.info("Vivify main camera: scene depth texture enabled for this map");
      }
    }
  }
  _mainCameraPropsDirty = false;
  if (IsAlive(mainCamPtr)) ApplyFarCulling(mainCamPtr);
  RefreshCameraApplier(mainCamGO, allowCameraApplier);
}

void Runtime::RefreshMultipassRendering(UnityEngine::GameObject* mainCamGO) {

  if (IsAlive(mainCamGO) && _currentBeatmapData != nullptr && !_isResetting) {
    _multipassController = EnsureMultipassKeywordController(mainCamGO);
    return;
  }
  if (_multipassController != nullptr && UnityEngine::Object::op_Implicit_bool(_multipassController)) {
    _multipassController->set_enabled(false);
    UnityEngine::Object::Destroy(_multipassController);
  }
  _multipassController = nullptr;
  SetMultipassShaderState(false);
}

MultipassKeywordController* Runtime::EnsureMultipassKeywordController(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return nullptr;
  auto* controller = gameObject->GetComponent<MultipassKeywordController*>();
  if (!IsAlive(controller)) {
    controller = gameObject->AddComponent<MultipassKeywordController*>();
  }
  if (IsAlive(controller)) {
    bool const enabled = GetMultipassRenderingEnabled();
    if (controller->get_enabled() != enabled) {
      controller->set_enabled(enabled);
    }
    if (!enabled) {
      SetMultipassShaderState(false);
    }
  }
  return controller;
}

namespace {
bool MainEffectRenders(GlobalNamespace::MainEffectController* controller) {
  if (!IsManagedAlive(controller) || controller->_mainEffectContainer.ptr() == nullptr ||
      !IsManagedAlive(controller->_mainEffectContainer)) {
    return false;
  }
  auto mainEffectSO = controller->_mainEffectContainer->get_mainEffect();
  return mainEffectSO.ptr() != nullptr && IsManagedAlive(mainEffectSO) && mainEffectSO->get_hasPostProcessEffect();
}
}

void Runtime::RefreshCameraApplier(UnityEngine::GameObject* mainCamGO, bool allowCameraApplier) {
  if (_pauseMenuActive) allowCameraApplier = false;
  if (!allowCameraApplier) {
    if (_cameraApplier != nullptr && UnityEngine::Object::op_Implicit_bool(_cameraApplier) &&
        _cameraApplier->get_enabled()) {
      _cameraApplier->set_enabled(false);
    }
    return;
  }
  if (!IsAlive(mainCamGO)) {
    DestroyCameraApplier();
    return;
  }
  if (_cameraApplier == nullptr || !UnityEngine::Object::op_Implicit_bool(_cameraApplier) ||
      _cameraApplierGO != mainCamGO) {
    DestroyCameraApplier();
    _cameraApplier = mainCamGO->AddComponent<CameraApplier*>();
    _cameraApplier->set_enabled(false);
    _cameraApplierGO = mainCamGO;

    _cameraApplier->mainEffectController = nullptr;
    _cameraApplier->hasMainEffect = false;
  }

  // The game's main effect (bloom) is off on Quest unless a mod turns it on --
  // GraphicsTweaks' bloom setting does. Left to itself it renders in the game's
  // own ImageEffectController before the CameraApplier, so every Blit, the
  // "before main effect" ones too, landed on an already-bloomed image. When it
  // is on, the CameraApplier renders it between the map's pre- and
  // post-effects as on PC, and ShouldBypassImageEffect skips the game's pass so
  // it is not drawn twice. With it off nothing changes (0.14.27).
  auto* mainEffect = mainCamGO->GetComponent<GlobalNamespace::MainEffectController*>();
  bool const mainEffectActive = MainEffectRenders(mainEffect);
  _cameraApplier->mainEffectController = mainEffectActive ? mainEffect : nullptr;
  if (_cameraApplier->hasMainEffect != mainEffectActive) {
    _cameraApplier->hasMainEffect = mainEffectActive;
    PaperLogger.info("Vivify main camera: game main effect (bloom) {} the map's effects",
                     mainEffectActive ? "is on; rendering it between" : "is off; not rendering it with");
  }

  bool const needsBlit = !GetDisableAllBlits() &&
                         (!_preEffects.empty() || !_postEffects.empty() || HasMidRenderEffects() ||
                          !_secondaryCameras.empty());
  bool const needsCamera = needsBlit || GetExperimentalSeparateEye();
  if (_cameraApplier->get_enabled() != needsCamera) _cameraApplier->set_enabled(needsCamera);
}

void Runtime::DestroyCameraApplier() {
  RemoveMidRenderCommandBuffers();
  if (_cameraApplier != nullptr && UnityEngine::Object::op_Implicit_bool(_cameraApplier)) {
    _cameraApplier->set_enabled(false);
    UnityEngine::Object::Destroy(_cameraApplier);
  }
  _cameraApplier = nullptr;
  _cameraApplierGO = nullptr;
}

bool Runtime::ShouldBypassImageEffect(GlobalNamespace::ImageEffectController* controller) {
  if (_cameraApplier == nullptr || !UnityEngine::Object::op_Implicit_bool(_cameraApplier) ||
      !_cameraApplier->get_enabled() || !_cameraApplier->hasMainEffect) {
    return false;
  }
  if (!IsManagedAlive(controller)) return false;
  auto* mainEffect = _cameraApplier->mainEffectController;
  if (IsManagedAlive(mainEffect) && mainEffect->_imageEffectController.unsafePtr() == controller) return true;
  return controller->get_gameObject().unsafePtr() == _cameraApplierGO;
}

}
