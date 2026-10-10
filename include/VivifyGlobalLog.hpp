#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify {

// A log that is never truncated: every session appends to it (0.14.23).
//
// Parts are <base>.txt, <base>-p2.txt, <base>-p3.txt, ...; the highest-numbered
// one is written to, and once it passes partMaxBytes the next part is started.
// When all parts together pass totalMaxBytes, the older half of them (lowest
// numbers first) is deleted. Numbers are never reused, so the order of the
// parts stays the order they were written in even after old ones are gone.
//
// Not thread-safe: the caller serialises Open, Write and Flush.
class GlobalLog {
 public:
  GlobalLog(std::filesystem::path directory, std::string baseName, uint64_t partMaxBytes, uint64_t totalMaxBytes);

  // Opens the newest part for appending (or starts the first) and writes
  // header as its own line. False when nothing could be opened.
  bool Open(std::string_view header);
  void Write(std::string_view line);
  void Flush();
  bool IsOpen() const { return _file.is_open(); }

  // The path of part `number` (1 is the base name itself).
  std::filesystem::path PartPath(int number) const;
  // The parts on disk, by number, oldest first.
  std::vector<std::pair<int, std::filesystem::path>> Parts() const;

 private:
  bool OpenPart(int number);
  void PruneIfOverBudget();

  std::filesystem::path _directory;
  std::string _baseName;
  uint64_t _partMaxBytes;
  uint64_t _totalMaxBytes;
  std::ofstream _file;
  int _partNumber = 0;
  uint64_t _partBytes = 0;
};

}
