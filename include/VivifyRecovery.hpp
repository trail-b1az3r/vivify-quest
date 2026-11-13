#pragma once
// Crash recovery for Vivify maps (0.15.2).
//
// While a Vivify map plays, a small marker file records which map it is, a
// heartbeat, and each screen effect (Blit) as it starts. A map that did not
// end cleanly -- the game crashed, froze and was closed, or was closed while
// the map played -- leaves that file behind, and the next launch reads it.
// So does a map left from the pause menu soon after an effect started, when
// the game is restarted within a minute and a half (a freeze the player
// backed out of: 743 Aether's intro bokeh in 0.15.1).
//
// Such a map is put on the recovery list, and the next time it is played it
// runs in recovery mode: memory is freed before it loads, textures load at
// half resolution, map effects render at half resolution or less, and the
// effect that was starting when it went down is skipped. A Quest app cannot be
// given more memory than the system allows it, so this is how a map gets more
// room to run.
//
// The same goes for a map's own saber trail (0.15.15): Fall Away, WHY'S THIS
// DEALER and 42-flux went down within a frame of theirs being created. A map
// whose last run ended that way plays without its trail next time (the
// player's own trail stays). Unlike the rest of recovery mode, that applies
// with Crash Recovery Mode off as well, since otherwise the map cannot be
// played at all.
//
// Pure C++ (no game types), so it is exercised by tools/recovery on the host.

#include <cstdint>
#include <string>
#include <vector>

namespace Vivify::Recovery {

// Where the marker and the list live. Set once at startup; tests point it at
// a scratch directory.
void SetDirectory(std::string directory);

// Reads what the previous run left and updates the recovery list. Returns a
// line for the log describing what was found ("" when there was nothing).
// `now` is wall-clock seconds since the epoch.
std::string ProcessPreviousRun(int64_t now);

void LevelStarted(std::string const& levelPath, int64_t now);
// Every few seconds of play. Cheap: one small file rewrite.
void Heartbeat(float songTime, int64_t now);
void BlitStarted(std::string const& asset, float songTime, int64_t now);
// The map's own saber trail was just created.
void MapTrailStarted(float songTime, int64_t now);
// `finished` when the song reached its end.
void LevelEnded(bool finished, float songTime, int64_t now);

// What an entry's skippedBlit holds when the map's saber trail was to blame
// rather than an effect. Asset names never start with '<'.
inline constexpr char const* kMapTrailsEntry = "<map saber trails>";

struct Entry {
  std::string levelPath;
  int64_t when = 0;
  std::string skippedBlit;  // empty when no effect was to blame; kMapTrailsEntry for the trail
};

std::vector<Entry> Entries();
bool InRecovery(std::string const& levelPath);
// Effects to skip on this map: those that were starting when it went down.
std::vector<std::string> SkippedBlits(std::string const& levelPath);
// The map's own saber trail was being created when it went down.
bool SkipsMapTrails(std::string const& levelPath);
void Forget(std::string const& levelPath);
void ForgetAll();

}  // namespace Vivify::Recovery
