#pragma once
// Vivify's replay recorder (0.15.22, full build only): the BSOR replay format.
//
// BSOR is BeatLeader's open replay format; the Replay mod reads it from
// Mods/bl/replays/ whether BeatLeader is installed or not, and watches and
// renders it. The data model and encoder here are ported from the BeatLeader
// Quest mod (https://github.com/BeatLeader/beatleader-qmod, MIT License,
// Copyright (c) 2022 BeatLeader): shared/Models/Replay.hpp and
// src/Models/Replay.cpp, with the upload and BeatLeader account parts left
// out. No game types, so tools/replayformat tests it on the host.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace Vivify::ReplayFormat {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

struct Info {
  std::string version, gameVersion, timestamp;
  std::string playerID, playerName, platform;
  std::string trackingSystem, hmd, controller;
  std::string hash, songName, mapper, difficulty;
  int32_t score = 0;
  std::string mode, environment, modifiers;
  float jumpDistance = 0;
  bool leftHanded = false;
  float height = 0;
  float startTime = 0, failTime = 0, speed = 0;
};

struct Transform {
  Vec3 position;
  Quat rotation;
};

struct Frame {
  float time = 0;
  int32_t fps = 0;
  Transform head, leftHand, rightHand;
};

struct NoteCutInfo {
  bool speedOK = false, directionOK = false, saberTypeOK = false, wasCutTooSoon = false;
  float saberSpeed = 0;
  // Stored in the low bit of saberSpeed, as BeatLeader does.
  bool cutDistanceToCenterPositive = false;
  Vec3 saberDir;
  int32_t saberType = 0;
  float timeDeviation = 0, cutDirDeviation = 0;
  Vec3 cutPoint, cutNormal;
  float cutDistanceToCenter = 0, cutAngle = 0;
  float beforeCutRating = 0, afterCutRating = 0;
};

enum class NoteEventType : int32_t { Good = 0, Bad = 1, Miss = 2, Bomb = 3 };

struct NoteEvent {
  int32_t noteID = 0;
  float eventTime = 0, spawnTime = 0;
  NoteEventType eventType = NoteEventType::Good;
  NoteCutInfo cut;
};

struct WallEvent {
  int32_t wallID = 0;
  float energy = 0, time = 0, spawnTime = 0;
};

struct Height {
  float height = 0, time = 0;
};

struct Pause {
  int64_t duration = 0;
  float time = 0;
};

struct SaberOffsets {
  Vec3 leftPosition;
  Quat leftRotation;
  Vec3 rightPosition;
  Quat rightRotation;
};

struct Replay {
  Info info;
  std::vector<Frame> frames;
  std::vector<NoteEvent> notes;
  std::vector<WallEvent> walls;
  std::vector<Height> heights;
  std::vector<Pause> pauses;
  SaberOffsets saberOffsets;
};

inline constexpr int32_t kMagic = 0x442d3d69;
inline constexpr char kFormatVersion = 1;

// BeatLeader's note id: scoring type, line, layer, colour and direction.
// A colour below 0 (a bomb) is written as 3.
int32_t NoteID(int scoringType, int lineIndex, int lineLayer, int colorType, int cutDirection);
// BeatLeader's wall id: line, type and width.
int32_t WallID(int lineIndex, int type, int width);
// "Easy" ... "ExpertPlus", else "Error", as BeatLeader names difficulties.
std::string DifficultyName(int difficulty);

// Writes the whole replay, little-endian, exactly as BeatLeader does:
// header, then sections 0 (info) to 7 (custom data, always empty here).
void Encode(Replay const& replay, std::ostream& out);

// A file name the Replay mod finds for this map: it looks for
// "<difficulty>-<characteristic>-<hash>" in the name. One file per attempt.
std::string FileName(Info const& info, bool failed, bool quit);

// Keeps the newest `keep` replays (by name, which ends in the timestamp)
// among the files in `names` that are for the same map, difficulty and mode
// as `fileName`; returns the ones to delete.
std::vector<std::string> NamesToPrune(std::vector<std::string> const& names, std::string const& fileName, size_t keep);

}  // namespace Vivify::ReplayFormat
