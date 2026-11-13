#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace UnityEngine { class Camera; class RenderTexture; class Material; class GameObject; class Shader; class Object; }
namespace GlobalNamespace { class MirrorRendererSO; }
namespace Vivify { class CameraApplier; }
namespace Vivify::SeparateEye {
// Conversion is queued in the menu. Unity objects are loaded/polled only on
// the main thread. All entry points except the conversion worker are main-thread.
void Prepare(std::string const& level, std::string const& pcBundle, std::string const& cacheBase);
void Poll();
// Read-only preparation diagnostics; does not start or draw a level.
std::string PreparationState();
int PreparedDonorCount();
// Scene-eye scope only; mono cameras and screen-effect donors are separate.
int UntranslatedTessellationShaderCount();
int ActiveSceneEyeTessellationShaderCount();
int SourceTessellationShaderCount();
std::vector<UnityEngine::Shader*> PreparedShadersForDiagnostics();
void RememberMaterial(UnityEngine::Material* material);
// Match donor shaders by preserved bundle asset identity, including material
// references. Different shaders may legitimately share the same display name.
void RememberAsset(UnityEngine::Object* asset, std::string const& bundlePath);
void RememberFallback(UnityEngine::Material* material);
void RememberRenderers(UnityEngine::GameObject* root);
void NoteTextureCameras();
inline bool IsDonorShaderName(std::string_view name) {
  return name.starts_with("__VivifyEye") || name.starts_with("__VivifyMono__/") || name.starts_with("__VivifyBlitEye") ||
         name.starts_with("__VivifyNativePerspective__/") || name.starts_with("__VivifyNativeOther__/");
}
// Begin before culling/depth generation; restore after the authored camera.
bool BeginTextureCamera(void const* owner, UnityEngine::Camera* camera);
void EndTextureCamera(void const* owner);
UnityEngine::Material* BlitMaterial(UnityEngine::Material* source);
bool IsRenderingEye();
// Process slices of an existing multiview image without rendering a scene.
bool BeginBlitEye(UnityEngine::Camera* camera, int eye);
void EndBlitEye();
bool CanUseNativeMaterial(UnityEngine::Material* source);
void RenderEyeImage(UnityEngine::Camera* camera, UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest);
// Frame resources are per play session. Donors can survive a song restart.
void Reset(bool unloadDonors = false);
// Render before the main camera culls. Suppress its redundant scene draw only
// AFTER both complete eye images exist. First frame learns the real descriptor.
void PrepareFrame(UnityEngine::Camera* main, CameraApplier* effects);
void RestoreMainCamera(UnityEngine::Camera* main = nullptr);
void CancelFrame();
void RejectFixedMaterialFeatures();
void RejectCameraBindings();
// PC Vivify keys mirror results by camera identity. Use an isolated native
// cache for each camera, without changing any camera's real field of view.
GlobalNamespace::MirrorRendererSO* MirrorForCurrentCamera(GlobalNamespace::MirrorRendererSO* source);
bool IsPrivateMirror(GlobalNamespace::MirrorRendererSO* renderer);
std::vector<GlobalNamespace::MirrorRendererSO*> MirrorCopies(GlobalNamespace::MirrorRendererSO* source);
void ForgetMirrorCopies(GlobalNamespace::MirrorRendererSO* source);
// Consumes only a completed image from this camera and this frame. No scenes
// are rendered here. On pre-render rejection/failure the main view renders normally.
bool TryRender(UnityEngine::Camera* main, UnityEngine::RenderTexture* src, CameraApplier* effects);
}
