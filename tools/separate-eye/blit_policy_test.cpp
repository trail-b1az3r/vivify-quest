#include "VivifyBlitPolicy.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
using namespace Vivify::BlitPolicy;

int main() {
  int checks = 0;
  auto check = [&](bool ok, char const* why) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
    ++checks;
  };
  // A flat authored TV and a stereo scene carry distinct images. Both eyes
  // must receive valid source pixels without reading a nonexistent TV layer.
  std::vector<int> flat{23}, stereo{41, 79};
  for (int eye = 0; eye < 2; ++eye) {
    check(flat.at(SourceElement(true, false, 1, 1, eye).value()) == 23, "flat screen reaches both eyes");
    check(stereo.at(SourceElement(false, true, 2, 1, eye).value()) == stereo.at(eye), "stereo scene preserves eye identity");
  }
  for (int eye : {-1, 2, 10})
    check(!SourceElement(false, true, 2, 1, eye), "invalid destination eye refuses layer copies");
  for (int layers : {0, 1, 3, 6})
    check(!SourceElement(false, true, layers, 1, 1), "non-stereo array cannot enter stereo copy path");
  for (int samples : {0, 2, 4, 8})
    check(!SourceElement(false, true, 2, samples, 1), "unresolved source refuses raw copies");
  check(!SourceElement(false, false, 1, 1, 1), "cube or volume source refuses array-layer copying");
  check(!SourceElement(true, true, 2, 1, 1), "contradictory texture metadata is refused");

  // One chain changes between full scene, a small TV, and an odd-size mask.
  // Destination pixel coordinates remain at its own resolution at every step.
  for (auto size : {std::array<int, 2>{1712, 1808}, {512, 256}, {257, 129}, {1712, 1808}, {0, -1}}) {
    auto p = ScreenParams(size[0], size[1]);
    check(p[0] == std::max(1, size[0]) && p[1] == std::max(1, size[1]), "effect follows its current target, not previous camera");
    check(std::abs((p[2] - 1) * p[0] - 1) < 0.0002f && std::abs((p[3] - 1) * p[1] - 1) < 0.0002f,
          "pixel reciprocal terms remain finite at every target size");
  }

  // Inject a failure at keyword disable and at every command in a two-eye
  // copy/draw/execute sequence. A later camera must retain its initial mode.
  for (bool initiallyStereo : {false, true}) {
    for (int failure = -1; failure < 8; ++failure) {
      bool stereoEnabled = initiallyStereo, caught = false;
      int step = 0, toggles = 0;
      try {
        DrawSingleView(initiallyStereo, [&](bool enable) {
          stereoEnabled = enable; ++toggles;
          if (!enable && failure == 7) throw std::runtime_error("disable failed after mutation");
        }, [&] {
          check(!stereoEnabled, "two-eye flat texture draws select single-view shader");
          for (; step < 7; ++step) if (step == failure) throw std::runtime_error("command failed");
        });
      } catch (std::runtime_error const&) { caught = true; }
      check(stereoEnabled == initiallyStereo, "failure cannot leave the next camera in the wrong stereo mode");
      check(caught == (failure >= 0 && (failure < 7 || initiallyStereo)), "command failure propagates to renderer fallback");
      check(toggles == (initiallyStereo ? 2 : 0), "initially mono mode is never enabled during restoration");
    }
  }
  std::printf("%d screen-texture routing, sizing and failure-restoration checks passed\n", checks);
}
