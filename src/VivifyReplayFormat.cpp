// BSOR encoding, ported from the BeatLeader Quest mod's src/Models/Replay.cpp
// and src/Utils/FileManager.cpp (MIT License, Copyright (c) 2022 BeatLeader).
// See include/VivifyReplayFormat.hpp.
#include "VivifyReplayFormat.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace Vivify::ReplayFormat {
namespace {

void Put(std::ostream& out, void const* data, size_t size) { out.write(static_cast<char const*>(data), static_cast<std::streamsize>(size)); }
void Put(std::ostream& out, char value) { Put(out, &value, 1); }
void Put(std::ostream& out, bool value) { Put(out, static_cast<char>(value ? 1 : 0)); }
void Put(std::ostream& out, int32_t value) { Put(out, &value, 4); }
void Put(std::ostream& out, int64_t value) { Put(out, &value, 8); }
void Put(std::ostream& out, float value) { Put(out, &value, 4); }
void Put(std::ostream& out, std::string const& value) {
  Put(out, static_cast<int32_t>(value.size()));
  Put(out, value.data(), value.size());
}
void Put(std::ostream& out, Vec3 const& v) { Put(out, v.x); Put(out, v.y); Put(out, v.z); }
void Put(std::ostream& out, Quat const& q) { Put(out, q.x); Put(out, q.y); Put(out, q.z); Put(out, q.w); }

// BeatLeader keeps the cut side in the lowest mantissa bit of the speed.
float WithFlag(float value, bool flag) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  bits = flag ? (bits | 1u) : (bits & ~1u);
  std::memcpy(&value, &bits, 4);
  return value;
}

void Put(std::ostream& out, Info const& info) {
  for (auto const* s : {&info.version, &info.gameVersion, &info.timestamp, &info.playerID, &info.playerName,
                        &info.platform, &info.trackingSystem, &info.hmd, &info.controller, &info.hash,
                        &info.songName, &info.mapper, &info.difficulty})
    Put(out, *s);
  Put(out, info.score);
  Put(out, info.mode);
  Put(out, info.environment);
  Put(out, info.modifiers);
  Put(out, info.jumpDistance);
  Put(out, info.leftHanded);
  Put(out, info.height);
  Put(out, info.startTime);
  Put(out, info.failTime);
  Put(out, info.speed);
}

void Put(std::ostream& out, Frame const& f) {
  Put(out, f.time);
  Put(out, f.fps);
  for (auto const* t : {&f.head, &f.leftHand, &f.rightHand}) {
    Put(out, t->position);
    Put(out, t->rotation);
  }
}

void Put(std::ostream& out, NoteEvent const& n) {
  Put(out, n.noteID);
  Put(out, n.eventTime);
  Put(out, n.spawnTime);
  Put(out, static_cast<int32_t>(n.eventType));
  if (n.eventType != NoteEventType::Good && n.eventType != NoteEventType::Bad) return;
  auto const& c = n.cut;
  Put(out, c.speedOK);
  Put(out, c.directionOK);
  Put(out, c.saberTypeOK);
  Put(out, c.wasCutTooSoon);
  Put(out, WithFlag(c.saberSpeed, c.cutDistanceToCenterPositive));
  Put(out, c.saberDir);
  Put(out, c.saberType);
  Put(out, c.timeDeviation);
  Put(out, c.cutDirDeviation);
  Put(out, c.cutPoint);
  Put(out, c.cutNormal);
  Put(out, c.cutDistanceToCenter);
  Put(out, c.cutAngle);
  Put(out, c.beforeCutRating);
  Put(out, c.afterCutRating);
}

void Put(std::ostream& out, WallEvent const& w) {
  Put(out, w.wallID);
  Put(out, w.energy);
  Put(out, w.time);
  Put(out, w.spawnTime);
}
void Put(std::ostream& out, Height const& h) { Put(out, h.height); Put(out, h.time); }
void Put(std::ostream& out, Pause const& p) { Put(out, p.duration); Put(out, p.time); }

template <typename T>
void PutList(std::ostream& out, std::vector<T> const& list) {
  Put(out, static_cast<int32_t>(list.size()));
  for (auto const& item : list) Put(out, item);
}

// BeatLeader's SanitizeFilenameComponent: printable ASCII only, path and
// shell characters to '_', runs of spaces collapsed; dashes optionally
// dropped (the song name), since '-' separates the name's fields.
std::string Sanitize(std::string const& value, bool removeDashes, std::string const& fallback) {
  static constexpr std::string_view kIllegal = "\\/:?*\"<>()|\r\n";
  std::string result;
  bool space = false;
  for (unsigned char c : value) {
    if (c < 32 || c >= 127) continue;
    if (removeDashes && c == '-') continue;
    if (c == ' ' || c == '\t') {
      space = !result.empty();
      continue;
    }
    if (space) result.push_back(' ');
    space = false;
    result.push_back(kIllegal.find(static_cast<char>(c)) != std::string_view::npos ? '_' : static_cast<char>(c));
  }
  return result.empty() ? fallback : result;
}

std::string MapKey(Info const& info) {
  return "-" + Sanitize(info.difficulty, false, "UnknownDifficulty") + "-" + Sanitize(info.mode, false, "UnknownMode") +
         "-" + Sanitize(info.hash, false, "UnknownHash") + "-";
}

// The timestamp a name ends in (seconds), or 0.
long long Timestamp(std::string const& name) {
  auto const dot = name.rfind(".bsor");
  auto const dash = name.rfind('-', dot);
  if (dot == std::string::npos || dash == std::string::npos) return 0;
  try {
    return std::stoll(name.substr(dash + 1, dot - dash - 1));
  } catch (...) {
    return 0;
  }
}

constexpr std::string_view kPlayerPrefix = "VivifyLocal";

}  // namespace

int32_t NoteID(int scoringType, int lineIndex, int lineLayer, int colorType, int cutDirection) {
  if (colorType < 0) colorType = 3;
  return (scoringType + 2) * 10000 + lineIndex * 1000 + lineLayer * 100 + colorType * 10 + cutDirection;
}

int32_t WallID(int lineIndex, int type, int width) { return lineIndex * 100 + type * 10 + width; }

std::string DifficultyName(int difficulty) {
  static char const* const kNames[] = {"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
  return difficulty >= 0 && difficulty < 5 ? kNames[difficulty] : "Error";
}

void Encode(Replay const& replay, std::ostream& out) {
  Put(out, kMagic);
  Put(out, kFormatVersion);
  Put(out, static_cast<char>(0)); Put(out, replay.info);
  Put(out, static_cast<char>(1)); PutList(out, replay.frames);
  Put(out, static_cast<char>(2)); PutList(out, replay.notes);
  Put(out, static_cast<char>(3)); PutList(out, replay.walls);
  Put(out, static_cast<char>(4)); PutList(out, replay.heights);
  Put(out, static_cast<char>(5)); PutList(out, replay.pauses);
  Put(out, static_cast<char>(6));
  Put(out, replay.saberOffsets.leftPosition);
  Put(out, replay.saberOffsets.leftRotation);
  Put(out, replay.saberOffsets.rightPosition);
  Put(out, replay.saberOffsets.rightRotation);
  Put(out, static_cast<char>(7)); Put(out, static_cast<int32_t>(0));
}

std::string FileName(Info const& info, bool failed, bool exited) {
  std::string name(kPlayerPrefix);
  if (info.speed > 0.0001f) name += "-practice";
  if (failed) name += "-fail";
  // Replay reads "practice", "fail" and "quit" from the name (BeatLeader
  // writes "-exit", which Replay does not recognise as a quit).
  if (exited) name += "-quit";
  name += "-" + Sanitize(info.songName, true, "UnknownSong");
  name += MapKey(info);
  name += Sanitize(info.timestamp, false, "0") + ".bsor";
  return name;
}

std::vector<std::string> NamesToPrune(std::vector<std::string> const& names, std::string const& fileName, size_t keep) {
  // The map key is the part between the song name and the timestamp.
  auto const dot = fileName.rfind(".bsor");
  auto const lastDash = fileName.rfind('-', dot);
  if (dot == std::string::npos || lastDash == std::string::npos) return {};
  // "-<diff>-<mode>-<hash>-": the three dashes before the timestamp's.
  size_t start = lastDash;
  for (int i = 0; i < 3 && start != std::string::npos && start > 0; ++i) start = fileName.rfind('-', start - 1);
  if (start == std::string::npos) return {};
  std::string const key = fileName.substr(start, lastDash - start + 1);
  std::vector<std::string> same;
  for (auto const& name : names)
    if (name.rfind(kPlayerPrefix, 0) == 0 && name.size() > 5 && name.ends_with(".bsor") &&
        name.find(key) != std::string::npos)
      same.push_back(name);
  std::sort(same.begin(), same.end(), [](auto const& a, auto const& b) { return Timestamp(a) > Timestamp(b); });
  if (same.size() <= keep) return {};
  return {same.begin() + static_cast<std::ptrdiff_t>(keep), same.end()};
}

}  // namespace Vivify::ReplayFormat
