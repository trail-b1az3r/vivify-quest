// Host test for src/VivifyLookEstimate.cpp.
#include "VivifyLookEstimate.hpp"

#include <cstdio>

using namespace Vivify::LookEstimate;

static int fails = 0;
static void Expect(char const* what, Inputs const& in, Look want) {
  auto const got = Classify(in);
  bool const ok = got.look == want;
  std::printf("%s %-46s -> %-13s (%s)\n", ok ? "ok  " : "FAIL", what, LookName(got.look), got.reason.c_str());
  if (!ok) ++fails;
}

int main() {
  Inputs in;
  in = {}; in.shaderName = "Custom/Thing"; in.srcBlend = 1; in.dstBlend = 1;
  Expect("blend One/One is additive", in, Look::Additive);
  in = {}; in.shaderName = "Custom/Thing"; in.srcBlend = 5; in.dstBlend = 1;
  Expect("blend SrcAlpha/One is additive", in, Look::Additive);
  in = {}; in.shaderName = "Custom/Thing"; in.srcBlend = 5; in.dstBlend = 10;
  Expect("blend SrcAlpha/OneMinusSrcAlpha is blended", in, Look::Transparent);
  in = {}; in.shaderName = "Custom/Glow"; in.srcBlend = 1; in.dstBlend = 0;
  Expect("explicit opaque blend beats a 'glow' name", in, Look::Opaque);
  in = {}; in.shaderName = "Standard"; in.srcBlend = 1; in.dstBlend = 0; in.keywords = {"_ALPHATEST_ON"};
  Expect("Standard cutout keyword", in, Look::Cutout);
  in = {}; in.shaderName = "Standard"; in.keywords = {"_ALPHAPREMULTIPLY_ON"};
  Expect("Standard transparent keyword", in, Look::Transparent);
  in = {}; in.shaderName = "Custom/Circle"; in.renderQueue = 3000;
  Expect("Transmission's empty Custom/Circle, queue 3000", in, Look::Transparent);
  in = {}; in.shaderName = "Custom/Circle"; in.renderQueue = 2000;
  Expect("an empty opaque-queue shader stays opaque", in, Look::Opaque);
  in = {}; in.shaderName = "Swifter/Aether/VFX/StarFlare";
  Expect("name 'flare'", in, Look::Additive);
  in = {}; in.shaderName = "Roystan/Grass";
  Expect("name 'grass'", in, Look::Cutout);
  in = {}; in.shaderName = "Mochie/Water";
  Expect("name 'water'", in, Look::Transparent);
  in = {}; in.shaderName = "Custom/Thing"; in.renderType = "TransparentCutout";
  Expect("RenderType TransparentCutout", in, Look::Cutout);
  in = {}; in.shaderName = "Custom/Thing"; in.renderType = "Transparent"; in.hasEmission = true;
  Expect("transparent tag with emission glows", in, Look::Additive);
  in = {}; in.shaderName = "Custom/ParticleThing"; in.renderQueue = 3100;
  Expect("particle in the transparent queue", in, Look::Additive);
  in = {}; in.shaderName = "Custom/Thing"; in.zWrite = 0.0f;
  Expect("no depth write", in, Look::Transparent);
  in = {}; in.shaderName = "Hidden/Locked/.poiyomi/Poiyomi Toon World/abc";
  Expect("plain toon shader stays opaque", in, Look::Opaque);
  if (Candidates(Look::Opaque).size() != 0 || Candidates(Look::Additive).empty()) { std::puts("FAIL candidates"); ++fails; }
  if (TintScale("Legacy Shaders/Particles/Additive") != 0.5f || TintScale("Unlit/Transparent") != 1.0f) {
    std::puts("FAIL tint scale"); ++fails;
  }
  std::printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
