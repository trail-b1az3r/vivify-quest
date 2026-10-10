// Host driver for Vivify::GlobalLog; tools/globallog/run_tests.py runs it.
// usage: globallog <dir> <partMax> <totalMax> <lines> <lineLength> [header]
#include "VivifyGlobalLog.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 6) {
    std::cerr << "usage: globallog <dir> <partMax> <totalMax> <lines> <lineLength> [header]\n";
    return 2;
  }
  Vivify::GlobalLog log(argv[1], "vivify_global", std::strtoull(argv[2], nullptr, 10),
                        std::strtoull(argv[3], nullptr, 10));
  if (!log.Open(argc > 6 ? argv[6] : "=== session ===")) return 1;
  int const lines = std::atoi(argv[4]);
  std::string const line(static_cast<size_t>(std::atoi(argv[5])), 'x');
  for (int i = 0; i < lines; i++) log.Write(line);
  log.Flush();
  return 0;
}
