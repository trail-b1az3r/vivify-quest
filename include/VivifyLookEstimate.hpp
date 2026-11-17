#pragma once
// Estimated look (0.15.27): what a material whose shader cannot run here was
// meant to look like, worked out from what survives of it.
//
// A converted map's material keeps its property values, render queue, blend
// factors, keywords and RenderType tag whatever happens to its shader, since
// those live in the SerializedFile, not in the compiled programs. Until now
// every material Vivify could not run was given the same opaque stand-in, so a
// glow, a glass pane, a particle quad and a wall all came out as solid flat
// surfaces. This reads those surviving values and the shader's name and picks
// the kind of stand-in that draws the same way: opaque, alpha-cut, alpha-
// blended or additive. A shader shipped with no programs at all (Transmission's
// 'Custom/Circle') draws nothing even on PC; an estimate is the best anything
// can do for it.
//
// No game types: tools/lookestimate tests it on the host.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify::LookEstimate {

enum class Look { Opaque, Cutout, Transparent, Additive };

struct Inputs {
  std::string shaderName;      // the original shader's name
  int renderQueue = -1;        // the material's queue (-1: unknown)
  std::optional<int> srcBlend; // UnityEngine.Rendering.BlendMode values
  std::optional<int> dstBlend;
  std::optional<float> zWrite; // _ZWrite, when the material has one
  std::string renderType;      // the RenderType tag, when set
  std::vector<std::string> keywords;
  bool hasEmission = false;    // a non-black emission colour
};

struct Estimate {
  Look look = Look::Opaque;
  // Why, for the log: "blend One/One", "queue 3000", "name 'glow'", ...
  std::string reason;
};

Estimate Classify(Inputs const& inputs);

char const* LookName(Look look);

// Shaders that draw this look, best first; the runtime takes the first one
// this device can run. Empty for Opaque, which keeps Vivify's generic
// stand-in.
std::vector<std::string_view> Candidates(Look look);

// The Legacy particle shaders compute 2 * tint * texture, so a tint of 0.5
// is "as authored"; everything else multiplies by the tint as it is.
float TintScale(std::string_view standInShaderName);

}  // namespace Vivify::LookEstimate
