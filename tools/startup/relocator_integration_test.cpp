// Include the actual generated relocator core, excluding hook-installation
// wrappers and their static RWX pool initializer. No generated code executes.
#include "And64RelocatorCore.inc"
#include <array>
#include <cstdio>
#include <vector>

static unsigned passed;
static void Check(bool ok, char const* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
  ++passed;
}
static uint32_t Imm19(uint32_t opcode, int displacement) {
  return opcode | ((uint32_t(displacement / 4) & 0x7ffffu) << 5);
}
static uint32_t Adr(uint32_t opcode, int displacement) {
  return opcode | ((uint32_t(displacement) & 3) << 29) |
      (((uint32_t(displacement) >> 2) & 0x7ffffu) << 5);
}
static int64_t Sign(uint32_t bits, unsigned width) {
  return bits >= (1u << (width - 1)) ? int64_t(bits) - (int64_t(1) << width) : bits;
}
static uint64_t Read64(void const* p) { uint64_t value; memcpy(&value, p, 8); return value; }
int main() {
  // One real allocation keeps target and output addresses valid and separated
  // beyond literal/conditional ranges. No executable mapping is requested.
  std::vector<uint32_t> storage(3 * 1024 * 1024);
  auto* source = reinterpret_cast<uint32_t*>((reinterpret_cast<uintptr_t>(storage.data() + 1024 * 1024) + 4095) & ~uintptr_t(4095));
  auto* farOutput = storage.data() + 2 * 1024 * 1024;
  auto* nearOutput = source + 128;
  auto run = [&](uint32_t ins, uint32_t* output, auto fix) {
    *source = ins; memset(output, 0, 256);
    context c{}; c.basep = reinterpret_cast<int64_t>(source); c.endp = c.basep + 4;
    uint32_t* input = source; uint32_t* out = output;
    Check(fix(&input, &out, &c), "instruction recognized");
    Check(input == source + 1 && out > output, "input/output advancement");
    return out;
  };
  // Exact saved instruction words include LDR W, X and SIMD S. Also cover Q,
  // LDRSW, positive/negative extrema, and near displacement re-encoding.
  for (uint32_t opcode : {0x18000008u, 0x5800000cu, 0x1c000000u, 0x9c000003u, 0x98000004u}) {
    for (int displacement : {-1048576, -148096, -16, 16, 1048560}) {
      auto* target = reinterpret_cast<unsigned char*>(source) + displacement;
      std::array<unsigned char, 16> bytes{};
      for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(i + passed);
      memcpy(target, bytes.data(), bytes.size());
      auto* end = run(Imm19(opcode, displacement), farOutput, __fix_loadlit);
      auto* load = farOutput; while (*load == A64_NOP) ++load;
      auto const width = opcode == 0x5800000cu || opcode == 0x98000004u ? 8 : opcode == 0x9c000003u ? 16 : 4;
      Check((load[0] & 0xff00001f) == (opcode & 0xff00001f), "literal opcode/register preserved");
      Check(Sign((load[0] >> 5) & 0x7ffff, 19) * 4 == 8, "literal reads embedded data");
      Check(memcmp(load + 2, bytes.data(), width) == 0, "far literal copies correct source bytes");
      Check(load + 2 + width / 4 == end, "literal branch skips entire payload");
      Check(load[1] == 0x14000001u + width / 4, "literal continuation skips data");
    }
    for (int displacement : {-16384, -64, 64, 16384}) {
      auto const ins = Imm19(opcode, displacement);
      run(ins, nearOutput, __fix_loadlit);
      auto* load = nearOutput; while (*load == A64_NOP) ++load;
      auto const target = reinterpret_cast<uintptr_t>(load) + Sign((load[0] >> 5) & 0x7ffff, 19) * 4;
      Check(target == reinterpret_cast<uintptr_t>(source) + displacement, "near literal re-encodes signed target");
    }
  }
  for (uint32_t opcode : {0x54000000u, 0x34000009u, 0x3500000au, 0xb400000bu, 0xb500000cu, 0x3600000du, 0xb708000eu}) {
    bool const test = (opcode & 0x7e000000) == 0x36000000;
    unsigned const width = test ? 14 : 19;
    uint32_t const mask = test ? 0x3fff : 0x7ffff;
    for (int displacement : {-32768, -4, 4, 32764}) {
      auto ins = opcode | ((uint32_t(displacement / 4) & mask) << 5);
      run(ins, farOutput, __fix_cond_comp_test_branch);
      auto* branch = farOutput; while (*branch == A64_NOP) ++branch;
      Check(Sign((branch[0] >> 5) & mask, width) * 4 == 8, "far conditional selects absolute branch");
      Check(Read64(branch + 4) == reinterpret_cast<uintptr_t>(source) + displacement, "far conditional embeds signed target");
      Check(branch[1] == 0x14000005 && branch[2] == 0x58000051 && branch[3] == 0xd61f0220, "conditional continuation and scratch branch preserved");
      run(ins, nearOutput, __fix_cond_comp_test_branch);
      auto* near = nearOutput; while (*near == A64_NOP) ++near;
      if ((near[0] & ~uint32_t(mask << 5)) == (ins & ~uint32_t(mask << 5)) && near[1] != 0x14000005) {
        Check(reinterpret_cast<uintptr_t>(near) + Sign((near[0] >> 5) & mask, width) * 4 ==
              reinterpret_cast<uintptr_t>(source) + displacement, "near conditional re-encodes signed target");
      }
    }
  }
  for (int displacement : {-1048576, -148095, -4, 4, 1048575}) {
    auto ins = Adr(0x10000005, displacement);
    run(ins, farOutput, __fix_pcreladdr);
    auto* load = farOutput; while (*load == A64_NOP) ++load;
    Check(Read64(load + 2) == reinterpret_cast<uintptr_t>(source) + displacement, "far ADR embeds signed byte address");
  }
  for (int pages : {-1048576, -1000, -1, 0, 1, 1000, 1048575}) {
    auto* end = run(Adr(0x90000006, pages), farOutput, __fix_pcreladdr);
    auto* load = farOutput; while (*load == A64_NOP) ++load;
    Check(end == load + 4, "ADRP materializes original page even when it contains the prologue");
    uint64_t const expected = (reinterpret_cast<uintptr_t>(source) & ~uint64_t(4095)) + int64_t(pages) * 4096;
    Check(Read64(load + 2) == expected, "ADRP embeds full signed page address");
  }
  // Exercise the complete dispatcher and its forward-reference fix map too.
  for (uint32_t opcode : {0x54000000u, 0x35000009u, 0x3600000au}) {
    source[0] = opcode | (1u << 5); source[1] = A64_NOP;
    __fix_instructions(source, 2, farOutput);
    Check((farOutput[0] & 0x00ffffe0) == (1u << 5) && farOutput[1] == A64_NOP,
          "forward conditional follows relocated next instruction");
    source[0] = A64_NOP;
    source[1] = opcode | (((opcode & 0x7e000000) == 0x36000000 ? 0x3fff : 0x7ffff) << 5);
    __fix_instructions(source, 2, farOutput);
    Check(farOutput[0] == A64_NOP && (farOutput[1] & 0x00ffffe0) == (source[1] & 0x00ffffe0),
          "dispatcher preserves backward prologue branch");
  }
  // A backward reference within a relocated prologue must follow the relocated
  // instruction rather than point back into overwritten source code.
  for (uint32_t opcode : {0x54000000u, 0x35000009u, 0x3600000au}) {
    source[0] = A64_NOP; source[1] = opcode | (((opcode & 0x7e000000) == 0x36000000 ? 0x3fff : 0x7ffff) << 5);
    context c{}; c.basep = reinterpret_cast<int64_t>(source); c.endp = c.basep + 8;
    c.get_and_set_current_index(source, farOutput);
    uint32_t* input = source + 1; uint32_t* out = farOutput + 1;
    Check(__fix_cond_comp_test_branch(&input, &out, &c), "internal backward conditional recognized");
    Check((farOutput[1] & 0x00ffffe0) == (source[1] & 0x00ffffe0), "internal backward reference follows relocated instruction");
  }
  std::printf("%u actual relocator integration checks passed (no generated code executed)\n", passed);
}
