#pragma once
#include "VivifyTypes.hpp"
#include "GlobalNamespace/BloomPrePassRenderDataSO.hpp"
#include <array>

namespace Vivify {
// A complete nested camera render may replace scene-depth and bloom globals.
// Keep the enclosing camera's bindings until it resumes drawing.
class CameraBindings {
 public:
  CameraBindings() = default;
  CameraBindings(CameraBindings const&) = delete;
  CameraBindings& operator=(CameraBindings const&) = delete;
  ~CameraBindings() { Restore(); }
  void Capture(UnityEngine::Camera* camera);
  bool Restore() noexcept;
 private:
  SafePtr<UnityEngine::RenderTexture> _active;
  std::array<SafePtr<UnityEngine::Texture>, 5> _textures;
  int _captured = 0;
  bool _activeCaptured = false, _bloomCaptured = false, _restored = false;
  SafePtr<UnityEngine::Texture> _bloomTexture;
  int _bloomTextureId = 0, _ratioId = 0, _eyeId = 0, _transformId = 0;
  UnityEngine::Vector4 _ratio;
  UnityEngine::Vector4 _screenParams;
  bool _screenParamsCaptured = false;
  std::array<UnityEngine::Vector4, 2> _depthParams;
  int _depthParamsCaptured = 0;
  float _eye = 0;
  UnityEngine::Matrix4x4 _transform;
  SafePtr<GlobalNamespace::BloomPrePassRenderDataSO> _bloomSource;
  SafePtr<GlobalNamespace::BloomPrePassRenderDataSO_Data> _bloomData;
  SafePtr<Array<StringW>> _keywords;
  std::array<bool, 16> _keywordStates{};
  int _keywordCount = 0;
};
}
