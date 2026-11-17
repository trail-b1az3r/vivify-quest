// Host test for src/VivifyCrashReport.cpp: a child process crashes on purpose
// three calls deep and the report it leaves is checked.
//   g++ -std=c++20 -O0 -fno-omit-frame-pointer -rdynamic -I include -o /tmp/crashtest
//       tools/crashreport/main.cpp src/VivifyCrashReport.cpp
//   /tmp/crashtest
#include "VivifyCrashReport.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace C = Vivify::CrashReport;

int failures = 0;
void check(char const* what, bool ok) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

extern "C" __attribute__((noinline)) void vivify_test_fault(int volatile* where) { *where = 1; }
extern "C" __attribute__((noinline)) void vivify_test_middle(int volatile* where) {
  vivify_test_fault(where);
  asm volatile("");
}
extern "C" __attribute__((noinline)) void vivify_test_outer(int volatile* where) {
  vivify_test_middle(where);
  asm volatile("");
}

int CrashChild(std::string const& dir, int how) {
  pid_t const pid = fork();
  if (pid == 0) {
    C::Install(dir, "test");
    C::NoteLine("Vivify saber trail: 1 map trail(s) created (following the game's trail)");
    C::NoteLine("a line\nwith a newline");
    if (how == 0) vivify_test_outer(nullptr);
    if (how == 1) std::abort();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return status;
}

int main() {
  auto const dir = std::filesystem::temp_directory_path() / "vivify-crash-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  C::Install(dir.string(), "test");
  check("nothing pending at first", C::TakePendingReport().empty());

  int const status = CrashChild(dir.string(), 0);
  check("the child still dies of the signal (handed on)", WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
  std::string const report = C::TakePendingReport();
  std::printf("%s", report.c_str());
  check("a report was written", report.find("native crash") != std::string::npos);
  check("it names the signal", report.find("SIGSEGV") != std::string::npos);
  check("it gives the fault address", report.find("fault address 0x0") != std::string::npos);
  check("the faulting function is the pc frame", report.find("#00 pc") != std::string::npos &&
                                                     report.find("vivify_test_fault") != std::string::npos);
  check("the walk reaches the callers", report.find("vivify_test_middle") != std::string::npos &&
                                            report.find("vivify_test_outer") != std::string::npos);
  check("the last log lines are in it", report.find("map trail(s) created") != std::string::npos);
  check("newlines in a line are flattened", report.find("a line with a newline") != std::string::npos);
  check("it moved into VivifyCrash.txt", std::filesystem::exists(dir / "VivifyCrash.txt") &&
                                             !std::filesystem::exists(dir / "VivifyCrash.new"));

  int const aborted = CrashChild(dir.string(), 1);
  check("an abort is still an abort", WIFSIGNALED(aborted) && WTERMSIG(aborted) == SIGABRT);
  check("and is reported", C::TakePendingReport().find("SIGABRT") != std::string::npos);

  int const clean = CrashChild(dir.string(), 2);
  check("a clean exit leaves no report", WIFEXITED(clean) && C::TakePendingReport().empty());

  std::filesystem::remove_all(dir);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
