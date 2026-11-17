#include "VivifyArm64Relocation.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace A = Vivify::Arm64Relocation;
static std::uint64_t checks = 0;
static void Check(bool condition, char const* message) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
// The oracle starts from the desired SIGNED displacement, then encodes it.
// It does not decode the instruction with the implementation under test.
static A::Address ReferenceTarget(A::Address pc, std::int64_t displacement) {
  return displacement < 0 ? pc - (static_cast<std::uint64_t>(-(displacement + 1)) + 1)
                          : pc + static_cast<std::uint64_t>(displacement);
}
static std::uint32_t EncodeAdr(std::int32_t displacement, bool page, unsigned rd) {
  std::uint32_t const bits = static_cast<std::uint32_t>(displacement) & 0x1fffffu;
  return (page ? 0x90000000u : 0x10000000u) | ((bits & 3u) << 29) | ((bits >> 2) << 5) | rd;
}

int main() {
  constexpr A::Address pc = 0x6ebbea5b28;
  constexpr std::uint32_t crashInstruction = 0x18edec08;
  static_assert(A::LiteralDisplacement(crashInstruction) == -148096);
  static_assert(A::LiteralTarget(pc, crashInstruction) == 0x6ebbe818a8);
  Check(A::LiteralTarget(pc, crashInstruction) == 0x6ebbe818a8, "recorded fault's intended literal address");
  // Reproduce the old unsigned-mask expression only to prove its distinctive
  // bad target matches the recorded memcpy source, not as the new oracle.
  auto const oldOffset = (static_cast<std::int32_t>(crashInstruction << 8) >> 11) & ~3u;
  Check(pc + oldOffset == 0x6fbbe818a8, "old expression exactly reproduces recorded fault source");
  Check((pc + oldOffset) - A::LiteralTarget(pc, crashInstruction) == (A::Address(1) << 32), "recorded error is exactly four GiB");
  struct Crash { A::Address pc; std::uint32_t instruction; A::Address intended, fault; };
  constexpr std::array<Crash,5> recorded = {{
    {0x6ed4224b28,0x58f41f0c,0x6ed420cf08,0x6fd420cf08},
    {0x6ebe4602a0,0x1ceba500,0x6ebe437740,0x6fbe437740},
    {0x6eba94a2e0,0x18c041a8,0x6eba8cab14,0x6fba8cab14},
    {0x6ed7c75fb0,0x18f58834,0x6ed7c610b4,0x6fd7c610b4},
    {0x6ebbea5b28,0x18edec08,0x6ebbe818a8,0x6fbbe818a8}
  }};
  for (auto const& c : recorded) {
    Check(A::LiteralTarget(c.pc,c.instruction)==c.intended, "all five recorded LDR W/X/S crash targets corrected");
    auto const buggy = (static_cast<std::int32_t>(c.instruction << 8) >> 11) & ~3u;
    Check(c.pc + buggy == c.fault, "all five recorded literal-copy faults reproduced");
  }

  // Every possible signed imm19, all literal data widths and boundary Rt
  // registers. Opcode/width/register bits must not leak into the displacement.
  constexpr std::array<std::uint32_t, 7> literals = {0x18000000u,0x58000000u,0x98000000u,0x1c000000u,0x5c000000u,0x9c000000u,0xd8000000u};
  for (std::int32_t displacement = -(1 << 20); displacement < (1 << 20); displacement += 4) {
    auto const field = static_cast<std::uint32_t>(displacement / 4) & 0x7ffffu;
    for (auto opcode : literals) for (unsigned rt : {0u,31u}) {
      auto const instruction = opcode | (field << 5) | rt;
      Check(A::LiteralDisplacement(instruction) == displacement, "all literal imm19 displacements");
      Check(A::LiteralTarget(pc, instruction) == ReferenceTarget(pc, displacement), "all literal addresses");
    }
    for (auto opcode : {0x54000000u,0x34000000u,0x35000000u,0xb4000000u,0xb5000000u}) {
      auto const instruction = opcode | (field << 5) | 15u;
      Check(A::ConditionalBranchDisplacement(instruction) == displacement, "all conditional and compare branch displacements");
      Check(A::ConditionalBranchTarget(pc, instruction) == ReferenceTarget(pc, displacement), "all conditional and compare branch targets");
    }
  }
  // Every signed imm21. ADR is byte-relative; ADRP is page-relative and must
  // preserve negative offsets as well as the full +/-4 GiB page range.
  for (std::int32_t displacement = -(1 << 20); displacement < (1 << 20); ++displacement) {
    unsigned const rd = (static_cast<unsigned>(displacement) & 1u) ? 31u : 0u;
    auto const instruction = EncodeAdr(displacement, false, rd);
    Check(A::AdrDisplacement(instruction) == displacement, "all ADR imm21 displacements");
    Check(A::AdrTarget(pc, instruction) == ReferenceTarget(pc, displacement), "all ADR addresses");
    auto const pageInstruction = EncodeAdr(displacement, true, rd);
    auto const bytes = std::int64_t(displacement) * 4096;
    Check(A::AdrpDisplacement(pageInstruction) == bytes, "all ADRP signed 64-bit displacements");
    Check(A::AdrpTarget(pc, pageInstruction) == ReferenceTarget(pc - pc % 4096, bytes), "all ADRP page-aligned addresses");
  }
  for (std::int32_t displacement = -(1 << 15); displacement < (1 << 15); displacement += 4) {
    auto const field = static_cast<std::uint32_t>(displacement / 4) & 0x3fffu;
    for (auto opcode : {0x36000000u,0x37000000u,0xb6f80000u,0xb7f80000u}) {
      auto const instruction = opcode | (field << 5) | 31u;
      Check(A::TestBranchDisplacement(instruction) == displacement, "all test-bit branch imm14 displacements");
      Check(A::TestBranchTarget(pc, instruction) == ReferenceTarget(pc, displacement), "all test-bit branch targets");
    }
  }
  // Wrapping and page-edge reference checks use unsigned address arithmetic
  // independently of the decoder's sign handling.
  for (A::Address edgePc : {A::Address(0), A::Address(3), A::Address(4095), A::Address(4096),
                           std::numeric_limits<A::Address>::max(), A::Address(0x6ffffffffc)}) {
    for (std::int32_t displacement : {-1048576,-4,-1,0,1,4,1048575}) {
      Check(A::AdrTarget(edgePc, EncodeAdr(displacement, false, 31)) == ReferenceTarget(edgePc, displacement), "ADR address/page boundaries");
      Check(A::AdrpTarget(edgePc, EncodeAdr(displacement, true, 31)) ==
            ReferenceTarget(edgePc - edgePc % 4096, std::int64_t(displacement) * 4096), "ADRP address/page boundaries");
    }
  }
  std::printf("%llu signed ARM64 relocation checks passed; recorded +4 GiB literal fault reproduced and corrected\n", static_cast<unsigned long long>(checks));
}
