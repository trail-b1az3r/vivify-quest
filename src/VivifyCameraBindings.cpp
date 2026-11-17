#include "VivifyCameraBindings.hpp"
#include "GlobalNamespace/BloomPrePass.hpp"
#include "GlobalNamespace/BloomPrePassRendererSO.hpp"
#include "GlobalNamespace/ToneMappingExtensions.hpp"
#include <stdexcept>

namespace Vivify {
namespace {
constexpr std::array<char const*, 5> textures = {"_CameraDepthTexture", "_CameraDepthNormalsTexture", "_CameraOpaqueTexture",
                                               "_CameraMotionVectorsTexture", "_LastCameraDepthTexture"};
constexpr std::array<char const*, 2> depthParams = {"_ZBufferParams", "_ProjectionParams"};
template <class T> bool Alive(T* p) { return IsManagedAlive(p); }
}
void CameraBindings::Capture(UnityEngine::Camera* camera) {
  _active = UnityEngine::RenderTexture::get_active().unsafePtr(); _activeCaptured = true;
  _screenParams = UnityEngine::Shader::GetGlobalVector(u"_ScreenParams"); _screenParamsCaptured = true;
  // A saved depth image must keep the parameters of the camera that made it.
  for (size_t i = 0; i != depthParams.size(); ++i) {
    _depthParams[i] = UnityEngine::Shader::GetGlobalVector(StringW(std::string(depthParams[i]))); ++_depthParamsCaptured;
  }
  for (size_t i = 0; i != textures.size(); ++i) {
    _textures[i] = UnityEngine::Shader::GetGlobalTexture(StringW(std::string(textures[i]))).unsafePtr(); ++_captured;
  }
  using Bloom = GlobalNamespace::BloomPrePassRendererSO;
  static bool const initialized = [] {
    il2cpp_functions::runtime_class_init(classof(GlobalNamespace::BloomPrePassRendererSO*));
    il2cpp_functions::runtime_class_init(classof(GlobalNamespace::ToneMappingExtensions*));
    return true;
  }();
  (void)initialized;
  _bloomTextureId = Bloom::getStaticF__bloomPrePassTextureID();
  _ratioId = Bloom::getStaticF__customFogTextureToScreenRatioID();
  _eyeId = Bloom::getStaticF__stereoCameraEyeOffsetID();
  _transformId = Bloom::getStaticF__vertexTransformMatrixID();
  _bloomTexture = UnityEngine::Shader::GetGlobalTexture(_bloomTextureId).unsafePtr();
  _ratio = UnityEngine::Shader::GetGlobalVector(_ratioId);
  _eye = UnityEngine::Shader::GetGlobalFloat(_eyeId);
  _transform = UnityEngine::Shader::GetGlobalMatrix(_transformId); _bloomCaptured = true;
  auto* bloom = Alive(camera) ? camera->GetComponent<GlobalNamespace::BloomPrePass*>() : nullptr;
  if (!Alive(bloom)) {
    auto* main = UnityEngine::Camera::get_main().unsafePtr();
    if (Alive(main)) bloom = main->GetComponent<GlobalNamespace::BloomPrePass*>();
  }
  if (Alive(bloom) && Alive(bloom->____bloomPrePassRenderData.unsafePtr())) {
    _bloomSource = bloom->____bloomPrePassRenderData.unsafePtr(); _bloomData = _bloomSource->___data;
  }
  auto names = GlobalNamespace::ToneMappingExtensions::getStaticF__shaderKeywordMap();
  if (names) {
    if (names.size() > _keywordStates.size()) throw std::runtime_error("unexpected tone-mapping keyword layout");
    _keywords = static_cast<Array<StringW>*>(names);
    for (auto name : names) {
      _keywordStates[_keywordCount] = name && name->get_Length() > 0 && UnityEngine::Shader::IsKeywordEnabled(name);
      ++_keywordCount;
    }
  }
}
bool CameraBindings::Restore() noexcept {
  if (_restored) return true;
  bool ok = true;
  // A failed texture/keyword must not prevent the remaining bindings from
  // being restored. Keep failed snapshots retryable at destruction as well.
  auto attempt = [&](auto action) noexcept { try { action(); } catch (...) { ok = false; } };
  if (_activeCaptured) attempt([&] { UnityEngine::RenderTexture::set_active(_active && Alive(_active.ptr()) ? _active.ptr() : nullptr); });
  if (_screenParamsCaptured) attempt([&] { UnityEngine::Shader::SetGlobalVector(u"_ScreenParams", _screenParams); });
  for (int i = 0; i != _depthParamsCaptured; ++i)
    attempt([&] { UnityEngine::Shader::SetGlobalVector(StringW(std::string(depthParams[i])), _depthParams[i]); });
  for (int i = 0; i != _captured; ++i)
    attempt([&] { UnityEngine::Shader::SetGlobalTexture(StringW(std::string(textures[i])), _textures[i] && Alive(_textures[i].ptr()) ? _textures[i].ptr() : nullptr); });
  if (_bloomCaptured) {
    attempt([&] { UnityEngine::Shader::SetGlobalTexture(_bloomTextureId, _bloomTexture && Alive(_bloomTexture.ptr()) ? _bloomTexture.ptr() : nullptr); });
    attempt([&] { UnityEngine::Shader::SetGlobalVector(_ratioId, _ratio); });
    attempt([&] { UnityEngine::Shader::SetGlobalFloat(_eyeId, _eye); });
    attempt([&] { UnityEngine::Shader::SetGlobalMatrix(_transformId, _transform); });
  }
  attempt([&] { if (_bloomSource && Alive(_bloomSource.ptr())) _bloomSource->___data = _bloomData.ptr(); });
  if (_keywords) for (int i = 0; i != _keywordCount; ++i)
    attempt([&] {
      auto name = ArrayW<StringW>(_keywords.ptr())[i];
      if (!name || name->get_Length() <= 0) return;
      if (_keywordStates[i]) UnityEngine::Shader::EnableKeyword(name); else UnityEngine::Shader::DisableKeyword(name);
    });
  _restored = ok;
  if (!ok) PaperLogger.error("Vivify: restoring the enclosing camera's depth/bloom bindings failed");
  return ok;
}
}
