#pragma once
// Vivify's replay recorder (0.15.22, full build only).
//
// Records each single-player level to a BSOR file in Mods/bl/replays/, where
// the Replay mod finds it, so Replay can watch and render the play without
// BeatLeader installed and without uploading anything. Recording follows the
// BeatLeader Quest mod's recorder (src/Core/ReplayRecorder.cpp, MIT License,
// Copyright (c) 2022 BeatLeader); see VivifyReplayFormat.hpp for the format.
//
// It records only when Record Replays is on, the Replay mod is installed, no
// replay is being watched or rendered, and -- unless Record With BeatLeader
// is on -- BeatLeader is not installed (BeatLeader records its own).

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

DECLARE_CLASS_CODEGEN(Vivify, ReplayRecorderTicker, UnityEngine::MonoBehaviour) {
  DECLARE_DEFAULT_CTOR();
  DECLARE_SIMPLE_DTOR();
  DECLARE_INSTANCE_METHOD(void, LateUpdate);
};

namespace Vivify::ReplayRecorder {

void InstallHooks();
// Where replays are written: Mods/bl/replays/.
std::string Directory();
// "" before the first save of this session.
std::string LastSaved();

}  // namespace Vivify::ReplayRecorder
