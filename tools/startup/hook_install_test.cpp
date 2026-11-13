// Exercise the actual generated installer against data buffers. Memory
// protection is intercepted; no executable allocation or generated code runs.
#include <sys/mman.h>
#include <cstddef>
static bool allowProtection = true;
static unsigned protectionCalls;
static void (*protectionObserver)() = nullptr;
static int TestMprotect(void*, std::size_t, int) {
  ++protectionCalls;
  if (protectionObserver) protectionObserver();
  return allowProtection ? 0 : -1;
}
#define mprotect TestMprotect
#include "And64HookInstaller.inc"
#undef mprotect
#include <algorithm>
#include <cstdlib>
#include <cstdio>
namespace Stub = Vivify::Arm64HookStub;
static unsigned passed;
static void** publishedResult;
static uint64_t expectedPrior;
static void Check(bool ok, char const* why) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
  ++passed;
}
static void CheckPublishedChain() {
  Check(publishedResult && *publishedResult, "original chain pointer published before entry mutation");
  auto target = reinterpret_cast<uint64_t>(*publishedResult);
  auto stub = Stub::DecodeExisting(std::span<uint32_t const>(static_cast<uint32_t*>(*publishedResult), 4), target, expectedPrior + 4);
  Check(stub && stub->target == expectedPrior, "published chain is fully initialized before entry mutation");
}
int main() {
  for (unsigned padding : {0u, 1u}) {
    alignas(8) std::array<uint32_t, 12> source;
    auto* entry = source.data() + padding;
    auto const address = reinterpret_cast<uint64_t>(entry);
    auto const count = Stub::EntryWordCount(address);
    auto const replacement = uint64_t(0x7000001000);
    alignas(8) std::array<uint32_t, 50> backup{};
    auto install = [&](uint64_t target, size_t capacity = sizeof(backup)) {
      return A64HookFunctionV(entry, reinterpret_cast<void*>(target), backup.data(), capacity);
    };
    for (uint32_t pool : {0x58f41f0cu, 0x1ceba500u, 0x18c041a8u, 0x18f58834u, 0x18edec08u}) {
      source.fill(Stub::Nop);
      size_t const first = padding ? 1 : 0;
      entry[first] = Stub::LoadX17; entry[first + 1] = Stub::BranchX17;
      entry[first + 2] = pool; entry[first + 3] = 0x6e;
      uint64_t const prior = (uint64_t(0x6e) << 32) | pool;
      auto* chain = install(replacement);
      auto chainStub = Stub::DecodeExisting(backup, reinterpret_cast<uint64_t>(backup.data()), replacement);
      Check(chain == backup.data() && chainStub && chainStub->target == prior,
            "existing literal stub gets a populated code-only prior chain without decoding its pointer");
      auto decoded = Stub::DecodeExisting(std::span<uint32_t const>(entry, count), address, replacement + 4);
      Check(decoded && decoded->target == replacement && decoded->kind == Stub::Kind::Mov48,
            "installed entry is a data-free exact target jump");
      Check(entry[count] == Stub::Nop, "installer preserves following source word");
      auto unchanged = source;
      Check(install(replacement) == nullptr && source == unchanged, "duplicate replacement fails without rewriting entry");
      Check(install(replacement + 4) == backup.data(), "subsequent hook retains allocated chain identity");
      chainStub = Stub::DecodeExisting(backup, reinterpret_cast<uint64_t>(backup.data()), replacement + 4);
      Check(chainStub && chainStub->target == replacement, "subsequent hook chains to previous MOV target");
    }
    // The previously published original pointer must refer to a populated
    // trampoline before the new entry can start invoking its replacement.
    source.fill(Stub::Nop);
    auto words = Stub::Encode48(replacement);
    std::copy(words->begin(), words->end(), entry);
    void* result = nullptr;
    publishedResult = &result; expectedPrior = replacement;
    protectionObserver = &CheckPublishedChain;
    A64HookFunction(entry, reinterpret_cast<void*>(replacement + 8), &result);
    protectionObserver = nullptr; publishedResult = nullptr;
    auto resultStub = Stub::DecodeExisting(std::span<uint32_t const>(static_cast<uint32_t*>(result), 4),
                                          reinterpret_cast<uint64_t>(result), replacement + 8);
    Check(resultStub && resultStub->target == replacement, "public wrapper returns populated prior chain");
    source.fill(Stub::Nop);
    auto before = source;
    auto calls = protectionCalls;
    Check(install(replacement, count * 10 * sizeof(uint32_t) - 1) == nullptr && source == before && protectionCalls == calls,
          "undersized trampoline fails before protection or source writes");
    Check(install(replacement) == backup.data(), "ordinary unhooked code uses its relocated trampoline");
    for (size_t i = 0; i < count; ++i) Check(backup[i] == Stub::Nop, "original NOP instructions copied into trampoline");
    source.fill(Stub::Nop); before = source; allowProtection = false;
    Check(install(replacement) == nullptr && source == before, "protection failure leaves source unchanged");
    allowProtection = true;
    Check(install(uint64_t(1) << 48) == nullptr && source == before, "unsupported pointer width refuses installation");
    Check(install(address) == nullptr && source == before, "self target refuses installation");
    entry[0] = Stub::LoadX17; entry[1] = Stub::BranchX17; entry[2] = 0; entry[3] = 0;
    before = source; calls = protectionCalls;
    Check(install(replacement) == nullptr && source == before && protectionCalls == calls,
          "recognizable invalid literal stub never falls through to instruction relocation");
  }
  std::printf("%u actual installer and hook-chain checks passed (protection intercepted; no code executed)\n", passed);
}
