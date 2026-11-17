#include "VivifyCrashReport.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace Vivify::CrashReport {
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
constexpr size_t kSignalCount = std::size(kSignals);

struct sigaction gPrevious[kSignalCount];
bool gHavePrevious[kSignalCount] = {};
bool gInstalled = false;

// Paths and version as plain buffers: the handler cannot allocate.
char gPendingPath[512] = {};
char gVersion[64] = {};
std::string gDirectory;

std::atomic<bool> gHandling{false};

// The last log lines, oldest overwritten first. Written under the log sink's
// lock; the handler reads them without one (a line being written at the
// moment of the crash may come out torn, which is acceptable).
constexpr int kRingLines = 40;
constexpr int kLineBytes = 240;
char gRing[kRingLines][kLineBytes] = {};
std::atomic<uint32_t> gRingNext{0};

// --- async-signal-safe output -------------------------------------------

void PutBytes(int fd, char const* text, size_t length);

void Put(int fd, char const* text) {
  if (text != nullptr) PutBytes(fd, text, std::strlen(text));
}

void PutBytes(int fd, char const* text, size_t length) {
  while (length > 0) {
    ssize_t const written = write(fd, text, length);
    if (written <= 0) return;
    text += written;
    length -= static_cast<size_t>(written);
  }
}

void PutHex(int fd, uintptr_t value) {
  char buffer[2 + 16 + 1];
  buffer[0] = '0';
  buffer[1] = 'x';
  char digits[16];
  int count = 0;
  do {
    digits[count++] = "0123456789abcdef"[value & 0xF];
    value >>= 4;
  } while (value != 0 && count < 16);
  int at = 2;
  while (count > 0) buffer[at++] = digits[--count];
  buffer[at] = '\0';
  Put(fd, buffer);
}

void PutDec(int fd, long long value) {
  char buffer[24];
  char digits[21];
  int count = 0;
  bool const negative = value < 0;
  unsigned long long magnitude = negative ? 0ull - static_cast<unsigned long long>(value)
                                          : static_cast<unsigned long long>(value);
  do {
    digits[count++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0 && count < 20);
  int at = 0;
  if (negative) buffer[at++] = '-';
  while (count > 0) buffer[at++] = digits[--count];
  buffer[at] = '\0';
  Put(fd, buffer);
}

char const* SignalName(int signal) {
  switch (signal) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGBUS: return "SIGBUS (bad memory access)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGTRAP: return "SIGTRAP (trap)";
    default: return "signal";
  }
}

void PutFrame(int fd, int index, char const* kind, uintptr_t pc) {
  Put(fd, "  #");
  if (index < 10) Put(fd, "0");
  PutDec(fd, index);
  Put(fd, " ");
  Put(fd, kind);
  Put(fd, " ");
  PutHex(fd, pc);
  // dladdr is not on the async-signal-safe list, but it only reads the
  // linker's tables; crash reporters use it the same way.
  Dl_info info{};
  if (pc != 0 && dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fname != nullptr) {
    char const* name = std::strrchr(info.dli_fname, '/');
    Put(fd, "  ");
    Put(fd, name != nullptr ? name + 1 : info.dli_fname);
    Put(fd, " +");
    PutHex(fd, pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
    if (info.dli_sname != nullptr) {
      Put(fd, " (");
      Put(fd, info.dli_sname);
      Put(fd, " +");
      PutHex(fd, pc - reinterpret_cast<uintptr_t>(info.dli_saddr));
      Put(fd, ")");
    }
  }
  Put(fd, "\n");
}

void WriteReport(int fd, int signal, siginfo_t* info, void* context) {
  Put(fd, "=== Vivify ");
  Put(fd, gVersion);
  Put(fd, " native crash at ");
  PutDec(fd, static_cast<long long>(time(nullptr)));
  Put(fd, " (seconds since 1970) ===\n");
  Put(fd, SignalName(signal));
  Put(fd, ", code ");
  PutDec(fd, info != nullptr ? info->si_code : 0);
  Put(fd, ", fault address ");
  PutHex(fd, info != nullptr ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
  Put(fd, "\nthread ");
  PutDec(fd, static_cast<long long>(syscall(SYS_gettid)));
  char threadName[17] = {};
  if (prctl(PR_GET_NAME, threadName, 0, 0, 0) == 0) {
    Put(fd, " '");
    Put(fd, threadName);
    Put(fd, "'");
  }
  Put(fd, "\n");

  uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
  bool haveRegisters = false;
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
  if (uc != nullptr) {
    pc = uc->uc_mcontext.pc;
    lr = uc->uc_mcontext.regs[30];
    sp = uc->uc_mcontext.sp;
    fp = uc->uc_mcontext.regs[29];
    haveRegisters = true;
  }
#elif defined(__x86_64__)
  // For the host test (tools/crashreport); the frame records are laid out the same.
  if (uc != nullptr) {
    pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
    fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    haveRegisters = true;
  }
#endif
  if (haveRegisters) {
    Put(fd, "pc ");
    PutHex(fd, pc);
    Put(fd, "  lr ");
    PutHex(fd, lr);
    Put(fd, "  sp ");
    PutHex(fd, sp);
    Put(fd, "  fp ");
    PutHex(fd, fp);
    Put(fd, "\nbacktrace:\n");
    int index = 0;
    PutFrame(fd, index++, "pc", pc);
    if (lr != 0) PutFrame(fd, index++, "lr", lr);
    // Frame records {previous fp, return address} on this thread's stack.
    // Only followed upwards from the stack pointer and within a bound, so a
    // corrupt chain ends the walk rather than faulting in here.
    uintptr_t const limit = sp + 8u * 1024u * 1024u;
    uintptr_t previousReturn = lr;
    for (int depth = 0; depth < 48; depth++) {
      if (fp < sp || fp >= limit || (fp & 0x7) != 0) break;
      auto const* record = reinterpret_cast<uintptr_t const*>(fp);
      uintptr_t const nextFp = record[0];
      uintptr_t const returnAddress = record[1];
      if (returnAddress == 0) break;
      // On arm64 the first record usually repeats lr.
      if (!(depth == 0 && returnAddress == previousReturn)) PutFrame(fd, index++, "fp", returnAddress);
      previousReturn = returnAddress;
      if (nextFp <= fp) break;
      fp = nextFp;
    }
  }

  Put(fd, "last Vivify log lines (oldest first):\n");
  uint32_t const next = gRingNext.load(std::memory_order_relaxed);
  for (int i = 0; i < kRingLines; i++) {
    char const* line = gRing[(next + static_cast<uint32_t>(i)) % kRingLines];
    if (line[0] == '\0') continue;
    Put(fd, "  ");
    // Bounded: a torn line may have lost its terminator.
    size_t const length = strnlen(line, kLineBytes);
    PutBytes(fd, line, length);
    Put(fd, "\n");
  }
  Put(fd, "=== end ===\n");
}

int SignalIndex(int signal) {
  for (size_t i = 0; i < kSignalCount; i++) {
    if (kSignals[i] == signal) return static_cast<int>(i);
  }
  return -1;
}

void Handler(int signal, siginfo_t* info, void* context);

void HandOn(int signal, siginfo_t* info, void* context) {
  int const index = SignalIndex(signal);
  struct sigaction previous {};
  if (index >= 0 && gHavePrevious[index]) {
    previous = gPrevious[index];
  } else {
    previous.sa_handler = SIG_DFL;
  }
  // What was there before goes back, so from here on the crash is handled
  // exactly as it would have been without Vivify.
  if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler == SIG_IGN) previous.sa_handler = SIG_DFL;
  sigaction(signal, &previous, nullptr);
  if (previous.sa_flags & SA_SIGINFO) {
    if (previous.sa_sigaction != nullptr) {
      previous.sa_sigaction(signal, info, context);
      return;
    }
  } else if (previous.sa_handler != SIG_DFL) {
    previous.sa_handler(signal);
    return;
  }
  // Default action. A fault re-runs the instruction on return and is fatal
  // then; a signal that was sent (abort, kill) has to be sent again.
  if (info == nullptr || info->si_code <= 0) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
}

void Handler(int signal, siginfo_t* info, void* context) {
  if (!gHandling.exchange(true)) {
    int const fd = open(gPendingPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      WriteReport(fd, signal, info, context);
      fsync(fd);
      close(fd);
    }
  }
  HandOn(signal, info, context);
}

void Take(size_t i) {
  struct sigaction current {};
  if (sigaction(kSignals[i], nullptr, &current) != 0) return;
  if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == Handler) return;
  struct sigaction ours {};
  ours.sa_sigaction = Handler;
  ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&ours.sa_mask);
  if (sigaction(kSignals[i], &ours, nullptr) == 0) {
    gPrevious[i] = current;
    gHavePrevious[i] = true;
  }
}

}  // namespace

void Install(std::string const& directory, std::string_view version) {
  gDirectory = directory;
  std::string const pending = (std::filesystem::path(directory) / "VivifyCrash.new").string();
  std::strncpy(gPendingPath, pending.c_str(), sizeof(gPendingPath) - 1);
  std::size_t const n = std::min(version.size(), sizeof(gVersion) - 1);
  std::memcpy(gVersion, version.data(), n);
  gVersion[n] = '\0';
  for (size_t i = 0; i < kSignalCount; i++) Take(i);
  gInstalled = true;
}

void Reinstall() {
  if (!gInstalled) return;
  for (size_t i = 0; i < kSignalCount; i++) Take(i);
}

void NoteLine(std::string_view line) {
  uint32_t const slot = gRingNext.load(std::memory_order_relaxed) % kRingLines;
  char* out = gRing[slot];
  size_t const n = std::min(line.size(), static_cast<size_t>(kLineBytes - 1));
  std::memcpy(out, line.data(), n);
  out[n] = '\0';
  for (size_t i = 0; i < n; i++) {
    if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
  }
  gRingNext.store(slot + 1, std::memory_order_relaxed);
}

std::string TakePendingReport() {
  if (gDirectory.empty()) return {};
  std::filesystem::path const pending = std::filesystem::path(gDirectory) / "VivifyCrash.new";
  std::filesystem::path const kept = std::filesystem::path(gDirectory) / "VivifyCrash.txt";
  std::error_code ec;
  if (!std::filesystem::exists(pending, ec)) return {};
  std::ifstream in(pending);
  std::stringstream text;
  text << in.rdbuf();
  in.close();
  // VivifyCrash.txt keeps the reports of earlier crashes; it starts again once
  // it passes half a megabyte.
  bool const fresh = !std::filesystem::exists(kept, ec) || std::filesystem::file_size(kept, ec) > 512 * 1024;
  std::ofstream out(kept, fresh ? std::ios::trunc : std::ios::app);
  out << text.str() << "\n";
  out.close();
  std::filesystem::remove(pending, ec);
  return text.str();
}

}  // namespace Vivify::CrashReport
