#include "VivifyArm64HookStub.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace Stub = Vivify::Arm64HookStub;
using Stub::Address;
static std::uint64_t checks = 0;

static void Require(bool condition, char const* reason) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << reason << '\n';
    std::exit(1);
  }
}

// Independent ARM move-wide interpreter: decode op, register, halfword and
// immediate fields. It does not use the production decoder or its masks.
static Address Execute(std::span<std::uint32_t const> words) {
  Address x17 = 0xfeedfacecafebeefull;
  for (auto word : words) {
    if ((word >> 21) == 0x6b0 && ((word >> 5) & 31) == 17 &&
        (word & 0x1fffffu) == 0x1f0220u) return x17;  // BR X17
    Require((word >> 31) == 1, "move is 64-bit");
    Require(((word >> 23) & 63) == 37, "move-wide instruction class");
    Require((word & 31) == 17, "move writes scratch X17");
    auto const operation = (word >> 29) & 3;
    auto const shift = ((word >> 21) & 3) * 16;
    Address const immediate = (word >> 5) & 0xffffu;
    if (operation == 2) {
      x17 = immediate << shift;
    } else {
      Require(operation == 3, "only MOVZ and MOVK are emitted");
      x17 = (x17 & ~(Address{0xffff} << shift)) | (immediate << shift);
    }
  }
  Require(false, "stub must branch before falling through");
  return 0;
}

// The unmodified And64 relocation loop dispatches these PC-relative classes.
// Instructions outside them are copied verbatim, including MOVZ/MOVK/BR/NOP.
static bool LegacyPcRelative(std::uint32_t word) {
  return (word & 0xfc000000u) == 0x14000000u ||
      (word & 0xfc000000u) == 0x94000000u ||
      (word & 0xff000010u) == 0x54000000u ||
      (word & 0x7f000000u) == 0x34000000u ||
      (word & 0x7f000000u) == 0x35000000u ||
      (word & 0x7f000000u) == 0x36000000u ||
      (word & 0x7f000000u) == 0x37000000u ||
      (word & 0xbf000000u) == 0x18000000u ||
      (word & 0x3f000000u) == 0x1c000000u ||
      (word & 0xff000000u) == 0x98000000u ||
      (word & 0x9f000000u) == 0x10000000u ||
      (word & 0x9f000000u) == 0x90000000u;
}

static void CheckTarget(Address target) {
  auto encoded = Stub::Encode48(target);
  Require(encoded.has_value(), "valid target encodes");
  Require(Execute(*encoded) == target, "independent instruction execution reaches target");
  for (auto word : *encoded)
    Require(!LegacyPcRelative(word), "old installer must never see PC-relative data");
  for (Address symbol : {Address{0x100000}, Address{0x100004}}) {
    std::array<std::uint32_t, 5> image{
        (*encoded)[0], (*encoded)[1], (*encoded)[2], (*encoded)[3], Stub::Nop};
    auto count = Stub::EntryWordCount(symbol);
    auto view = std::span(image).first(count);
    Require(Stub::HasStubPrefix(view, symbol), "new prefix recognized");
    auto decoded = Stub::DecodeExisting(view, symbol, 0x200000);
    Require(decoded && decoded->target == target && decoded->wordCount == count &&
            decoded->kind == Stub::Kind::Mov48, "new stub round trip");
    // Old installer copies count words then appends a callback. BR is inside
    // the copied prefix, so it reaches target without executing that callback.
    for (auto word : view) Require(!LegacyPcRelative(word), "entire legacy window is code");
    Require(Execute(view) == target, "legacy copied stub preserves target at any PC");
  }
}

int main() {
  static_assert(Stub::Encode48(4).has_value());
  static_assert(!Stub::Encode48(3));
  static_assert(Stub::EntryWordCount(0x100004) == 5);

  // Exhaust every value in each immediate halfword independently. Other
  // halfwords stay nonzero and distinct; expected values come from the loop.
  for (unsigned lane = 0; lane != 3; ++lane) {
    unsigned shift = lane * 16;
    for (Address field = 0; field != 65536; ++field) {
      Address target = (0x123456789abcull & ~(Address{0xffff} << shift)) |
          (field << shift);
      if ((target & 3u) != 0) {
        Require(!Stub::Encode48(target), "misaligned field rejected");
      } else {
        CheckTarget(target);
      }
    }
  }
  for (Address target : {Address{4}, Address{0xfffc}, Address{0x10000},
                        Address{0xfffffffc}, Address{0x100000000},
                        Address{0xfffffffffffc}}) CheckTarget(target);
  for (Address target : {Address{0}, Address{1}, Address{2}, Address{3},
                        Address{0x1000000000000}, std::numeric_limits<Address>::max()})
    Require(!Stub::ValidTarget(target) && !Stub::Encode48(target), "invalid target rejected");
  Require(Stub::EntryWordCount(3) == 0, "invalid symbol rejected");

  // The five saved crashing words are pointer payloads in old stubs. Each
  // passes an old literal-load test when mistakenly decoded as an instruction.
  for (std::uint32_t low : {0x58f41f0cu, 0x1ceba500u, 0x18c041a8u,
                            0x18f58834u, 0x18edec08u}) {
    Address const target = (Address{0x6d} << 32) | low;
    Require(LegacyPcRelative(low), "saved payload triggers old decoder");
    for (Address symbol : {Address{0x100000}, Address{0x100004}}) {
      std::array<std::uint32_t, 5> image{};
      std::size_t pool;
      if (symbol & 7) {
        image = {Stub::Nop, Stub::LoadX17, Stub::BranchX17, low, 0x6d};
        pool = 3;
      } else {
        image = {Stub::LoadX17, Stub::BranchX17, low, 0x6d, 0xdeadbeef};
        pool = 2;
      }
      auto decoded = Stub::DecodeExisting(image, symbol, 0x200000);
      Require(Stub::HasStubPrefix(image, symbol), "old prefix recognized");
      Require(decoded && decoded->target == target, "old payload preserved as data");
      Require(decoded->kind == (pool == 2 ? Stub::Kind::LiteralAligned : Stub::Kind::LiteralPadded), "old layout classified");
      Require(!Stub::DecodeExisting(image, symbol ^ 4, 0x200000), "wrong alignment rejected");
      Require(Stub::HasStubPrefix(image, symbol ^ 4), "wrong alignment cannot fall back to pool decoding");
      Require(!Stub::DecodeExisting(image, symbol, target), "duplicate replacement rejected");
      Require(!Stub::DecodeExisting(image, symbol, symbol), "replacement self-loop rejected");
      image[pool] = static_cast<std::uint32_t>(symbol);
      image[pool + 1] = 0;
      Require(!Stub::DecodeExisting(image, symbol, 0x200000), "old target self-loop rejected");
      image[pool] = 0;
      Require(!Stub::DecodeExisting(image, symbol, 0x200000), "null old target rejected");
      image[pool + 1] = 0x10000;
      Require(!Stub::DecodeExisting(image, symbol, 0x200000), "noncanonical old target rejected");
    }
  }

  auto encoded = *Stub::Encode48(0x123456789abc);
  std::array<std::uint32_t, 5> image{encoded[0], encoded[1], encoded[2], encoded[3], Stub::Nop};
  for (std::size_t size = 0; size < 5; ++size)
    Require(!Stub::DecodeExisting(std::span(image).first(size), 0x100004, 0x200000), "short snapshot rejected");
  image[4] = 0xdeadbeef;
  Require(Stub::HasStubPrefix(image, 0x100004), "bad fifth word still recognizable");
  Require(!Stub::DecodeExisting(image, 0x100004, 0x200000), "missing fifth NOP rejected");
  Require(Stub::DecodeExisting(std::span(image).first(4), 0x100000, 0x200000).has_value(), "aligned stub needs four words only");
  for (unsigned index = 0; index < 4; ++index) {
    auto wrong = image;
    wrong[index] ^= 1;  // Wrong destination register, or BR's fixed low bits.
    Require(!Stub::HasStubPrefix(wrong, 0x100000), "near-match must not be accepted");
    Require(!Stub::DecodeExisting(wrong, 0x100000, 0x200000), "altered opcode rejected");
  }
  std::array<std::uint32_t, 2> prefix{Stub::LoadX17, Stub::BranchX17};
  Require(Stub::HasStubPrefix(prefix, 0x100000), "truncated old prefix remains recognizable");
  Require(!Stub::DecodeExisting(prefix, 0x100000, 0x200000), "truncated literal payload rejected");
  std::cout << checks << " hook stub checks passed\n";
}
