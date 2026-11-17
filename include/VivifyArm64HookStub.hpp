#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Vivify::Arm64HookStub {

using Address = std::uint64_t;
using Words = std::array<std::uint32_t, 4>;
inline constexpr std::uint32_t Nop = 0xd503201fu;
inline constexpr std::uint32_t LoadX17 = 0x58000051u;
inline constexpr std::uint32_t BranchX17 = 0xd61f0220u;

// Quest native code addresses currently fit in 48 bits. This is an encoding
// check only: the caller must validate mapping, permissions and lifetime.
constexpr bool ValidTarget(Address target) {
  return target != 0 && (target >> 48) == 0 && (target & 3u) == 0;
}

// Matches the old installer's fixed four/five-word relocation window.
constexpr std::size_t EntryWordCount(Address symbol) {
  return !ValidTarget(symbol) ? 0 : ((symbol & 7u) == 0 ? 4 : 5);
}

// No inline pointer data: older installers can copy these as instructions.
// At a four-byte-misaligned entry the caller must append Nop as word five.
constexpr std::optional<Words> Encode48(Address target) {
  if (!ValidTarget(target)) return std::nullopt;
  return Words{
      0xd2800011u | (static_cast<std::uint32_t>(target & 0xffffu) << 5),
      0xf2a00011u | (static_cast<std::uint32_t>((target >> 16) & 0xffffu) << 5),
      0xf2c00011u | (static_cast<std::uint32_t>((target >> 32) & 0xffffu) << 5),
      BranchX17};
}

enum class Kind { LiteralAligned, LiteralPadded, Mov48 };
struct ExistingStub {
  Address target;
  std::size_t wordCount;
  Kind kind;
};

namespace Detail {
constexpr bool MovPrefix(std::span<std::uint32_t const> words) {
  constexpr std::uint32_t fixed = 0xffe0001fu;
  return words.size() >= 4 &&
      (words[0] & fixed) == 0xd2800011u &&
      (words[1] & fixed) == 0xf2a00011u &&
      (words[2] & fixed) == 0xf2c00011u && words[3] == BranchX17;
}
constexpr bool LiteralPrefix(std::span<std::uint32_t const> words) {
  return words.size() >= 2 && words[0] == LoadX17 && words[1] == BranchX17;
}
}  // namespace Detail

// A known prefix with an invalid payload must NOT fall back to instruction
// relocation: that would decode the pointer pool again. Recognize either old
// shape even at an unexpected alignment so the strict decoder can reject it.
constexpr bool HasStubPrefix(std::span<std::uint32_t const> words, Address symbol) {
  if (EntryWordCount(symbol) == 0) return false;
  return Detail::LiteralPrefix(words) ||
      (words.size() >= 3 && words[0] == Nop &&
       Detail::LiteralPrefix(words.subspan(1))) || Detail::MovPrefix(words);
}

// The caller supplies a bounded, already-readable snapshot; no raw reads or
// mutation occur here. A null result plus HasStubPrefix=true means fail closed.
constexpr std::optional<ExistingStub> DecodeExisting(
    std::span<std::uint32_t const> words, Address symbol, Address replacement) {
  auto const count = EntryWordCount(symbol);
  if (count == 0 || words.size() < count || !ValidTarget(replacement) ||
      replacement == symbol) return std::nullopt;
  Address target = 0;
  Kind kind;
  if (Detail::MovPrefix(words)) {
    if (count == 5 && words[4] != Nop) return std::nullopt;
    target = static_cast<Address>((words[0] >> 5) & 0xffffu) |
        (static_cast<Address>((words[1] >> 5) & 0xffffu) << 16) |
        (static_cast<Address>((words[2] >> 5) & 0xffffu) << 32);
    kind = Kind::Mov48;
  } else {
    std::size_t pool;
    if (count == 4 && Detail::LiteralPrefix(words)) {
      pool = 2;
      kind = Kind::LiteralAligned;
    } else if (count == 5 && words[0] == Nop &&
               Detail::LiteralPrefix(words.subspan(1))) {
      pool = 3;
      kind = Kind::LiteralPadded;
    } else {
      return std::nullopt;
    }
    target = static_cast<Address>(words[pool]) |
        (static_cast<Address>(words[pool + 1]) << 32);
  }
  if (!ValidTarget(target) || target == symbol || target == replacement)
    return std::nullopt;
  return ExistingStub{target, count, kind};
}

}  // namespace Vivify::Arm64HookStub
