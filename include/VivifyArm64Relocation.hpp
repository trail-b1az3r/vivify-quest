#pragma once
#include <cstdint>

namespace Vivify::Arm64Relocation {
using Address = std::uint64_t;
namespace Detail {
template<unsigned Bits>
constexpr std::int64_t SignedField(std::uint32_t raw) {
  static_assert(Bits > 0 && Bits < 32);
  auto const value = static_cast<std::int64_t>(raw & ((std::uint32_t(1) << Bits) - 1));
  return value >= (std::int64_t(1) << (Bits - 1)) ? value - (std::int64_t(1) << Bits) : value;
}
constexpr Address Add(Address pc, std::int64_t displacement) {
  // AArch64 addresses add modulo 2^64. The intentional unsigned conversion
  // here happens AFTER sign extension; it must not truncate to 32 bits.
  return pc + static_cast<Address>(displacement);
}
}

// These extract fields from instructions already recognized by the caller.
// They do not validate opcodes or prove that a target is mapped/readable.
constexpr std::int64_t LiteralDisplacement(std::uint32_t instruction) {
  return Detail::SignedField<19>((instruction >> 5) & 0x7ffffu) * 4;
}
constexpr Address LiteralTarget(Address pc, std::uint32_t instruction) {
  return Detail::Add(pc, LiteralDisplacement(instruction));
}
constexpr std::int64_t AdrDisplacement(std::uint32_t instruction) {
  std::uint32_t const field = (((instruction >> 5) & 0x7ffffu) << 2) | ((instruction >> 29) & 3u);
  return Detail::SignedField<21>(field);
}
constexpr Address AdrTarget(Address pc, std::uint32_t instruction) {
  return Detail::Add(pc, AdrDisplacement(instruction));
}
constexpr std::int64_t AdrpDisplacement(std::uint32_t instruction) {
  // Multiplication retains all 64 bits and avoids left-shifting a negative
  // signed value. The full displacement range is -4 GiB .. +4 GiB - 4 KiB.
  return AdrDisplacement(instruction) * 4096;
}
constexpr Address AdrpTarget(Address pc, std::uint32_t instruction) {
  return Detail::Add(pc & ~Address(4095), AdrpDisplacement(instruction));
}
constexpr std::int64_t ConditionalBranchDisplacement(std::uint32_t instruction) {
  return Detail::SignedField<19>((instruction >> 5) & 0x7ffffu) * 4;
}
constexpr Address ConditionalBranchTarget(Address pc, std::uint32_t instruction) {
  return Detail::Add(pc, ConditionalBranchDisplacement(instruction));
}
constexpr std::int64_t TestBranchDisplacement(std::uint32_t instruction) {
  return Detail::SignedField<14>((instruction >> 5) & 0x3fffu) * 4;
}
constexpr Address TestBranchTarget(Address pc, std::uint32_t instruction) {
  return Detail::Add(pc, TestBranchDisplacement(instruction));
}
}
