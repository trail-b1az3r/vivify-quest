#pragma once
#include <algorithm>
#include <array>
#include <optional>

namespace Vivify::BlitPolicy {
// CopyTexture elements are array layers, not destination eyes. A flat
// authored screen supplies the same element to both eyes.
inline std::optional<int> SourceElement(bool flat, bool array, int layers, int samples, int eye) {
  if (eye < 0 || eye > 1 || samples != 1) return {};
  if (flat && !array && layers == 1) return 0;
  if (array && !flat && layers == 2) return eye;
  return {};
}

inline std::array<float, 4> ScreenParams(int width, int height) {
  float const w = static_cast<float>(std::max(width, 1));
  float const h = static_cast<float>(std::max(height, 1));
  return {w, h, 1.0f + 1.0f / w, 1.0f + 1.0f / h};
}

// Restore the original keyword even if disabling it, recording commands or
// executing the commands throws. Do not enable a keyword that began disabled.
template<class Toggle, class Draw>
void DrawSingleView(bool stereoWasOn, Toggle toggle, Draw draw) {
  try {
    if (stereoWasOn) toggle(false);
    draw();
  } catch (...) {
    if (stereoWasOn) toggle(true);
    throw;
  }
  if (stereoWasOn) toggle(true);
}
}
