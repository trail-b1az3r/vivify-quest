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
// The same goes for a map's own saber trail created after the song started
// (0.15.15; only after the start since 0.15.17). A map whose last run ended
// within seconds of that plays without its trail next time (the player's own
// trail stays). Unlike the rest of recovery mode, that applies
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
// A prefab with a realtime reflection probe was just spawned, with realtime
// probes allowed (0.15.28).
void ReflectionProbesStarted(int64_t now);
// `finished` when the song reached its end.
void LevelEnded(bool finished, float songTime, int64_t now);

// What an entry's skippedBlit holds when the map's saber trail was to blame
// rather than an effect. Asset names never start with '<'.
// 0.15.15 and 0.15.16 also blamed a trail created at the very start, where
// everything else is created in the same moment, and a map then lost its
// trail for nothing (743 Aether). Their entries used another name and are
// ignored.
inline constexpr char const* kMapTrailsEntry = "<map saber trail after start>";
inline constexpr char const* kOldMapTrailsEntry = "<map saber trails>";
// Realtime reflection probes were on when the map went down with nothing else
// to blame (0.15.28): Passing Memories crashed three times in a row, and RSIH
// once, inside Unity's worker thread (libunity +0x13ee740) right after a
// prefab with a realtime probe spawned. The map then plays with realtime
// reflection probes off, even with Crash Recovery Mode off.
inline constexpr char const* kReflectionProbesEntry = "<realtime reflection probes>";

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
// Realtime reflection probes were on when it went down (see above).
bool SkipsReflectionProbes(std::string const& levelPath);
void Forget(std::string const& levelPath);
void ForgetAll();

}  // namespace Vivify::Recovery
