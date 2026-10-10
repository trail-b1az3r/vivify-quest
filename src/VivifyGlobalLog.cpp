#include "VivifyGlobalLog.hpp"

#include <algorithm>

namespace Vivify {

GlobalLog::GlobalLog(std::filesystem::path directory, std::string baseName, uint64_t partMaxBytes,
                     uint64_t totalMaxBytes)
    : _directory(std::move(directory)),
      _baseName(std::move(baseName)),
      _partMaxBytes(partMaxBytes),
      _totalMaxBytes(totalMaxBytes) {}

std::filesystem::path GlobalLog::PartPath(int number) const {
  if (number <= 1) return _directory / (_baseName + ".txt");
  return _directory / (_baseName + "-p" + std::to_string(number) + ".txt");
}

std::vector<std::pair<int, std::filesystem::path>> GlobalLog::Parts() const {
  std::vector<std::pair<int, std::filesystem::path>> parts;
  std::error_code ec;
  std::string const first = _baseName + ".txt";
  std::string const prefix = _baseName + "-p";
  for (auto const& entry : std::filesystem::directory_iterator(_directory, ec)) {
    if (ec) break;
    std::string const name = entry.path().filename().string();
    if (name == first) {
      parts.emplace_back(1, entry.path());
      continue;
    }
    if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - 4, 4, ".txt") != 0) {
      continue;
    }
    std::string const digits = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    if (digits.empty() || digits.size() > 9 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    int const number = std::stoi(digits);
    if (number >= 2) parts.emplace_back(number, entry.path());
  }
  std::sort(parts.begin(), parts.end());
  return parts;
}

bool GlobalLog::OpenPart(int number) {
  if (_file.is_open()) _file.close();
  _partNumber = number;
  std::error_code ec;
  auto const path = PartPath(number);
  auto const size = std::filesystem::file_size(path, ec);
  _partBytes = ec ? 0 : size;
  _file.open(path, std::ios::out | std::ios::app | std::ios::binary);
  return _file.is_open();
}

bool GlobalLog::Open(std::string_view header) {
  std::error_code ec;
  std::filesystem::create_directories(_directory, ec);
  auto const parts = Parts();
  int number = parts.empty() ? 1 : parts.back().first;
  if (!OpenPart(number)) return false;
  if (_partBytes >= _partMaxBytes && !OpenPart(number + 1)) return false;
  PruneIfOverBudget();
  Write(header);
  Flush();
  return true;
}

void GlobalLog::Write(std::string_view line) {
  if (!_file.is_open()) return;
  _file.write(line.data(), static_cast<std::streamsize>(line.size()));
  _file.put('\n');
  _partBytes += line.size() + 1;
  if (_partBytes >= _partMaxBytes) {
    _file.flush();
    if (OpenPart(_partNumber + 1)) PruneIfOverBudget();
  }
}

void GlobalLog::Flush() {
  if (_file.is_open()) _file.flush();
}

void GlobalLog::PruneIfOverBudget() {
  auto parts = Parts();
  uint64_t total = 0;
  std::error_code ec;
  for (auto const& [number, path] : parts) {
    auto const size = std::filesystem::file_size(path, ec);
    if (!ec) total += size;
    ec.clear();
  }
  if (total <= _totalMaxBytes) return;
  // The older half, never the part being written.
  size_t remove = std::max<size_t>(1, parts.size() / 2);
  for (auto const& [number, path] : parts) {
    if (remove == 0 || number == _partNumber) break;
    std::filesystem::remove(path, ec);
    ec.clear();
    remove--;
  }
}

}
