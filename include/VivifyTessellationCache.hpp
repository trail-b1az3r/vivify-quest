#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace Vivify::TessellationCache {
// Bump when the supported compiler contract, reflection or five-stage linking
// changes. This is independent of the unchanged default scene-eye cache.
inline constexpr char Policy[] = "uniform-triangle-pair-v1-actual-hull-five-stages";
struct Fingerprint {
  uint64_t size = 0, content = 0;
  bool valid = false;
  bool operator==(Fingerprint const& other) const {
    return valid && other.valid && size == other.size && content == other.content;
  }
};
inline Fingerprint FingerprintFile(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  Fingerprint result{0, 14695981039346656037ull, false};
  std::array<char, 65536> bytes;
  while (in) {
    in.read(bytes.data(), bytes.size());
    auto const count = in.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      result.content ^= static_cast<unsigned char>(bytes[static_cast<size_t>(i)]);
      result.content *= 1099511628211ull;
    }
    result.size += static_cast<uint64_t>(count);
  }
  result.valid = in.eof() && !in.bad() && result.size > 0;
  return result;
}
inline std::vector<std::string> Sorted(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  return names;
}
inline bool ValidNames(std::vector<std::string> const& names) {
  if (names.empty() || names.size() > 4096) return false;
  auto sorted = Sorted(names);
  return !sorted.front().empty() && std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}
struct Conversion {
  bool complete = false;
  int translated = 0, untranslated = 0;
  std::vector<std::string> names;
};
inline bool Covers(Conversion const& conversion, std::vector<std::string> const& expected) {
  return ValidNames(expected) && conversion.complete && conversion.untranslated == 0 &&
      conversion.translated == static_cast<int>(expected.size()) &&
      conversion.translated == static_cast<int>(conversion.names.size()) &&
      Sorted(conversion.names) == Sorted(expected);
}
struct Context {
  std::string source, cacheKey, marker;
  std::array<std::string, 2> paths;
  std::vector<std::string> expected;
};
inline bool ReadProof(Context const& context, Fingerprint const& source) {
  std::error_code ec;
  auto const size = std::filesystem::file_size(context.marker, ec);
  if (ec || size == 0 || size > 65536 || !ValidNames(context.expected)) return false;
  std::ifstream in(context.marker);
  std::string policy, cacheKey;
  Fingerprint savedSource, left, right;
  size_t count = 0;
  if (!(in >> std::quoted(policy) >> std::quoted(cacheKey) >> savedSource.size >> savedSource.content >>
        left.size >> left.content >> right.size >> right.content >> count) ||
      policy != Policy || cacheKey != context.cacheKey || count != context.expected.size() || count > 4096) return false;
  savedSource.valid = left.valid = right.valid = true;
  std::vector<std::string> names(count);
  for (auto& name : names) if (!(in >> std::quoted(name))) return false;
  in >> std::ws;
  return in.peek() == std::char_traits<char>::eof() && Sorted(names) == Sorted(context.expected) &&
      savedSource == source && left == FingerprintFile(context.paths[0]) && right == FingerprintFile(context.paths[1]);
}
struct Result {
  bool selected = false, cached = false;
  std::array<std::string, 2> paths;
  std::vector<std::string> covered;
  std::string reason;
};
// Conversion is supplied by the runtime; host tests exercise the same pair
// transaction with bounded disposable files and injected failures.
template<class Convert>
Result Prepare(bool requested, bool deviceSupports, bool stageInfoComplete, Context const& context, Convert convert) {
  Result result;
  if (!requested) { result.reason = "disabled"; return result; }
  if (!deviceSupports) { result.reason = "Unity tessellation capability is unavailable"; return result; }
  if (!stageInfoComplete || !ValidNames(context.expected)) {
    result.reason = "no complete supported source-stage classification"; return result;
  }
  try {
    auto const source = FingerprintFile(context.source);
    if (!source.valid) { result.reason = "source content could not be fingerprinted"; return result; }
    if (ReadProof(context, source)) {
      result.selected = result.cached = true; result.paths = context.paths; result.covered = context.expected;
      return result;
    }
    std::array<std::string, 3> temporary{context.paths[0] + ".pending", context.paths[1] + ".pending", context.marker + ".pending"};
    struct Cleanup {
      std::array<std::string, 3> const& paths;
      ~Cleanup() noexcept {
        for (auto const& path : paths) try { std::error_code ec; std::filesystem::remove(path, ec); } catch (...) {}
      }
    } cleanup{temporary};
    for (auto const& path : temporary) { std::error_code ec; std::filesystem::remove(path, ec); }
    std::array<Fingerprint, 2> fingerprint;
    for (int eye = 0; eye != 2; ++eye) {
      if (!Covers(convert(temporary[eye], eye), context.expected)) {
        result.reason = "both scene eyes did not prove complete supported hull/domain coverage"; return result;
      }
      fingerprint[eye] = FingerprintFile(temporary[eye]);
      if (!fingerprint[eye].valid) { result.reason = "a converted scene-eye file is empty or unreadable"; return result; }
    }
    if (!(source == FingerprintFile(context.source))) { result.reason = "source changed during pair conversion"; return result; }
    // Invalidate first, commit BOTH files, then publish the proof last. A
    // crash or failed second rename cannot authorize one tessellated eye.
    std::error_code ec;
    std::filesystem::remove(context.marker, ec);
    if (ec) { result.reason = "old pair proof could not be invalidated"; return result; }
    for (int eye = 0; eye != 2; ++eye) {
      std::filesystem::rename(temporary[eye], context.paths[eye], ec);
      if (ec || !(fingerprint[eye] == FingerprintFile(context.paths[eye]))) {
        result.reason = "scene-eye pair could not be committed"; return result;
      }
    }
    std::ofstream proof(temporary[2], std::ios::trunc);
    proof << std::quoted(std::string(Policy)) << '\n' << std::quoted(context.cacheKey) << '\n'
          << source.size << ' ' << source.content << '\n';
    for (auto const& value : fingerprint) proof << value.size << ' ' << value.content << '\n';
    proof << context.expected.size() << '\n';
    for (auto const& name : context.expected) proof << std::quoted(name) << '\n';
    proof.flush(); proof.close();
    if (!proof) { result.reason = "scene-eye proof could not be written"; return result; }
    std::filesystem::rename(temporary[2], context.marker, ec);
    if (ec || !ReadProof(context, source)) { result.reason = "scene-eye proof did not validate"; return result; }
    result.selected = true; result.paths = context.paths; result.covered = context.expected;
    return result;
  } catch (...) { result.reason = "optional tessellation preparation threw; using the normal scene eyes"; return result; }
}
}
