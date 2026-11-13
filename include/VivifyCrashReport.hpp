#pragma once
// A record of a native crash (0.15.15).
//
// The mod's logs say what Vivify was doing, not which native call faulted, and
// up to a quarter of a second of lines can still be in the buffer when the
// process dies. When the game takes a fatal signal this writes, before the
// game's own crash handling runs:
//   - the signal, the fault address and the thread;
//   - the program counter and a frame-pointer walk of the stack, each frame as
//     library + offset (and the nearest exported symbol), which the release
//     library's symbol table turns into a function name;
//   - the last Vivify log lines, including any not yet written to the logs.
// to VivifyCrash.new, which the next launch copies into the global log and
// appends to VivifyCrash.txt. The handler then hands the signal to whatever
// handler was there before, so the game's crash reporting is unchanged.

#include <string>
#include <string_view>

namespace Vivify::CrashReport {

// Takes the fatal signals, keeping the previous handlers to hand on to.
// `directory` is where the report files go.
void Install(std::string const& directory, std::string_view version);
// Takes them again if another library installed its own since (the game's
// engine can, after mods load). Cheap; called at each level start.
void Reinstall();
// One log line for the report's tail. Called by the log sink, under its lock.
void NoteLine(std::string_view line);
// The report a crash in the previous run left ("" if none). Moves it into
// VivifyCrash.txt.
std::string TakePendingReport();

}  // namespace Vivify::CrashReport
