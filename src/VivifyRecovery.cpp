#include "VivifyRecovery.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Vivify::Recovery {
namespace {

std::mutex gMutex;
std::string gDirectory = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify";

// What the running level has recorded so far; rewritten whole on each update
// so a crash at any point leaves a complete file.
struct Running {
  std::string levelPath;
  int64_t started = 0;
  int64_t heartbeat = 0;
  float heartbeatSongTime = 0.0f;
  std::string lastBlit;
  int64_t lastBlitAt = 0;
  float lastBlitSongTime = 0.0f;
  int64_t mapTrailAt = 0;
  float mapTrailSongTime = 0.0f;
  int64_t ended = 0;
  bool finished = false;
  bool active = false;
};
Running gRunning;

// How close an effect's start has to be to the end of a run for the effect to
// be blamed. A freeze is seconds after the effect begins; the heartbeat comes
// every 5 s, so up to 12 s between the effect and the last sign of life.
constexpr int64_t kBlitToDeathSeconds = 12;
// A level left from the pause menu, then the game restarted: the effect has to
// have started within this long before leaving, and the restart within
// kRestartSeconds of leaving.
constexpr int64_t kBlitToQuitSeconds = 60;
constexpr int64_t kRestartSeconds = 90;

std::filesystem::path MarkerPath() { return std::filesystem::path(gDirectory) / "PlayingLevel.txt"; }
std::filesystem::path ListPath() { return std::filesystem::path(gDirectory) / "RecoveryMaps.txt"; }

void WriteMarkerLocked() {
  std::error_code ec;
  std::filesystem::create_directories(gDirectory, ec);
  std::filesystem::path const temp = MarkerPath().string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    if (!out) return;
    out << "level\t" << gRunning.levelPath << "\n";
    out << "started\t" << gRunning.started << "\n";
    out << "heartbeat\t" << gRunning.heartbeat << "\t" << gRunning.heartbeatSongTime << "\n";
    if (!gRunning.lastBlit.empty()) {
      out << "blit\t" << gRunning.lastBlitAt << "\t" << gRunning.lastBlitSongTime << "\t" << gRunning.lastBlit << "\n";
    }
    if (gRunning.mapTrailAt != 0) {
      out << "trail\t" << gRunning.mapTrailAt << "\t" << gRunning.mapTrailSongTime << "\n";
    }
    if (gRunning.ended != 0) out << "ended\t" << gRunning.ended << "\t" << (gRunning.finished ? 1 : 0) << "\n";
    out.flush();
  }
  // A rename replaces the file in one step: never half a marker on disk.
  std::filesystem::rename(temp, MarkerPath(), ec);
}

std::vector<Entry> ReadListLocked() {
  std::vector<Entry> entries;
  std::ifstream in(ListPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    Entry entry;
    std::string when;
    if (!std::getline(fields, entry.levelPath, '\t')) continue;
    if (!std::getline(fields, when, '\t')) continue;
    std::getline(fields, entry.skippedBlit);
    if (entry.skippedBlit == "-") entry.skippedBlit.clear();
    try {
      entry.when = std::stoll(when);
    } catch (...) {
      continue;
    }
    if (!entry.levelPath.empty()) entries.push_back(std::move(entry));
  }
  return entries;
}

void WriteListLocked(std::vector<Entry> const& entries) {
  std::error_code ec;
  std::filesystem::create_directories(gDirectory, ec);
  std::ofstream out(ListPath(), std::ios::trunc);
  if (!out) return;
  out << "# Vivify maps in crash recovery mode: level path, when, effect skipped\n";
  for (auto const& entry : entries) {
    out << entry.levelPath << "\t" << entry.when << "\t" << (entry.skippedBlit.empty() ? "-" : entry.skippedBlit)
        << "\n";
  }
}

}  // namespace

void SetDirectory(std::string directory) {
  std::lock_guard lock(gMutex);
  gDirectory = std::move(directory);
}

std::string ProcessPreviousRun(int64_t now) {
  std::lock_guard lock(gMutex);
  std::ifstream in(MarkerPath());
  if (!in) return "";
  Running previous;
  bool hasEnded = false;
  bool hasBlit = false;
  bool hasTrail = false;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string key;
    std::getline(fields, key, '\t');
    try {
      if (key == "level") {
        std::getline(fields, previous.levelPath);
      } else if (key == "started") {
        fields >> previous.started;
      } else if (key == "heartbeat") {
        fields >> previous.heartbeat >> previous.heartbeatSongTime;
      } else if (key == "blit") {
        fields >> previous.lastBlitAt >> previous.lastBlitSongTime;
        fields.get();  // the tab before the asset name, which may hold spaces
        std::getline(fields, previous.lastBlit);
        hasBlit = !previous.lastBlit.empty();
      } else if (key == "trail") {
        fields >> previous.mapTrailAt >> previous.mapTrailSongTime;
        hasTrail = previous.mapTrailAt != 0;
      } else if (key == "ended") {
        int finished = 0;
        fields >> previous.ended >> finished;
        previous.finished = finished != 0;
        hasEnded = true;
      }
    } catch (...) {
    }
  }
  in.close();
  std::error_code ec;
  std::filesystem::remove(MarkerPath(), ec);
  if (previous.levelPath.empty()) return "";

  std::string blamedBlit;
  std::string what;
  if (!hasEnded) {
    int64_t const lastSign = std::max(previous.heartbeat, previous.started);
    bool const blitRecent = hasBlit && previous.lastBlitAt + kBlitToDeathSeconds >= lastSign;
    bool const trailRecent = hasTrail && previous.mapTrailAt + kBlitToDeathSeconds >= lastSign;
    // When both were recent, the later of the two is blamed: a crash that
    // comes back puts the other on the list as well.
    bool const trailLater = !blitRecent || previous.mapTrailAt > previous.lastBlitAt ||
                            (previous.mapTrailAt == previous.lastBlitAt &&
                             previous.mapTrailSongTime >= previous.lastBlitSongTime);
    if (trailRecent && trailLater) {
      blamedBlit = kMapTrailsEntry;
    } else if (blitRecent) {
      blamedBlit = previous.lastBlit;
    }
    what = "did not end: the game crashed or was closed while it played";
  } else if (!previous.finished && hasBlit && previous.ended - previous.lastBlitAt <= kBlitToQuitSeconds &&
             now - previous.ended <= kRestartSeconds) {
    blamedBlit = previous.lastBlit;
    what = "was left from the pause menu soon after an effect started, and the game restarted straight after";
  } else {
    return "";
  }

  auto entries = ReadListLocked();
  bool const duplicate = std::any_of(entries.begin(), entries.end(), [&](Entry const& entry) {
    return entry.levelPath == previous.levelPath && entry.skippedBlit == blamedBlit;
  });
  if (!duplicate) {
    entries.push_back(Entry{previous.levelPath, now, blamedBlit});
    WriteListLocked(entries);
  }
  std::string message = "Vivify crash recovery: '" + previous.levelPath + "' " + what + " (last sign of life " +
                        std::to_string(static_cast<int>(previous.heartbeatSongTime)) + " s into the song)";
  if (blamedBlit == kMapTrailsEntry) {
    message += "; its own saber trail had just been created, so next time it plays without it (your own trail "
               "stays), and in recovery mode if that is on";
  } else {
    message += blamedBlit.empty() ? "; it plays in recovery mode next time"
                                  : "; it plays in recovery mode next time, without the effect '" + blamedBlit +
                                        "' that was starting when it went down";
  }
  return message;
}

void LevelStarted(std::string const& levelPath, int64_t now) {
  std::lock_guard lock(gMutex);
  gRunning = Running{};
  gRunning.levelPath = levelPath;
  gRunning.started = now;
  gRunning.heartbeat = now;
  gRunning.active = !levelPath.empty();
  if (gRunning.active) WriteMarkerLocked();
}

void Heartbeat(float songTime, int64_t now) {
  std::lock_guard lock(gMutex);
  if (!gRunning.active || gRunning.ended != 0) return;
  gRunning.heartbeat = now;
  gRunning.heartbeatSongTime = songTime;
  WriteMarkerLocked();
}

void BlitStarted(std::string const& asset, float songTime, int64_t now) {
  std::lock_guard lock(gMutex);
  if (!gRunning.active || gRunning.ended != 0 || asset.empty()) return;
  gRunning.lastBlit = asset;
  gRunning.lastBlitAt = now;
  gRunning.lastBlitSongTime = songTime;
  gRunning.heartbeat = now;
  gRunning.heartbeatSongTime = songTime;
  WriteMarkerLocked();
}

void MapTrailStarted(float songTime, int64_t now) {
  std::lock_guard lock(gMutex);
  if (!gRunning.active || gRunning.ended != 0) return;
  gRunning.mapTrailAt = now;
  gRunning.mapTrailSongTime = songTime;
  WriteMarkerLocked();
}

void LevelEnded(bool finished, float songTime, int64_t now) {
  std::lock_guard lock(gMutex);
  if (!gRunning.active) return;
  gRunning.ended = now;
  gRunning.finished = finished;
  gRunning.heartbeatSongTime = songTime;
  if (finished) {
    // Nothing to look into next launch.
    std::error_code ec;
    std::filesystem::remove(MarkerPath(), ec);
  } else {
    // Kept, so a restart straight after can be told from an ordinary quit.
    WriteMarkerLocked();
  }
  gRunning.active = false;
}

std::vector<Entry> Entries() {
  std::lock_guard lock(gMutex);
  return ReadListLocked();
}

bool InRecovery(std::string const& levelPath) {
  if (levelPath.empty()) return false;
  auto const entries = Entries();
  return std::any_of(entries.begin(), entries.end(),
                     [&](Entry const& entry) { return entry.levelPath == levelPath; });
}

std::vector<std::string> SkippedBlits(std::string const& levelPath) {
  std::vector<std::string> blits;
  for (auto const& entry : Entries()) {
    if (entry.levelPath == levelPath && !entry.skippedBlit.empty() && entry.skippedBlit != kMapTrailsEntry) {
      blits.push_back(entry.skippedBlit);
    }
  }
  return blits;
}

bool SkipsMapTrails(std::string const& levelPath) {
  if (levelPath.empty()) return false;
  auto const entries = Entries();
  return std::any_of(entries.begin(), entries.end(), [&](Entry const& entry) {
    return entry.levelPath == levelPath && entry.skippedBlit == kMapTrailsEntry;
  });
}

void Forget(std::string const& levelPath) {
  std::lock_guard lock(gMutex);
  auto entries = ReadListLocked();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](Entry const& entry) { return entry.levelPath == levelPath; }),
                entries.end());
  WriteListLocked(entries);
}

void ForgetAll() {
  std::lock_guard lock(gMutex);
  std::error_code ec;
  std::filesystem::remove(ListPath(), ec);
}

}  // namespace Vivify::Recovery
