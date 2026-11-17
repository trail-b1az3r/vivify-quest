#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace Vivify::RenderTexturePolicy {
// Allocation identity, including the fields that distinguish an HDR stereo
// array from an equally sized ordinary screen image. Store the requested
// descriptor; Unity can normalize bookkeeping flags in the returned descriptor.
struct Descriptor {
  int width = 0, height = 0, samples = 0, layers = 0, mips = 0;
  int color = 0, stencil = 0, depth = 0, dimension = 0, shadowSampling = 0;
  int vrUsage = 0, flags = 0, memoryless = 0;
  bool operator==(Descriptor const&) const = default;
};

inline int DeclaredDimension(std::optional<int> authored, int inherited, float ratio, int limit) {
  // Divide before clamping: an authored 8192 / 2 is 4096, not 2048.
  // Convert only after bounding the floating result, including tiny ratios.
  float const divisor = std::isfinite(ratio) && ratio > 0 ? ratio : 1.0f;
  // PC performs float division before truncation. Double division can be one
  // pixel smaller for ratios such as 0.1f, even with the same stored input.
  float const size = static_cast<float>(authored.value_or(inherited)) / divisor;
  return static_cast<int>(std::clamp(static_cast<double>(size), 1.0, static_cast<double>(std::max(1, limit))));
}

inline bool NeedsAllocation(std::optional<Descriptor> const& allocated, Descriptor const& requested,
                            bool alive, bool created) {
  return !alive || !created || !allocated || *allocated != requested;
}
}
