#include "VivifyRenderTexturePolicy.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace Vivify::RenderTexturePolicy;

int main() {
  int checks = 0;
  auto check = [&](bool ok, char const* description) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", description); std::exit(1); }
    ++checks;
  };
  check(DeclaredDimension({}, 1712, 1, 4096) == 1712, "relative texture follows the actual eye width");
  check(DeclaredDimension(512, 1712, 2, 4096) == 256, "authored size overrides camera size before ratio");
  check(DeclaredDimension(8192, 1712, 2, 4096) == 4096, "ratio precedes device-size cap");
  check(DeclaredDimension(1024, 1, 0.1f, 16384) == 10240, "float division matches PC without a one-pixel truncation error");
  check(DeclaredDimension({}, 1713, 2, 4096) == 856, "odd eye size truncates like PC");
  check(DeclaredDimension({}, 1712, 0, 4096) == 1712, "zero ratio cannot divide by zero");
  check(DeclaredDimension({}, 1712, -1, 4096) == 1712, "negative ratio is invalid");
  check(DeclaredDimension({}, 1712, std::numeric_limits<float>::quiet_NaN(), 4096) == 1712, "NaN ratio is invalid");
  check(DeclaredDimension({}, 1712, std::numeric_limits<float>::infinity(), 4096) == 1712, "infinite ratio is invalid");
  check(DeclaredDimension({}, 1712, std::numeric_limits<float>::denorm_min(), 4096) == 4096, "overflowing ratio result is capped before integer conversion");
  check(DeclaredDimension(-50, 1712, 1, 4096) == 1, "negative authored size remains bounded");
  check(DeclaredDimension(0, 1712, 1, 0) == 1, "nonpositive limit cannot create an empty texture");

  Descriptor stereo{1712,1712,1,2,1,48,0,0,5,0,2,0,0};
  struct Resource {
    std::optional<Descriptor> allocation;
    bool alive = false, created = false;
    int content = 0, allocations = 0;
    void Acquire(Descriptor const& descriptor) {
      if (!NeedsAllocation(allocation, descriptor, alive, created)) return;
      allocation = descriptor; alive = created = true; content = 0; ++allocations;
    }
  } feedback;
  feedback.Acquire(stereo); feedback.content = 123;
  for (int frame = 0; frame < 120; ++frame) feedback.Acquire(stereo);
  check(feedback.content == 123 && feedback.allocations == 1, "a feedback image persists across frames with identical descriptors");
  auto hdr = stereo; hdr.color = 52;
  feedback.Acquire(hdr);
  check(feedback.content == 0 && feedback.allocations == 2, "HDR format changes recreate an equally sized target");
  auto mono = hdr; mono.layers = 1; mono.dimension = 2; mono.vrUsage = 0;
  feedback.Acquire(mono);
  check(feedback.allocations == 3 && feedback.allocation->layers == 1, "a mono image cannot reuse a stereo array");
  feedback.created = false; feedback.Acquire(mono);
  check(feedback.created && feedback.allocations == 4, "lost native texture storage is recreated");
  feedback.alive = false; feedback.Acquire(mono);
  check(feedback.alive && feedback.allocations == 5, "destroyed target is recreated even if descriptor matches");
  auto depth = mono; depth.depth = 90;
  feedback.Acquire(depth);
  check(feedback.allocations == 6, "depth/stencil changes cannot retain incompatible storage");
  auto memoryless = depth; memoryless.memoryless = 1;
  feedback.Acquire(memoryless);
  check(feedback.allocations == 7, "memoryless attachment policy participates in allocation identity");
  auto resized = memoryless; resized.width = 1440;
  feedback.Acquire(resized);
  check(feedback.allocations == 8, "eye-resolution changes resize dependent textures");

  for (int width : {1,1712,4096,std::numeric_limits<int>::max()})
    for (float ratio : {0.000001f,0.1f,0.5f,1.0f,1.5f,100000.0f}) {
      int size = DeclaredDimension(width, 1, ratio, 4096);
      check(size >= 1 && size <= 4096, "extreme authored sizes and ratios stay within allocation bounds");
    }
  std::printf("%d render-texture layout, feedback and sizing checks passed\n", checks);
}
