// Estimated look for materials whose shader cannot run (0.15.27). See
// include/VivifyLookEstimate.hpp.
#include "VivifyLookEstimate.hpp"

#include <algorithm>
#include <cctype>

namespace Vivify::LookEstimate {
namespace {

// UnityEngine.Rendering.BlendMode.
constexpr int kZero = 0, kOne = 1, kDstColor = 2, kSrcColor = 3, kSrcAlpha = 5, kOneMinusSrcColor = 6,
              kOneMinusSrcAlpha = 10;

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool HasWord(std::string const& lower, std::initializer_list<std::string_view> words, std::string* which) {
  for (auto word : words) {
    if (lower.find(word) != std::string::npos) {
      if (which) *which = std::string(word);
      return true;
    }
  }
  return false;
}

bool HasKeyword(Inputs const& in, std::string_view keyword) {
  return std::any_of(in.keywords.begin(), in.keywords.end(), [&](std::string const& k) { return Lower(k) == Lower(keyword); });
}

}  // namespace

Estimate Classify(Inputs const& in) {
  // 1. Blend factors are the material saying how it is drawn; nothing beats them.
  if (in.srcBlend && in.dstBlend) {
    int const src = *in.srcBlend, dst = *in.dstBlend;
    std::string const pair = "blend " + std::to_string(src) + "/" + std::to_string(dst);
    if (dst == kOne || (src == kOne && dst == kOneMinusSrcColor)) return {Look::Additive, pair + " (additive)"};
    if (dst == kOneMinusSrcAlpha) return {Look::Transparent, pair + " (alpha blended)"};
    if (src == kDstColor || dst == kSrcColor) return {Look::Transparent, pair + " (multiply, drawn alpha blended)"};
    if (src == kOne && dst == kZero) {
      // Opaque blending; a cutout keyword or queue still makes it alpha-tested.
      if (HasKeyword(in, "_ALPHATEST_ON") || (in.renderQueue >= 2450 && in.renderQueue < 2500))
        return {Look::Cutout, pair + " with alpha test"};
      return {Look::Opaque, pair + " (opaque)"};
    }
    if (src == kSrcAlpha && dst == kZero) return {Look::Opaque, pair + " (opaque)"};
  }

  // 2. Unity's own keywords for Standard / particle materials.
  if (HasKeyword(in, "_ALPHAPREMULTIPLY_ON") || HasKeyword(in, "_ALPHABLEND_ON"))
    return {Look::Transparent, "keyword alpha blend"};
  if (HasKeyword(in, "_ADDITIVE_ON") || HasKeyword(in, "_BLENDMODE_ADD")) return {Look::Additive, "keyword additive"};
  if (HasKeyword(in, "_ALPHATEST_ON")) return {Look::Cutout, "keyword alpha test"};

  // 3. The shader's name: authors name their shaders after what they do.
  std::string const name = Lower(in.shaderName);
  std::string word;
  if (HasWord(name, {"additive", "/add", " add", "glow", "bloom", "laser", "beam", "spark", "flare", "neon", "light ray",
                     "lightray", "godray", "fire", "flame", "energy", "hologram", "holo"},
              &word))
    return {Look::Additive, "name '" + word + "'"};
  if (HasWord(name, {"transparent", "glass", "alpha", "fade", "ghost", "smoke", "fog", "cloud", "water"}, &word))
    return {Look::Transparent, "name '" + word + "'"};
  if (HasWord(name, {"cutout", "leaf", "leaves", "foliage", "grass", "fur"}, &word))
    return {Look::Cutout, "name '" + word + "'"};

  // 4. The RenderType tag and the queue.
  std::string const tag = Lower(in.renderType);
  if (tag == "transparent") return {in.hasEmission ? Look::Additive : Look::Transparent, "RenderType Transparent"};
  if (tag == "transparentcutout") return {Look::Cutout, "RenderType TransparentCutout"};
  if (in.renderQueue >= 3000) {
    // A particle in the transparent queue is almost always additive glow.
    if (name.find("particle") != std::string::npos || in.hasEmission)
      return {Look::Additive, "queue " + std::to_string(in.renderQueue) + " with glow"};
    return {Look::Transparent, "queue " + std::to_string(in.renderQueue)};
  }
  if (in.renderQueue >= 2450) return {Look::Cutout, "queue " + std::to_string(in.renderQueue)};
  if (in.zWrite && *in.zWrite == 0.0f) return {Look::Transparent, "no depth write"};
  if (name.find("particle") != std::string::npos) return {Look::Additive, "name 'particle'"};
  return {Look::Opaque, "no blend, keyword, name or queue hint"};
}

char const* LookName(Look look) {
  switch (look) {
    case Look::Opaque: return "opaque";
    case Look::Cutout: return "alpha-cut";
    case Look::Transparent: return "alpha-blended";
    case Look::Additive: return "additive glow";
  }
  return "opaque";
}

std::vector<std::string_view> Candidates(Look look) {
  using namespace std::string_view_literals;
  switch (look) {
    case Look::Additive:
      return {"Legacy Shaders/Particles/Additive"sv, "Mobile/Particles/Additive"sv, "Particles/Additive"sv,
              "Legacy Shaders/Particles/Additive (Soft)"sv};
    case Look::Transparent:
      return {"Unlit/Transparent"sv, "Legacy Shaders/Particles/Alpha Blended"sv, "Mobile/Particles/Alpha Blended"sv,
              "Particles/Alpha Blended"sv, "Legacy Shaders/Transparent/Diffuse"sv};
    case Look::Cutout:
      return {"Unlit/Transparent Cutout"sv, "Legacy Shaders/Transparent/Cutout/Diffuse"sv,
              "Legacy Shaders/Transparent/Cutout/VertexLit"sv};
    case Look::Opaque: break;
  }
  return {};
}

float TintScale(std::string_view standInShaderName) {
  return standInShaderName.rfind("Legacy Shaders/Particles/", 0) == 0 ? 0.5f : 1.0f;
}

}  // namespace Vivify::LookEstimate
