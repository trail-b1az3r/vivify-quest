#include "VivifySeparateEyePolicy.hpp"
#include <cstdio>
#include <cstdlib>
using namespace Vivify::SeparateEye;
int main() {
  FrameConditions good;
  good.requested = good.ready = good.playing = good.stereo = good.twoLayerArray = good.resolved = good.copySupported = good.clearsBackground = true;
  int passed = 0;
  auto check = [&](bool ok, char const* name) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
    ++passed;
  };
  check(Rejection(good).empty(), "supported frame accepted");
  for (auto field : {&FrameConditions::requested, &FrameConditions::ready, &FrameConditions::playing,
                     &FrameConditions::stereo, &FrameConditions::twoLayerArray, &FrameConditions::resolved,
                     &FrameConditions::copySupported, &FrameConditions::clearsBackground}) {
    auto changed = good; changed.*field = false;
    check(!Rejection(changed).empty(), "missing prerequisite preserves original frame");
  }
  for (auto field : {&FrameConditions::paused, &FrameConditions::resetting, &FrameConditions::multiplayer, &FrameConditions::recovery,
                     &FrameConditions::reflections, &FrameConditions::midEffects, &FrameConditions::secondaryCameras, &FrameConditions::sharedEffectTargets,
                     &FrameConditions::externalCommands, &FrameConditions::croppedViewport}) {
    auto changed = good; changed.*field = true;
    check(!Rejection(changed).empty(), "unsupported frame preserves original renderer");
  }
  check(!Rejection(FrameConditions{}).empty(), "safe defaults refuse rendering");
  for (unsigned bits = 0; bits < 16; ++bits) {
    NativeSceneConditions native{bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8)};
    check(CanReuseMultiview(native) == (bits == 15), "native scene reuse needs every compatibility condition");
  }
  for (unsigned bits = 0; bits < 32; ++bits) {
    NativeMaterialConditions material{bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), bool(bits & 16)};
    check(CanReuseNativeMaterial(material) == (bits == 15), "fallback/unknown or active eye-tessellation materials cannot take native shortcut");
  }
  check(CanReuseNativeMaterial({true, true, true, true, false}), "supported upstream geometry retains native rendering");
  check(!CanReuseNativeMaterial({true, true, true, true, true}), "active paired tessellation retains its scene-eye renderer");
  for (unsigned bits = 0; bits < 32; ++bits) {
    EmptyGeometryConditions proof{bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), bool(bits & 16)};
    check(CanUseEmptyGeometry(proof) == (bits == 31), "empty specialization requires camera, shadow, provenance and donor proofs");
  }
  for (int layer = 0; layer < 32; ++layer) {
    uint32_t const bit = uint32_t(1) << layer;
    check(NeedsCameraMaterials(bit, layer, true), "included layer always retains material preparation");
    check(!NeedsCameraMaterials(~bit, layer, true), "excluded layer can skip when shadows are disabled");
    check(NeedsCameraMaterials(~bit, layer, false), "potential shadow casters stay even on excluded layers");
  }
  check(NeedsCameraMaterials(0, -1, true), "invalid negative layer is conservative");
  check(NeedsCameraMaterials(0, 32, true), "invalid high layer is conservative");
  check(NeedsCameraMaterials(~uint32_t(0), 31, true), "sign-bit layer survives mask conversion");
  auto nativeOnly = good; nativeOnly.copySupported = nativeOnly.resolved = false; nativeOnly.croppedViewport = true;
  check(Rejection(nativeOnly, false).empty(), "native reuse does not need mono-copy capabilities or a full viewport");
  check(!Rejection(nativeOnly, true).empty(), "separate eyes retain their copy/viewport requirements");
  auto background = good; background.clearsBackground = false;
  check(Rejection(background, false).empty(), "native original camera retains its background clear behavior");
  check(!Rejection(background).empty(), "replacement eyes cannot replay a background-dependent camera");
  SceneFrame transaction;
  check(!transaction.Ready(10, 123), "no completed image means main draw cannot be suppressed");
  transaction.Begin(10, 123);
  check(!transaction.Ready(10, 123), "starting an eye render is not completion");
  transaction.Complete();
  check(transaction.Ready(10, 123), "both eyes completed for current main camera");
  check(!transaction.Ready(11, 123), "stale image cannot suppress a later frame");
  check(!transaction.Ready(10, 456), "another camera cannot consume the image");
  transaction.Begin(11, 123);
  check(!transaction.Ready(11, 123), "new frame failure cannot reuse old completion");
  transaction.Complete(); transaction.Clear();
  check(!transaction.Ready(11, 123), "disable/reset clears completed image");
  transaction.Begin(12, 0); transaction.Complete();
  check(!transaction.Ready(12, 0), "null camera cannot complete a frame");
  transaction.Begin(-1, 123); transaction.Complete();
  check(!transaction.Ready(-1, 123), "invalid frame cannot be presented");
  MirrorRenderStack mirrors;
  check(mirrors.Resolve(1) == 0, "inactive mirror has no redirected renderer");
  mirrors.Push(1, 101);
  check(mirrors.Resolve(1) == 101 && mirrors.Resolve(101) == 101, "recursive original/private mirror preserves effective identity");
  mirrors.Push(2, 202);
  check(mirrors.Resolve(1) == 101 && mirrors.Resolve(2) == 202, "nested different mirrors retain each original renderer identity");
  check(mirrors.Resolve(3) == 0 && mirrors.Resolve(0) == 0, "unrelated/null mirrors cannot inherit another cache");
  mirrors.Push(1, 101); mirrors.Pop();
  check(mirrors.Resolve(1) == 101 && mirrors.Resolve(2) == 202, "unwinding recursion preserves outer renderers");
  mirrors.Pop();
  check(mirrors.Resolve(2) == 0 && mirrors.Resolve(1) == 101, "returning from nested mirror releases just its scope");
  mirrors.Pop(); mirrors.Pop();
  check(mirrors.Resolve(1) == 0 && mirrors.Resolve(101) == 0, "completed mirror scopes cannot redirect a later frame");
  std::printf("%d renderer policy and frame-lifecycle checks passed\n", passed);
}
