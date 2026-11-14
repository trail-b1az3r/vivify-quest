// Host tests for src/VivifyRecovery.cpp:
//   g++ -std=c++20 -I include -o /tmp/recoverytest tools/recovery/main.cpp src/VivifyRecovery.cpp && /tmp/recoverytest
#include "VivifyRecovery.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace R = Vivify::Recovery;

int failures = 0;
void check(char const* what, bool ok) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

int main() {
  auto const dir = std::filesystem::temp_directory_path() / "vivify-recovery-test";
  std::filesystem::remove_all(dir);
  R::SetDirectory(dir.string());

  // Nothing left by a previous run.
  check("a first run finds nothing", R::ProcessPreviousRun(1000).empty());

  // Crash while an effect starts: no "ended", the effect just before the last heartbeat.
  R::LevelStarted("/maps/aether", 1000);
  R::Heartbeat(1.0f, 1001);
  R::BlitStarted("assets/aether/materials/intro/introbokeh.mat", 2.07f, 1002);
  std::string const crash = R::ProcessPreviousRun(1100);
  check("a crash is reported", crash.find("did not end") != std::string::npos);
  check("the crashed map is in recovery", R::InRecovery("/maps/aether"));
  check("the effect starting at the crash is skipped",
        R::SkippedBlits("/maps/aether") == std::vector<std::string>{"assets/aether/materials/intro/introbokeh.mat"});
  check("the marker is consumed", R::ProcessPreviousRun(1200).empty());

  // An effect long before a crash is not blamed.
  R::LevelStarted("/maps/dealer", 2000);
  R::BlitStarted("assets/early.mat", 3.0f, 2003);
  R::Heartbeat(60.0f, 2060);
  R::ProcessPreviousRun(2100);
  check("a crash long after an effect still enters recovery", R::InRecovery("/maps/dealer"));
  check("but does not skip that effect", R::SkippedBlits("/maps/dealer").empty());

  // Left from the pause menu 18 s after an effect, game restarted 16 s later.
  R::LevelStarted("/maps/frozen", 3000);
  R::BlitStarted("assets/postprocess.mat", 28.7f, 3028);
  R::LevelEnded(false, 30.0f, 3046);
  R::ProcessPreviousRun(3062);
  check("a quit soon after an effect plus a quick restart enters recovery", R::InRecovery("/maps/frozen"));
  check("and skips that effect", R::SkippedBlits("/maps/frozen") == std::vector<std::string>{"assets/postprocess.mat"});

  // The same, but the game restarted much later: an ordinary quit.
  R::LevelStarted("/maps/quit", 4000);
  R::BlitStarted("assets/fx.mat", 5.0f, 4005);
  R::LevelEnded(false, 10.0f, 4010);
  check("a quit with a late restart is not a crash", R::ProcessPreviousRun(9000).empty());
  check("and the map is not in recovery", !R::InRecovery("/maps/quit"));

  // A finished map leaves nothing.
  R::LevelStarted("/maps/done", 5000);
  R::BlitStarted("assets/fx.mat", 200.0f, 5200);
  R::LevelEnded(true, 210.0f, 5210);
  check("a finished map leaves nothing", R::ProcessPreviousRun(5215).empty() && !R::InRecovery("/maps/done"));

  // The same incident is not listed twice.
  R::LevelStarted("/maps/aether", 6000);
  R::BlitStarted("assets/aether/materials/intro/introbokeh.mat", 2.07f, 6002);
  R::ProcessPreviousRun(6100);
  check("a repeated incident is listed once", R::SkippedBlits("/maps/aether").size() == 1);

  // An asset name with spaces survives the round trip.
  R::LevelStarted("/maps/spaces (x - y)", 7000);
  R::BlitStarted("assets/materials/post processing/drop effects.mat", 96.4f, 7096);
  R::ProcessPreviousRun(7200);
  check("paths and effect names with spaces survive",
        R::SkippedBlits("/maps/spaces (x - y)") ==
            std::vector<std::string>{"assets/materials/post processing/drop effects.mat"});

  // A crash a frame after the map's saber trail went on (Fall Away, 0.15.14).
  R::LevelStarted("/maps/fallaway", 8000);
  R::MapTrailStarted(0.0f, 8000);
  std::string const trailCrash = R::ProcessPreviousRun(8100);
  check("a trail made at the very start is not blamed (0.15.17)",
        !R::SkipsMapTrails("/maps/fallaway") && trailCrash.find("saber trail") == std::string::npos);
  check("and skips no effect", R::SkippedBlits("/maps/fallaway").empty());

  // An entry 0.15.16 wrote for a start-of-song trail is ignored.
  {
    std::ofstream list(dir / "RecoveryMaps.txt", std::ios::app);
    list << "/maps/aether\t1794614018\t<map saber trails>\n";
  }
  check("an old trail entry gives the trail back", !R::SkipsMapTrails("/maps/aether"));
  check("and is not taken for an effect", R::SkippedBlits("/maps/aether").empty() ||
                                              R::SkippedBlits("/maps/aether") ==
                                                  std::vector<std::string>{"assets/aether/materials/intro/introbokeh.mat"});

  // An effect, then the trail half a second later in the same second, then the crash (WHY'S THIS DEALER).
  R::LevelStarted("/maps/dealer2", 9000);
  R::BlitStarted("assets/postprocess.mat", 28.71f, 9028);
  R::MapTrailStarted(29.14f, 9028);
  R::ProcessPreviousRun(9100);
  check("of an effect and a trail, the later is blamed",
        R::SkipsMapTrails("/maps/dealer2") && R::SkippedBlits("/maps/dealer2").empty());

  // A trail long before the crash, an effect just before it: the effect.
  R::LevelStarted("/maps/later", 10000);
  R::MapTrailStarted(0.0f, 10000);
  R::Heartbeat(55.0f, 10055);
  R::BlitStarted("assets/late.mat", 58.0f, 10058);
  R::ProcessPreviousRun(10100);
  check("an old trail is not blamed", !R::SkipsMapTrails("/maps/later"));
  check("the recent effect is", R::SkippedBlits("/maps/later") == std::vector<std::string>{"assets/late.mat"});

  // A trail at the start and a clean finish: nothing.
  R::LevelStarted("/maps/fine", 11000);
  R::MapTrailStarted(0.0f, 11000);
  R::LevelEnded(true, 120.0f, 11120);
  check("a finished map with a trail leaves nothing", R::ProcessPreviousRun(11130).empty());
  check("and its trail is kept", !R::SkipsMapTrails("/maps/fine"));

  R::Forget("/maps/aether");
  check("forget one map", !R::InRecovery("/maps/aether") && R::InRecovery("/maps/frozen"));
  R::ForgetAll();
  check("forget all", R::Entries().empty());

  std::filesystem::remove_all(dir);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
