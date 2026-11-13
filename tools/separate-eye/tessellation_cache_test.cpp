#include "VivifyTessellationCache.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace Vivify::TessellationCache;
namespace fs = std::filesystem;
static int checked = 0;
static void check(bool value, char const* message) {
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
  ++checked;
}
static void write(std::string const& path, std::string const& value) { std::ofstream(path, std::ios::binary) << value; }
int main() {
  auto root = fs::temp_directory_path() / ("vivify-tess-pair-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  int fixture = 0;
  auto context = [&] {
    auto dir = root / std::to_string(fixture++); fs::create_directory(dir);
    Context c{(dir / "source").string(), "source cache key with spaces|compiler-v1", (dir / "pair.proof").string(),
              {(dir / "scene-left").string(), (dir / "scene-right").string()}, {"Grass"}};
    write(c.source, "original source");
    return c;
  };
  int calls = 0;
  auto convert = [&](std::string const& path, int eye) {
    ++calls; write(path, eye ? "right complete bundle" : "left complete bundle");
    return Conversion{true, 1, 0, {"Grass"}};
  };
  auto c = context();
  check(!Prepare(false, true, true, c, convert).selected && calls == 0, "default off performs no conversion");
  check(!fs::exists(c.marker) && !fs::exists(c.paths[0]) && !fs::exists(c.paths[1]), "default off leaves new cache paths untouched");
  check(!Prepare(true, false, true, c, convert).selected && calls == 0, "unknown device support falls back before conversion");
  check(!Prepare(true, true, false, c, convert).selected && calls == 0, "incomplete source classification cannot authorize tessellation");
  check(!Covers(Conversion{}, c.expected), "unknown conversion defaults cannot count as translated");
  check(!Covers({true, 0, 0, {}}, c.expected), "zero translated shaders cannot authorize a pair");
  check(!Covers({true, 1, 1, {"Grass"}}, c.expected), "untranslated stages cannot count as complete");
  check(!Covers({true, 1, 0, {"Other"}}, c.expected), "count equality alone cannot prove shader identity");
  check(!Covers({true, 2, 0, {"Grass", "Grass"}}, c.expected), "duplicate shader names cannot mask an unknown shader");
  check(!Covers({true, 1, 0, {"Grass"}}, {"Grass", "Unknown"}), "mixed unknown maps keep the whole normal pair");
  auto valid = Prepare(true, true, true, c, convert);
  check(valid.selected && !valid.cached && calls == 2, "both complete eyes are committed together");
  check(valid.paths == c.paths && valid.covered == c.expected, "only proven named scene-eye coverage is exposed");
  check(fs::exists(c.marker) && ReadProof(c, FingerprintFile(c.source)), "committed pair proof validates files/source/context");
  auto cache = Prepare(true, true, true, c, convert);
  check(cache.selected && cache.cached && calls == 2, "a valid pair cache needs no recompilation");
  auto changedContext = c; changedContext.cacheKey += "|new compiler policy";
  check(!ReadProof(changedContext, FingerprintFile(c.source)), "compiler/source cache policy is part of the proof");
  auto changedNames = c; changedNames.expected = {"Other"};
  check(!ReadProof(changedNames, FingerprintFile(c.source)), "marker names must match current source classification");
  write(c.paths[0], "LEFT complete bundle"); // exactly the same byte length
  check(!ReadProof(c, FingerprintFile(c.source)), "equal-size cache corruption invalidates its content fingerprint");
  auto decline = [](std::string const&, int) { return Conversion{}; };
  auto corrupt = Prepare(true, true, true, c, decline);
  check(!corrupt.selected && corrupt.covered.empty(), "corrupt cache plus unsupported conversion cannot reuse stale coverage");
  check(Prepare(true, true, true, c, convert).selected, "an invalidated cache can be rebuilt as a complete pair");
  write(c.source, "changed! source"); // same byte length as the initial source
  check(!ReadProof(c, FingerprintFile(c.source)), "source content changes invalidate the pair independently of size");
  c = context();
  auto oneEye = [&](std::string const& path, int eye) {
    if (!eye) return convert(path, eye);
    write(path, "partial right"); return Conversion{};
  };
  auto partial = Prepare(true, true, true, c, oneEye);
  check(!partial.selected && partial.covered.empty(), "one supported eye never enables tessellation");
  check(!fs::exists(c.marker) && !fs::exists(c.paths[0]) && !fs::exists(c.paths[1]), "failed right conversion publishes no final pair");
  check(!fs::exists(c.paths[0] + ".pending") && !fs::exists(c.paths[1] + ".pending"), "failed conversion temporary files are removed");
  c = context();
  auto throws = [&](std::string const& path, int eye) -> Conversion {
    convert(path, eye); throw std::runtime_error("injected converter failure");
  };
  check(!Prepare(true, true, true, c, throws).selected && !fs::exists(c.marker), "an optional converter exception preserves fallback");
  c = context();
  auto secondThrows = [&](std::string const& path, int eye) -> Conversion {
    convert(path, eye); if (eye) throw std::runtime_error("injected second-eye provider failure");
    return {true, 1, 0, {"Grass"}};
  };
  check(!Prepare(true, true, true, c, secondThrows).selected && !fs::exists(c.marker), "second-eye provider exceptions keep the whole normal pair");
  check(!fs::exists(c.paths[0] + ".pending") && !fs::exists(c.paths[1] + ".pending"), "second-eye exception cleans both temporary donors");
  c = context();
  auto emptyFile = [](std::string const& path, int) { write(path, ""); return Conversion{true, 1, 0, {"Grass"}}; };
  check(!Prepare(true, true, true, c, emptyFile).selected && !fs::exists(c.marker), "an empty donor after a failed write cannot publish coverage");
  c = context();
  auto normalPath = (root / "original-v9-eye").string(); write(normalPath, "untouched normal donor");
  auto const normalFingerprint = FingerprintFile(normalPath);
  auto proofWriteFails = [&](std::string const& path, int eye) {
    auto converted = convert(path, eye);
    if (eye) fs::create_directory(c.marker + ".pending"); // force stream-open/write failure without filling the disk
    return converted;
  };
  check(!Prepare(true, true, true, c, proofWriteFails).selected && !fs::exists(c.marker), "proof write failure cannot publish either tessellated eye");
  check(FingerprintFile(normalPath) == normalFingerprint, "optional disk/write failure does not touch a normal v9 donor");
  check(!ReadProof(c, FingerprintFile(c.source)), "unproven files from failed disk writes cannot become active cache hits");
  // Linux CI additionally exercises an actual ENOSPC-producing stream. This
  // character device never consumes storage and is unavailable on macOS.
  if (fs::exists("/dev/full")) {
    c = context();
    auto fullDevice = [&](std::string const& path, int eye) {
      auto converted = convert(path, eye);
      if (eye) fs::create_symlink("/dev/full", c.marker + ".pending");
      return converted;
    };
    check(!Prepare(true, true, true, c, fullDevice).selected && !fs::exists(c.marker), "ENOSPC while flushing pair proof preserves normal donors");
  }
  c = context();
  auto sourceChanges = [&](std::string const& path, int eye) {
    auto converted = convert(path, eye); if (eye) write(c.source, "source replaced"); return converted;
  };
  check(!Prepare(true, true, true, c, sourceChanges).selected && !fs::exists(c.marker), "a source change during conversion cannot publish coverage");
  c = context(); fs::create_directory(c.paths[1]);
  auto commitFailure = Prepare(true, true, true, c, convert);
  check(!commitFailure.selected && !fs::exists(c.marker), "a failed second final rename cannot authorize the first eye");
  check(commitFailure.covered.empty(), "failed pair commits expose no translated shader count");
  c = context(); c.expected = {"Grass\nname with \"quotes\" and \\backslash"};
  auto unusualName = [&](std::string const& path, int eye) {
    convert(path, eye); return Conversion{true, 1, 0, c.expected};
  };
  check(Prepare(true, true, true, c, unusualName).selected && ReadProof(c, FingerprintFile(c.source)), "coverage names survive exact quoted serialization");
  write(c.marker, "unknown or truncated proof");
  check(!Prepare(true, true, true, c, decline).selected, "truncated proof is never accepted by file existence alone");
  write(c.marker, std::string(70000, 'x'));
  check(!ReadProof(c, FingerprintFile(c.source)), "oversized cache proofs are rejected before parsing");
  check(!Prepare(false, true, true, c, convert).selected, "turning the feature off ignores even an existing candidate cache");
  fs::remove_all(root);
  std::printf("%d tessellation pair-cache checks passed\n", checked);
}
