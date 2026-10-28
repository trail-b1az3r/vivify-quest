#include "VivifyReport.hpp"
#include "VivifyGlobalLog.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace Vivify::Report {
namespace {

std::mutex& FileMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& PathStorage() {
  static std::string path =
      "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt";
  return path;
}

std::string& LastErrorStorage() {
  static std::string error;
  return error;
}

std::string Timestamp() {
  auto const now = std::chrono::system_clock::now();
  auto const seconds = std::chrono::system_clock::to_time_t(now);
  std::tm parts{};
#if defined(_WIN32)
  localtime_s(&parts, &seconds);
#else
  localtime_r(&seconds, &parts);
#endif
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
    return "unknown-time";
  }
  return buffer;
}

// Drops the oldest blocks once the file gets large, so leaving the mod
// installed for months cannot fill a headset. The cut lands on a line boundary
// so the surviving text still reads cleanly.
void TrimIfLarge(std::filesystem::path const& path) {
  std::error_code ec;
  auto const size = std::filesystem::file_size(path, ec);
  if (ec || size <= kMaxBytes) return;

  std::ifstream input(path, std::ios::binary);
  if (!input) return;
  input.seekg(static_cast<std::streamoff>(size - kKeepBytes), std::ios::beg);
  if (!input) return;
  std::string kept((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  input.close();

  auto const firstNewline = kept.find('\n');
  if (firstNewline != std::string::npos) kept.erase(0, firstNewline + 1);

  // Written beside the target and renamed, so an interrupted trim cannot leave
  // the report truncated to nothing.
  auto const temporary = std::filesystem::path(path).concat(".trim");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "[earlier entries dropped to keep this file small]\n" << kept;
    if (!output) return;
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) std::filesystem::remove(temporary, ec);
}

}  // namespace

std::string const& FilePath() {
  return PathStorage();
}

void SetFilePathForTesting(std::string path) {
  std::lock_guard<std::mutex> lock(FileMutex());
  PathStorage() = std::move(path);
}

std::string LastError() {
  std::lock_guard<std::mutex> lock(FileMutex());
  return LastErrorStorage();
}

bool Append(std::string_view title, std::string_view body) {
  // A diagnostic file must never be the reason the game breaks, so no
  // exception escapes into Unity. A failure is returned and its reason kept
  // (LastError) so the caller can say so in the session log; before 0.14.29 it
  // was dropped without a trace, and a report that never appeared looked the
  // same as one nobody had looked for.
  try {
    std::lock_guard<std::mutex> lock(FileMutex());
    std::filesystem::path const path(PathStorage());

    std::error_code ec;
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        LastErrorStorage() = "cannot create '" + path.parent_path().string() + "': " + ec.message();
        return false;
      }
    }
    TrimIfLarge(path);

    std::ofstream output;
    if (!OpenAtEnd(output, path, LastErrorStorage())) return false;
    output << "\n================================================================\n"
           << "[" << Timestamp() << "] " << title << "\n"
           << "----------------------------------------------------------------\n"
           << body;
    if (!body.empty() && body.back() != '\n') output << "\n";
    output.flush();
    if (!output) {
      LastErrorStorage() = "write to '" + path.string() + "' failed";
      return false;
    }
    LastErrorStorage().clear();
    return true;
  } catch (std::exception const& e) {
    LastErrorStorage() = std::string("threw: ") + e.what();
    return false;
  } catch (...) {
    LastErrorStorage() = "threw";
    return false;
  }
}

}  // namespace Vivify::Report
