#include "VivifyMirrorProbe.hpp"

#include "VivifyRuntimeInternal.hpp"

#ifndef VIVIFY_GAME_1_45_3
#include <optional>
#include <string>

#include "GlobalNamespace/Mirror.hpp"
#include "GlobalNamespace/MirrorRendererSO.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/FilterMode.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Graphics.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/RenderTexture.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"
#include "UnityEngine/RenderTextureFormat.hpp"
#include "UnityEngine/RenderTextureMemoryless.hpp"
#include "UnityEngine/Rendering/CommandBuffer.hpp"
#include "UnityEngine/Rendering/RenderTargetIdentifier.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Texture.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/TextureWrapMode.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/VRTextureUsage.hpp"
#include "UnityEngine/Vector2.hpp"
#endif

namespace Vivify::MirrorProbe {
namespace {
std::optional<MirrorLayout::StereoRemedy> gStereo;
std::optional<MirrorLayout::MonoRemedy> gMono;
}  // namespace

MirrorLayout::StereoRemedy const* Stereo() { return gStereo && gStereo->known ? &*gStereo : nullptr; }
MirrorLayout::MonoRemedy const* Mono() { return gMono && gMono->known ? &*gMono : nullptr; }

#ifdef VIVIFY_GAME_1_45_3
void NoteMirrorDrawn() {}
bool Wanted() { return false; }
void Run(UnityEngine::RenderTexture*) {}
void FlipVertically(UnityEngine::Texture*) {}
#else
namespace {
constexpr int kSize = 16;
bool gDone = false;
int gTries = 0;
float gLastTry = -100.0f;
bool gMirrorDrawn = false;

UnityEngine::Rendering::RenderTargetIdentifier TargetId(UnityEngine::Texture* texture, bool layered) {
  auto id = UnityEngine::Rendering::RenderTargetIdentifier::op_Implicit___UnityEngine__Rendering__RenderTargetIdentifier(
      texture);
  if (layered) id.m_DepthSlice = -1;
  return id;
}

void Draw(UnityEngine::Rendering::CommandBuffer* cb, UnityEngine::Texture* source, int sourceSlice,
          UnityEngine::Texture* dest, bool destLayered, int destSlice, UnityEngine::Material* material) {
  auto from = TargetId(source, false);
  auto to = TargetId(dest, destLayered);
  cb->Blit_Identifier(byref(from), byref(to), material, material ? 0 : -1, UnityEngine::Vector2(1.0f, 1.0f),
                      UnityEngine::Vector2(0.0f, 0.0f), sourceSlice, destSlice);
}

// The four quarter centres of a 16 x 16 read-back, [row][col], row 0 bottom.
bool ReadQuarters(UnityEngine::RenderTexture* texture, UnityEngine::Texture2D* reader,
                  MirrorLayout::Texel (&out)[2][2], std::string& raw) {
  auto previous = UnityEngine::RenderTexture::get_active();
  UnityEngine::RenderTexture::set_active(texture);
  reader->ReadPixels(UnityEngine::Rect(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)), 0, 0,
                     false);
  UnityEngine::RenderTexture::set_active(previous);
  auto pixels = reader->GetPixels32();
  if (!pixels || pixels.size() < kSize * kSize) return false;
  for (int row = 0; row < 2; row++) {
    for (int col = 0; col < 2; col++) {
      int sum[3] = {0, 0, 0};
      int const cx = col == 0 ? 3 : 11, cy = row == 0 ? 3 : 11;
      for (int y = cy; y < cy + 2; y++) {
        for (int x = cx; x < cx + 2; x++) {
          auto const& p = pixels[y * kSize + x];
          sum[0] += p.r;
          sum[1] += p.g;
          sum[2] += p.b;
        }
      }
      out[row][col] = MirrorLayout::Classify(sum[0] / 4, sum[1] / 4, sum[2] / 4);
      raw += fmt::format("{}({},{},{})", raw.empty() || raw.back() == '[' ? "" : " ", sum[0] / 4, sum[1] / 4,
                         sum[2] / 4);
    }
  }
  return true;
}

// The material property the mirror's reflection texture is bound to: the one
// holding a texture the size of the mirror's own.
std::optional<std::string> ReflectionProperty(UnityEngine::Material* material,
                                              GlobalNamespace::MirrorRendererSO* renderer) {
  auto names = material->GetTexturePropertyNames();
  if (!names) return std::nullopt;
  for (int i = 0; i < names.size(); i++) {
    auto texture = material->GetTexture(names[i]);
    auto* rt = IsManagedAlive(texture.unsafePtr())
                   ? il2cpp_utils::try_cast<UnityEngine::RenderTexture>(texture.unsafePtr()).value_or(nullptr)
                   : nullptr;
    if (!rt) continue;
    int const w = rt->get_width(), h = rt->get_height();
    if ((w == renderer->____stereoTextureWidth && h == renderer->____stereoTextureHeight) ||
        (w == renderer->____monoTextureWidth && h == renderer->____monoTextureHeight))
      return static_cast<std::string>(names[i]);
  }
  return std::nullopt;
}

std::string Describe(MirrorLayout::StereoRemedy const& remedy) {
  if (!remedy.known) return "no known layout";
  std::string text = MirrorLayout::ModeName(remedy.mode);
  if (remedy.mode == MirrorLayout::StereoMode::OverUnder)
    text += fmt::format(" (left eye in the {} half)", remedy.row[0] == 0 ? "lower" : "upper");
  if (remedy.flipV) text += ", upside down";
  return text;
}
}  // namespace

void NoteMirrorDrawn() { gMirrorDrawn = true; }
bool Wanted() { return gMirrorDrawn && !gDone; }

void Run(UnityEngine::RenderTexture* eyeTexture) {
  if (gDone || !IsManagedAlive(eyeTexture)) return;
  float const now = UnityEngine::Time::get_unscaledTime();
  if (now - gLastTry < 1.0f) return;
  gLastTry = now;

  // A drawing game mirror, its material and the property its reflection is in.
  GlobalNamespace::Mirror* mirror = nullptr;
  UnityEngine::Material* material = nullptr;
  std::string property;
  bool anyMirror = false;
  auto mirrors = UnityEngine::Object::FindObjectsOfType<GlobalNamespace::Mirror*>();
  for (int i = 0; mirrors && i < mirrors.size(); i++) {
    auto* candidate = mirrors[i];
    if (!IsManagedAlive(candidate) || !candidate->get_isEnabled()) continue;
    auto go = candidate->get_gameObject();
    if (!IsManagedAlive(go.unsafePtr()) || !go->get_activeInHierarchy()) continue;
    auto* mat = candidate->____mirrorMaterial.unsafePtr();
    auto* renderer = candidate->____mirrorRenderer.unsafePtr();
    if (!IsManagedAlive(mat) || !IsManagedAlive(renderer)) continue;
    anyMirror = true;
    auto found = ReflectionProperty(mat, renderer);
    if (!found) continue;
    mirror = candidate;
    material = mat;
    property = *found;
    break;
  }
  if (!mirror) {
    // A mirror that is on but never holds a reflection: the game is not
    // drawing mirrors (mirror quality off). Stop looking after a minute of it.
    if (anyMirror && ++gTries >= 60) {
      gDone = true;
      PaperLogger.info("Vivify mirror test: the game's mirrors are on but hold no reflection (mirrors off in the "
                       "game's quality settings?); not tested");
    }
    return;
  }
  gDone = true;

  auto desc = eyeTexture->get_descriptor();
  bool const layered = desc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray &&
                       desc.get_volumeDepth() == 2;
  bool const stereoOn = UnityEngine::Shader::IsKeywordEnabled(StringW("STEREO_MULTIVIEW_ON"));
  std::string const shaderName = IsManagedAlive(material->get_shader().unsafePtr())
                                     ? static_cast<std::string>(material->get_shader()->get_name())
                                     : std::string("?");

  UnityEngine::Texture2D* pattern = nullptr;
  UnityEngine::Texture2D* reader = nullptr;
  UnityEngine::Material* probe = nullptr;
  UnityEngine::Mesh* quad = nullptr;
  UnityEngine::Rendering::CommandBuffer* cb = nullptr;
  std::vector<UnityEngine::RenderTexture*> temporaries;
  bool keywordOff = false;
  std::string rawStereo, rawMono;
  try {
    // Bottom-left red, bottom-right green, top-left blue, top-right yellow.
    pattern = UnityEngine::Texture2D::New_ctor(2, 2, UnityEngine::TextureFormat::RGBA32, false);
    pattern->set_filterMode(UnityEngine::FilterMode::Point);
    pattern->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);
    pattern->SetPixel(0, 0, UnityEngine::Color(1.0f, 0.0f, 0.0f, 1.0f));
    pattern->SetPixel(1, 0, UnityEngine::Color(0.0f, 1.0f, 0.0f, 1.0f));
    pattern->SetPixel(0, 1, UnityEngine::Color(0.0f, 0.0f, 1.0f, 1.0f));
    pattern->SetPixel(1, 1, UnityEngine::Color(1.0f, 1.0f, 0.0f, 1.0f));
    pattern->Apply(false);
    reader = UnityEngine::Texture2D::New_ctor(kSize, kSize, UnityEngine::TextureFormat::RGBA32, false);
    probe = UnityEngine::Material::New_ctor(material);
    // Both windings, so the quad shows whatever the mirror material culls.
    quad = UnityEngine::Mesh::New_ctor();
    {
      auto vertices = ArrayW<UnityEngine::Vector3>(static_cast<il2cpp_array_size_t>(4));
      vertices[0] = UnityEngine::Vector3(-0.5f, -0.5f, 0.0f);
      vertices[1] = UnityEngine::Vector3(0.5f, -0.5f, 0.0f);
      vertices[2] = UnityEngine::Vector3(-0.5f, 0.5f, 0.0f);
      vertices[3] = UnityEngine::Vector3(0.5f, 0.5f, 0.0f);
      int32_t const order[12] = {0, 2, 1, 1, 2, 3, 0, 1, 2, 1, 3, 2};
      auto triangles = ArrayW<int32_t>(static_cast<il2cpp_array_size_t>(12));
      for (int i = 0; i < 12; i++) triangles[i] = order[i];
      quad->set_vertices(vertices);
      quad->set_triangles(triangles);
    }
    probe->SetTexture(StringW(property), pattern);
    cb = UnityEngine::Rendering::CommandBuffer::New_ctor();

    auto flat = desc;
    flat.set_width(kSize);
    flat.set_height(kSize);
    flat.set_dimension(UnityEngine::Rendering::TextureDimension::Tex2D);
    flat.set_volumeDepth(1);
    flat.set_vrUsage(UnityEngine::VRTextureUsage::None);
    flat.set_msaaSamples(1);
    flat.set_depthBufferBits(0);
    flat.set_colorFormat(UnityEngine::RenderTextureFormat::ARGB32);
    flat.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
    auto temporary = [&](UnityEngine::RenderTextureDescriptor d) -> UnityEngine::RenderTexture* {
      auto* t = UnityEngine::RenderTexture::GetTemporary(d).unsafePtr();
      if (IsManagedAlive(t)) temporaries.push_back(t);
      return IsManagedAlive(t) ? t : nullptr;
    };

    // The two-eye program, drawn into both layers of a small eye-like array.
    std::optional<MirrorLayout::StereoRemedy> stereo;
    if (layered && stereoOn) {
      auto array = desc;
      array.set_width(kSize);
      array.set_height(kSize);
      array.set_msaaSamples(1);
      array.set_depthBufferBits(0);
      array.set_colorFormat(UnityEngine::RenderTextureFormat::ARGB32);
      array.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
      auto* target = temporary(array);
      auto* eye0 = temporary(flat);
      auto* eye1 = temporary(flat);
      if (target && eye0 && eye1) {
        Draw(cb, UnityEngine::Texture2D::get_blackTexture().unsafePtr(), -1, target, true, -1, nullptr);
        // The two-eye program places its quad with the eyes' own view and
        // projection (UnityStereoGlobals), not a blit's flat one, so a blit
        // put the test picture anywhere (0.15.55 log: mixed colours). A large
        // quad two metres in front of the headset camera fills both eyes'
        // views instead, as a mirror would (0.16.0).
        auto* camera = UnityEngine::Camera::get_current().unsafePtr();
        if (IsManagedAlive(camera) && IsManagedAlive(quad)) {
          auto transform = camera->get_transform();
          auto const forward = transform->get_forward();
          auto const position = transform->get_position();
          auto const centre = UnityEngine::Vector3(position.x + forward.x * 2.0f, position.y + forward.y * 2.0f,
                                                   position.z + forward.z * 2.0f);
          auto const matrix = UnityEngine::Matrix4x4::TRS(centre, transform->get_rotation(),
                                                          UnityEngine::Vector3(40.0f, 40.0f, 1.0f));
          cb->SetRenderTarget(TargetId(target, true));
          cb->DrawMesh(quad, matrix, probe, 0, 0);
        }
        Draw(cb, target, 0, eye0, false, -1, nullptr);
        Draw(cb, target, 1, eye1, false, -1, nullptr);
        UnityEngine::Graphics::ExecuteCommandBuffer(cb);
        cb->Clear();
        MirrorLayout::Texel samples[2][2][2];
        rawStereo = "left [";
        bool ok = ReadQuarters(eye0, reader, samples[0], rawStereo);
        rawStereo += "] right [";
        ok = ok && ReadQuarters(eye1, reader, samples[1], rawStereo);
        rawStereo += "]";
        if (ok) stereo = MirrorLayout::Stereo(samples);
      }
    }

    // The one-eye program (the Separate-Eye Renderer's eye cameras).
    std::optional<MirrorLayout::MonoRemedy> mono;
    if (auto* single = temporary(flat)) {
      if (stereoOn) {
        UnityEngine::Shader::DisableKeyword(StringW("STEREO_MULTIVIEW_ON"));
        keywordOff = true;
      }
      Draw(cb, UnityEngine::Texture2D::get_blackTexture().unsafePtr(), -1, single, false, -1, nullptr);
      Draw(cb, pattern, -1, single, false, -1, probe);
      UnityEngine::Graphics::ExecuteCommandBuffer(cb);
      cb->Clear();
      if (keywordOff) {
        UnityEngine::Shader::EnableKeyword(StringW("STEREO_MULTIVIEW_ON"));
        keywordOff = false;
      }
      MirrorLayout::Texel samples[2][2];
      rawMono = "[";
      if (ReadQuarters(single, reader, samples, rawMono)) mono = MirrorLayout::Mono(samples);
      rawMono += "]";
    }

    gStereo = stereo;
    gMono = mono;
    PaperLogger.info("Vivify mirror test: '{}' ('{}', reflection in {}): the two-eye program reads {}{}; the one-eye "
                     "program reads {}{}. Read back: two-eye {}; one-eye {}",
                     ToStdString(mirror->get_gameObject()->get_name()), shaderName, property,
                     stereo ? Describe(*stereo)
                            : std::string(!layered ? "(not tested: the eye image is not two layers)"
                                                   : (!stereoOn ? "(not tested: the two-eye keyword is off)"
                                                                : "(no reading)")),
                     Stereo() && (Stereo()->mode != MirrorLayout::StereoMode::AsGame || Stereo()->flipV)
                         ? "; Vivify now draws the reflection that way"
                         : "",
                     mono ? (mono->known ? std::string(mono->flipV ? "it upside down" : "it upright") +
                                               (mono->half < 0 ? std::string(" across the whole width")
                                                               : std::string(mono->half == 0 ? ", only its left half"
                                                                                             : ", only its right half"))
                                         : std::string("no known layout"))
                          : std::string("(no reading)"),
                     Mono() && (Mono()->flipV || Mono()->half >= 0)
                         ? "; Vivify now draws one-eye reflections that way"
                         : "",
                     rawStereo.empty() ? std::string("-") : rawStereo, rawMono.empty() ? std::string("-") : rawMono);
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify mirror test failed ({}); mirrors are drawn as the game draws them", ex.what());
  } catch (...) {
    PaperLogger.warn("Vivify mirror test failed; mirrors are drawn as the game draws them");
  }
  if (keywordOff) {
    try { UnityEngine::Shader::EnableKeyword(StringW("STEREO_MULTIVIEW_ON")); } catch (...) {}
  }
  for (auto* t : temporaries) {
    try { if (IsManagedAlive(t)) UnityEngine::RenderTexture::ReleaseTemporary(t); } catch (...) {}
  }
  try {
    if (cb) cb->Dispose();
    if (IsManagedAlive(probe)) UnityEngine::Object::Destroy(probe);
    if (IsManagedAlive(quad)) UnityEngine::Object::Destroy(quad);
    if (IsManagedAlive(pattern)) UnityEngine::Object::Destroy(pattern);
    if (IsManagedAlive(reader)) UnityEngine::Object::Destroy(reader);
  } catch (...) {}
}

void FlipVertically(UnityEngine::Texture* texture) {
  auto* rt = IsManagedAlive(texture)
                 ? il2cpp_utils::try_cast<UnityEngine::RenderTexture>(texture).value_or(nullptr)
                 : nullptr;
  if (!rt) return;
  auto desc = rt->get_descriptor();
  desc.set_msaaSamples(1);
  desc.set_depthBufferBits(0);
  desc.set_memoryless(UnityEngine::RenderTextureMemoryless::None);
  auto* temp = UnityEngine::RenderTexture::GetTemporary(desc).unsafePtr();
  if (!IsManagedAlive(temp)) return;
  auto previous = UnityEngine::RenderTexture::get_active();
  // Plain one-texture copies: the two-eye keyword (on while the headset
  // camera draws) would pick the copy shader's multiview program.
  bool const stereoOn = UnityEngine::Shader::IsKeywordEnabled(StringW("STEREO_MULTIVIEW_ON"));
  if (stereoOn) UnityEngine::Shader::DisableKeyword(StringW("STEREO_MULTIVIEW_ON"));
  try {
    UnityEngine::Graphics::Blit(rt, temp, UnityEngine::Vector2(1.0f, -1.0f), UnityEngine::Vector2(0.0f, 1.0f));
    UnityEngine::Graphics::Blit(temp, rt);
  } catch (...) {
    if (stereoOn) UnityEngine::Shader::EnableKeyword(StringW("STEREO_MULTIVIEW_ON"));
    UnityEngine::RenderTexture::set_active(previous);
    UnityEngine::RenderTexture::ReleaseTemporary(temp);
    throw;
  }
  if (stereoOn) UnityEngine::Shader::EnableKeyword(StringW("STEREO_MULTIVIEW_ON"));
  UnityEngine::RenderTexture::set_active(previous);
  UnityEngine::RenderTexture::ReleaseTemporary(temp);
}
#endif

}  // namespace Vivify::MirrorProbe
