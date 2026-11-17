// Host test for src/VivifyReplayFormat.cpp: encodes a replay and reads it back
// with an independent reader written from the BSOR layout (magic, version,
// sections 0..7), then checks names and pruning.
#include "VivifyReplayFormat.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using namespace Vivify::ReplayFormat;

static int fails = 0;
static void Check(char const* what, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++fails;
}

struct Reader {
  std::string data;
  size_t at = 0;
  template <typename T> T Get() {
    T value{};
    if (at + sizeof(T) > data.size()) throw std::runtime_error("short read");
    std::memcpy(&value, data.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
  }
  std::string Str() {
    auto const n = Get<int32_t>();
    if (n < 0 || at + n > data.size()) throw std::runtime_error("bad string");
    std::string s = data.substr(at, n);
    at += n;
    return s;
  }
  void Skip(size_t n) { at += n; }
};

int main() {
  Replay r;
  r.info.version = "0.15.22";
  r.info.gameVersion = "1.40.8";
  r.info.timestamp = "1794700000";
  r.info.playerID = "VivifyLocal";
  r.info.playerName = "Player";
  r.info.hash = "ABCDEF";
  r.info.songName = "Fall-Away";
  r.info.difficulty = DifficultyName(4);
  r.info.score = 123456;
  r.info.mode = "Standard";
  r.info.modifiers = "FS,NF";
  r.info.jumpDistance = 18.5f;
  r.info.leftHanded = true;
  r.info.failTime = 42.25f;
  Frame f;
  f.time = 1.5f;
  f.fps = 90;
  f.head.position = {0.1f, 1.6f, -0.2f};
  f.leftHand.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
  r.frames = {f, f};
  NoteEvent good;
  good.noteID = NoteID(0, 1, 2, 0, 1);
  good.spawnTime = 2;
  good.eventTime = 2.01f;
  good.cut.speedOK = true;
  good.cut.saberSpeed = 3.0f;
  good.cut.cutDistanceToCenterPositive = true;
  good.cut.beforeCutRating = 1.2f;
  good.cut.afterCutRating = 0.9f;
  NoteEvent miss;
  miss.noteID = NoteID(0, 3, 0, -1, 8);
  miss.eventType = NoteEventType::Miss;
  r.notes = {good, miss};
  r.walls = {{WallID(1, 0, 2), 5.0f, 6.0f, 4.0f}};
  r.heights = {{1.7f, 0.5f}};
  r.pauses = {{12, 30.0f}};
  r.saberOffsets.rightPosition = {0, 0, 0.1f};

  std::ostringstream out;
  Encode(r, out);
  Reader in{out.str()};
  try {
    Check("magic", in.Get<int32_t>() == 0x442d3d69);
    Check("format version 1", in.Get<char>() == 1);
    Check("section 0", in.Get<char>() == 0);
    Check("version string", in.Str() == "0.15.22");
    Check("game version", in.Str() == "1.40.8");
    Check("timestamp", in.Str() == "1794700000");
    Check("player id", in.Str() == "VivifyLocal");
    Check("player name", in.Str() == "Player");
    for (int i = 0; i < 4; ++i) in.Str();  // platform, tracking, hmd, controller
    Check("hash", in.Str() == "ABCDEF");
    Check("song name", in.Str() == "Fall-Away");
    in.Str();  // mapper
    Check("difficulty name", in.Str() == "ExpertPlus");
    Check("score", in.Get<int32_t>() == 123456);
    Check("mode", in.Str() == "Standard");
    in.Str();  // environment
    Check("modifiers", in.Str() == "FS,NF");
    Check("jump distance", in.Get<float>() == 18.5f);
    Check("left handed", in.Get<char>() == 1);
    in.Get<float>();  // height
    in.Get<float>();  // start time
    Check("fail time", in.Get<float>() == 42.25f);
    in.Get<float>();  // speed
    Check("section 1", in.Get<char>() == 1);
    Check("two frames", in.Get<int32_t>() == 2);
    Check("frame time", in.Get<float>() == 1.5f);
    Check("frame fps", in.Get<int32_t>() == 90);
    Check("head y", (in.Skip(4), in.Get<float>()) == 1.6f);
    in.Skip(4 + 16 + 12 + 16 + 12 + 16);  // rest of frame 1
    in.Skip(4 + 4 + 3 * 28);             // frame 2
    Check("section 2", in.Get<char>() == 2);
    Check("two notes", in.Get<int32_t>() == 2);
    Check("note id", in.Get<int32_t>() == 21201);
    Check("event time", in.Get<float>() == 2.01f);
    Check("spawn time", in.Get<float>() == 2.0f);
    Check("good cut", in.Get<int32_t>() == 0);
    Check("speed ok", in.Get<char>() == 1);
    in.Skip(3);
    float speed = in.Get<float>();
    uint32_t bits;
    std::memcpy(&bits, &speed, 4);
    Check("cut side in the speed's low bit", (bits & 1u) == 1u);
    in.Skip(12 + 4 + 4 + 4 + 12 + 12 + 4 + 4);
    Check("before cut rating", in.Get<float>() == 1.2f);
    Check("after cut rating", in.Get<float>() == 0.9f);
    Check("bomb colour written as 3", in.Get<int32_t>() == 23038);
    in.Skip(8);
    Check("miss has no cut info", in.Get<int32_t>() == 2);
    Check("section 3", in.Get<char>() == 3);
    Check("one wall", in.Get<int32_t>() == 1);
    Check("wall id", in.Get<int32_t>() == 102);
    in.Skip(12);
    Check("section 4", in.Get<char>() == 4);
    Check("one height", in.Get<int32_t>() == 1);
    in.Skip(8);
    Check("section 5", in.Get<char>() == 5);
    Check("one pause", in.Get<int32_t>() == 1);
    Check("pause duration is 8 bytes", in.Get<int64_t>() == 12);
    Check("pause time", in.Get<float>() == 30.0f);
    Check("section 6", in.Get<char>() == 6);
    in.Skip(2 * (12 + 16));
    Check("section 7", in.Get<char>() == 7);
    Check("no custom data", in.Get<int32_t>() == 0);
    Check("nothing after", in.at == in.data.size());
  } catch (std::exception const& e) {
    Check(e.what(), false);
  }

  std::string const name = FileName(r.info, true, false);
  Check("file name", name == "VivifyLocal-fail-FallAway-ExpertPlus-Standard-ABCDEF-1794700000.bsor");
  Check("a quit is named the way Replay reads it", FileName(r.info, false, true).find("-quit-") != std::string::npos);
  Check("Replay mod finds it", name.find("ExpertPlus-Standard-ABCDEF") != std::string::npos);
  auto older = r.info;
  older.timestamp = "1794600000";
  auto other = r.info;
  other.difficulty = "Hard";
  std::vector<std::string> names = {name, FileName(older, false, false), FileName(other, false, false),
                                    "123-FallAway-ExpertPlus-Standard-ABCDEF.bsor"};
  auto prune = NamesToPrune(names, name, 1);
  Check("prunes only the older attempt of the same map", prune.size() == 1 && prune[0] == FileName(older, false, false));
  Check("keeps other players' files", NamesToPrune(names, name, 0).size() == 2);
  std::printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
