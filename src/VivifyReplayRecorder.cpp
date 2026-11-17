// Vivify's replay recorder (0.15.22, full build only). See
// include/VivifyReplayRecorder.hpp.
//
// The hooks and what they record follow the BeatLeader Quest mod's
// src/Core/ReplayRecorder.cpp (https://github.com/BeatLeader/beatleader-qmod,
// MIT License, Copyright (c) 2022 BeatLeader). Left out: uploading, the
// BeatLeader account and its player id, dance mode, multiplayer, and the
// saber offsets section (written as identity, which older replays also have).
#include "main.hpp"

#include "VivifyReplayFormat.hpp"
#include "VivifyReplayRecorder.hpp"

#if __has_include(<sys/system_properties.h>)
#include <sys/system_properties.h>  // bionic; absent from the host syntax check
#define VIVIFY_HAS_SYSTEM_PROPERTIES 1
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "custom-types/shared/delegate.hpp"
#include "scotland2/shared/modloader.h"

#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "GlobalNamespace/BadCutScoringElement.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/BeatmapObjectSpawnController.hpp"
#include "GlobalNamespace/CutScoreBuffer.hpp"
#include "GlobalNamespace/EnvironmentName.hpp"
#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/GoodCutScoringElement.hpp"
#include "GlobalNamespace/IVariableMovementDataProvider.hpp"
#include "GlobalNamespace/LevelCompletionResults.hpp"
#include "GlobalNamespace/MissScoringElement.hpp"
#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"
#include "GlobalNamespace/NoteData.hpp"
#include "GlobalNamespace/NoteSpawnData.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/ObstacleData.hpp"
#include "GlobalNamespace/ObstacleSpawnData.hpp"
#include "GlobalNamespace/PauseMenuManager.hpp"
#include "GlobalNamespace/PlayerHeadAndObstacleInteraction.hpp"
#include "GlobalNamespace/PlayerHeightDetector.hpp"
#include "GlobalNamespace/PlayerSpecificSettings.hpp"
#include "GlobalNamespace/PlayerTransforms.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "GlobalNamespace/SaberManager.hpp"
#include "GlobalNamespace/SaberMovementData.hpp"
#include "GlobalNamespace/SaberSwingRating.hpp"
#include "GlobalNamespace/SaberSwingRatingCounter.hpp"
#include "GlobalNamespace/ScoreController.hpp"
#include "GlobalNamespace/ScoringElement.hpp"
#include "GlobalNamespace/StandardLevelScenesTransitionSetupDataSO.hpp"
#include "System/Action_1.hpp"
#include "System/Collections/Generic/HashSet_1.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "UnityEngine/Application.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "Zenject/DiContainer.hpp"

DEFINE_TYPE(Vivify, ReplayRecorderTicker);

using namespace GlobalNamespace;
namespace RF = Vivify::ReplayFormat;

namespace Vivify::ReplayRecorder {
namespace {

// At most this many replays are kept per map, difficulty and mode; older
// attempts are deleted when a new one is saved.
constexpr size_t kKeepPerMap = 10;
// A quit or restart this early in the song is not kept.
constexpr float kMinQuitSongTime = 5.0f;

std::optional<RF::Replay> replay;

AudioTimeSyncController* audio = nullptr;
PlayerTransforms* playerTransforms = nullptr;
SaberManager* saberManager = nullptr;
PlayerHeadAndObstacleInteraction* headObstacles = nullptr;
bool automaticHeight = false;

std::map<NoteData*, int> noteIds;                  // note -> our spawn id
std::map<int, RF::NoteEvent> noteEvents;           // spawn id -> event so far
std::map<int, NoteController*> noteControllers;    // spawn id -> controller
std::map<int, NoteCutInfo> noteCuts;               // spawn id -> the game's cut info
std::map<NoteData*, size_t> replayNoteIndex;       // note -> index in replay->notes
std::map<SaberMovementData*, float> preSwing;
std::map<SaberSwingRatingCounter*, float> postSwing;
int nextNoteId = 0;

std::map<ObstacleController*, RF::WallEvent> wallEvents;
bool inWall = false;

std::optional<RF::Pause> currentPause;
std::chrono::steady_clock::time_point pauseStarted;

std::mutex lastSavedMutex;
std::string lastSaved;

std::string hmd = "Unknown";

void Reset() {
  replay.reset();
  audio = nullptr;
  playerTransforms = nullptr;
  saberManager = nullptr;
  headObstacles = nullptr;
  noteIds.clear();
  noteEvents.clear();
  noteControllers.clear();
  noteCuts.clear();
  replayNoteIndex.clear();
  preSwing.clear();
  postSwing.clear();
  nextNoteId = 0;
  wallEvents.clear();
  inWall = false;
  currentPause.reset();
}

bool ModLoaded(char const* id) { return LoadedModHandle(id) != nullptr; }

// Why a level is not recorded, or nullptr when it is.
char const* SkipReason() {
  if constexpr (kLiteBuild) return "lite build";
  if (!GetRecordReplays()) return "Record Replays is off";
  if (!ModLoaded("replay")) return "the Replay mod is not installed";
  if (GetReplayPlaying()) return "a replay is being watched or rendered";
  if (ModLoaded("bl") && !GetRecordReplaysWithBeatLeader())
    return "BeatLeader is installed and records its own (Record With BeatLeader is off)";
  return nullptr;
}

RF::Vec3 ToVec(UnityEngine::Vector3 v) { return {v.x, v.y, v.z}; }
RF::Quat ToQuat(UnityEngine::Quaternion q) { return {q.x, q.y, q.z, q.w}; }

// A pose relative to the player's origin, as BeatLeader records it.
RF::Transform Local(UnityEngine::Transform* origin, UnityEngine::Transform* target) {
  RF::Transform result;
  result.position = ToVec(origin->InverseTransformPoint(target->get_position()));
  result.rotation = ToQuat(UnityEngine::Quaternion::op_Multiply(UnityEngine::Quaternion::Inverse(origin->get_rotation()),
                                                                target->get_rotation()));
  return result;
}

void FillCut(RF::NoteCutInfo& out, NoteCutInfo const& cut, NoteController* controller) {
  out.speedOK = cut.speedOK;
  out.directionOK = cut.directionOK;
  out.saberTypeOK = cut.saberTypeOK;
  out.wasCutTooSoon = cut.wasCutTooSoon;
  out.saberSpeed = cut.saberSpeed;
  out.saberDir = ToVec(cut.saberDir);
  out.saberType = static_cast<int>(cut.saberType);
  out.timeDeviation = cut.timeDeviation;
  out.cutDirDeviation = cut.cutDirDeviation;
  out.cutPoint = ToVec(cut.cutPoint);
  out.cutNormal = ToVec(cut.cutNormal);
  out.cutDistanceToCenter = cut.cutDistanceToCenter;
  out.cutAngle = cut.cutAngle;
  if (controller != nullptr && controller->get_noteTransform() != nullptr) {
    auto const toPoint = UnityEngine::Vector3::op_Subtraction(cut.cutPoint, controller->get_noteTransform()->get_position());
    out.cutDistanceToCenterPositive = UnityEngine::Vector3::Dot(cut.cutNormal, toPoint) <= 0;
  }
}

float ChooseSwingRating(float real, float unclamped) { return real < 1 ? real : std::max(real, unclamped); }

void ScoringElementFinished(ScoringElement* element) {
  if (!replay || element == nullptr) return;
  auto* noteData = element->get_noteData();
  auto index = replayNoteIndex.find(noteData);
  if (index == replayNoteIndex.end() || index->second >= replay->notes.size()) return;
  int const id = noteIds[noteData];
  auto& event = replay->notes[index->second];
  bool const bomb = noteData->get_colorType() == ColorType::None;

  if (il2cpp_utils::try_cast<MissScoringElement>(element)) {
    if (!bomb) event.eventType = RF::NoteEventType::Miss;
  } else if (il2cpp_utils::try_cast<BadCutScoringElement>(element)) {
    event.eventType = bomb ? RF::NoteEventType::Bomb : RF::NoteEventType::Bad;
    if (noteCuts.contains(id)) FillCut(event.cut, noteCuts[id], noteControllers[id]);
  } else if (auto good = il2cpp_utils::try_cast<GoodCutScoringElement>(element)) {
    auto* buffer = good.value()->_cutScoreBuffer;
    if (buffer == nullptr) return;
    auto* counter = buffer->_saberSwingRatingCounter;
    FillCut(event.cut, buffer->get_noteCutInfo(), noteControllers[id]);
    if (counter != nullptr) {
      auto* movement = reinterpret_cast<SaberMovementData*>(counter->_saberMovementData);
      event.cut.beforeCutRating = ChooseSwingRating(counter->get_beforeCutRating(), preSwing[movement]);
      event.cut.afterCutRating = ChooseSwingRating(counter->get_afterCutRating(), postSwing[counter]);
      preSwing[movement] = 0;
      postSwing[counter] = 0;
    }
  }
}

void HeadEnteredObstacle(UnityW<ObstacleController> obstacle) {
  if (!replay || inWall || audio == nullptr) return;
  auto found = wallEvents.find(obstacle.unsafePtr());
  if (found == wallEvents.end()) return;
  auto wall = found->second;
  wall.time = audio->get_songTime();
  replay->walls.push_back(wall);
  inWall = true;
}

void FetchHmd() {
#ifdef VIVIFY_HAS_SYSTEM_PROPERTIES
  // __system_property_read_callback needs API 26; the mod targets 24.
  if (auto const* prop = __system_property_find("ro.product.model")) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_read(prop, nullptr, value) > 0) hmd = value;
  }
#endif
}

void Save(RF::Replay finished, bool failed, bool quit) {
  std::thread([finished = std::move(finished), failed, quit]() {
    try {
      std::string const dir = Directory();
      std::filesystem::create_directories(dir);
      std::string const name = RF::FileName(finished.info, failed, quit);
      std::string const path = dir + name;
      {
        std::ofstream out(path + ".tmp", std::ios::binary | std::ios::trunc);
        RF::Encode(finished, out);
        out.flush();
        if (!out) throw std::runtime_error("write failed");
      }
      std::filesystem::rename(path + ".tmp", path);
      std::vector<std::string> names;
      for (auto const& entry : std::filesystem::directory_iterator(dir)) names.push_back(entry.path().filename().string());
      for (auto const& old : RF::NamesToPrune(names, name, kKeepPerMap)) {
        std::error_code ec;
        std::filesystem::remove(dir + old, ec);
      }
      {
        std::lock_guard lock(lastSavedMutex);
        lastSaved = path;
      }
      PaperLogger.info("Vivify replay recorder: saved {} ({} frames, {} notes, {} walls, {} pauses)", path,
                       finished.frames.size(), finished.notes.size(), finished.walls.size(), finished.pauses.size());
    } catch (std::exception const& ex) {
      PaperLogger.error("Vivify replay recorder: could not save the replay: {}", ex.what());
    } catch (...) {
      PaperLogger.error("Vivify replay recorder: could not save the replay");
    }
  }).detach();
}

std::string Modifiers(GameplayModifiers* m, float energy) {
  if (m == nullptr) return "";
  std::vector<char const*> list;
  if (m->get_disappearingArrows()) list.push_back("DA");
  if (m->get_songSpeed() == GameplayModifiers::SongSpeed::Faster) list.push_back("FS");
  if (m->get_songSpeed() == GameplayModifiers::SongSpeed::Slower) list.push_back("SS");
  if (m->get_songSpeed() == GameplayModifiers::SongSpeed::SuperFast) list.push_back("SF");
  if (m->get_ghostNotes()) list.push_back("GN");
  if (m->get_noArrows()) list.push_back("NA");
  if (m->get_noBombs()) list.push_back("NB");
  if (m->get_noFailOn0Energy() && energy == 0) list.push_back("NF");
  if (m->get_enabledObstacleType() == GameplayModifiers::EnabledObstacleType::NoObstacles) list.push_back("NO");
  if (m->get_strictAngles()) list.push_back("SA");
  if (m->get_proMode()) list.push_back("PM");
  if (m->get_smallCubes()) list.push_back("SC");
  if (m->get_failOnSaberClash()) list.push_back("CS");
  if (m->get_instaFail()) list.push_back("IF");
  if (m->get_energyType() == GameplayModifiers::EnergyType::Battery) list.push_back("BE");
  std::string joined;
  for (auto const* item : list) joined += (joined.empty() ? "" : ",") + std::string(item);
  return joined;
}

// ---- hooks ----------------------------------------------------------------

MAKE_HOOK_MATCH(Recorder_InstallBindings, &GameplayCoreInstaller::InstallBindings, void, GameplayCoreInstaller* self) {
  Recorder_InstallBindings(self);
  try {
    Reset();
    if (auto const* reason = SkipReason()) {
      PaperLogger.info("Vivify replay recorder: not recording this level ({})", reason);
      return;
    }
    auto* container = self->get_Container();
    playerTransforms = reinterpret_cast<PlayerTransforms*>(container->Resolve(csTypeOf(PlayerTransforms*)));
    saberManager = reinterpret_cast<SaberManager*>(container->Resolve(csTypeOf(SaberManager*)));
    audio = reinterpret_cast<AudioTimeSyncController*>(container->Resolve(csTypeOf(AudioTimeSyncController*)));
    if (playerTransforms == nullptr || saberManager == nullptr || audio == nullptr) {
      PaperLogger.warn("Vivify replay recorder: gameplay objects not found; not recording this level");
      Reset();
      return;
    }
    if (auto* setup = self->_sceneSetupData; setup && setup->playerSpecificSettings)
      automaticHeight = setup->playerSpecificSettings->automaticPlayerHeight;
    RF::Replay fresh;
    fresh.info.version = std::string("Vivify ") + VERSION;
    fresh.info.gameVersion = std::string(UnityEngine::Application::get_version());
    fresh.info.timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    fresh.info.playerID = "VivifyLocal";
    fresh.info.playerName = "Player";
    fresh.info.platform = "oculus";
    fresh.info.trackingSystem = "Oculus";
    fresh.info.hmd = hmd;
    fresh.info.controller = "Oculus Touch";
    replay = std::move(fresh);
    self->get_gameObject()->AddComponent<Vivify::ReplayRecorderTicker*>();
    PaperLogger.info("Vivify replay recorder: recording this level");
  } catch (std::exception const& ex) {
    PaperLogger.error("Vivify replay recorder: start failed: {}", ex.what());
    Reset();
  }
}

MAKE_HOOK_MATCH(Recorder_HeightLateUpdate, &PlayerHeightDetector::LateUpdate, void, PlayerHeightDetector* self) {
  Recorder_HeightLateUpdate(self);
  if (replay && audio != nullptr && automaticHeight)
    replay->heights.push_back({self->_lastReportedHeight, audio->get_songTime()});
}

MAKE_HOOK_MATCH(Recorder_SpawnNote, &BeatmapObjectManager::AddSpawnedNoteController, void, BeatmapObjectManager* self,
                NoteController* controller, NoteSpawnData spawnData) {
  Recorder_SpawnNote(self, controller, spawnData);
  if (!replay || controller == nullptr) return;
  auto* data = controller->get_noteData();
  if (data == nullptr) return;
  int const id = ++nextNoteId;
  noteIds[data] = id;
  noteControllers[id] = controller;
  RF::NoteEvent event;
  event.noteID = RF::NoteID(static_cast<int>(data->get_scoringType()), data->get_lineIndex(),
                            static_cast<int>(data->get_noteLineLayer()), static_cast<int>(data->get_colorType()),
                            static_cast<int>(data->get_cutDirection()));
  event.spawnTime = data->get_time();
  noteEvents[id] = event;
}

MAKE_HOOK_MATCH(Recorder_SpawnObstacle, &BeatmapObjectManager::AddSpawnedObstacleController, void,
                BeatmapObjectManager* self, ObstacleController* controller, ObstacleSpawnData spawnData) {
  Recorder_SpawnObstacle(self, controller, spawnData);
  if (!replay || audio == nullptr || controller == nullptr) return;
  auto* data = controller->get_obstacleData();
  if (data == nullptr) return;
  RF::WallEvent wall;
  wall.wallID = RF::WallID(data->get_lineIndex(), static_cast<int>(data->get_type()), data->get_width());
  wall.spawnTime = audio->get_songTime();
  wallEvents[controller] = wall;
}

MAKE_HOOK_MATCH(Recorder_NoteWasCut, &BeatmapObjectManager::HandleNoteControllerNoteWasCut, void,
                BeatmapObjectManager* self, NoteController* controller, ByRef<NoteCutInfo> cutInfo) {
  Recorder_NoteWasCut(self, controller, cutInfo);
  if (!replay || controller == nullptr) return;
  auto found = noteIds.find(controller->get_noteData());
  if (found != noteIds.end()) noteCuts[found->second] = cutInfo.heldRef;
}

// The note events go into the replay in the order the game scores them,
// with the time it does so, as BeatLeader's recorder does.
MAKE_HOOK_MATCH(Recorder_ScoreLateUpdate, &ScoreController::LateUpdate, void, ScoreController* self) {
  if (replay && self->_sortedScoringElementsWithoutMultiplier && self->_sortedNoteTimesWithoutScoringElements &&
      self->_audioTimeSyncController) {
    float const songTime = self->_audioTimeSyncController->get_songTime();
    auto* times = self->_sortedNoteTimesWithoutScoringElements;
    float const nearestUncut = times->get_Count() > 0 ? times->get_Item(0) : 10000000.0f;
    float const skipAfter = songTime + 0.15f;
    auto* elements = self->_sortedScoringElementsWithoutMultiplier;
    for (int i = 0; i < elements->get_Count(); ++i) {
      auto* element = elements->get_Item(i);
      if (element->get_time() >= skipAfter && element->get_time() <= nearestUncut) break;
      auto* noteData = element->get_noteData();
      if (il2cpp_utils::try_cast<MissScoringElement>(element) &&
          (noteData->get_colorType() == ColorType::None || noteData->get_scoringType() == NoteData::ScoringType::NoScore))
        continue;
      auto id = noteIds.find(noteData);
      if (id == noteIds.end()) continue;
      auto event = noteEvents.find(id->second);
      if (event == noteEvents.end()) continue;
      event->second.eventTime = songTime;
      replay->notes.push_back(event->second);
      replayNoteIndex[noteData] = replay->notes.size() - 1;
    }
  }
  Recorder_ScoreLateUpdate(self);
}

MAKE_HOOK_MATCH(Recorder_ScoreStart, &ScoreController::Start, void, ScoreController* self) {
  Recorder_ScoreStart(self);
  if (!replay) return;
  self->add_scoringForNoteFinishedEvent(custom_types::MakeDelegate<System::Action_1<ScoringElement*>*>(
      std::function<void(ScoringElement*)>(ScoringElementFinished)));
  headObstacles = self->_playerHeadAndObstacleInteraction;
  if (headObstacles != nullptr)
    headObstacles->add_headDidEnterObstacleEvent(custom_types::MakeDelegate<System::Action_1<UnityW<ObstacleController>>*>(
        std::function<void(UnityW<ObstacleController>)>(HeadEnteredObstacle)));
  if (self->_audioTimeSyncController) audio = self->_audioTimeSyncController;
}

// The swing ratings without the game's clamp to 1, as BeatLeader records them.
MAKE_HOOK_MATCH(Recorder_ComputeSwingRating,
                static_cast<float (SaberMovementData::*)(bool, float)>(&SaberMovementData::ComputeSwingRating), float,
                SaberMovementData* self, bool overrideSegmentAngle, float overrideValue) {
  float const result = Recorder_ComputeSwingRating(self, overrideSegmentAngle, overrideValue);
  if (!replay) return result;
  auto data = self->_data;
  int const length = data.size();
  int const valid = self->_validCount;
  if (length == 0 || valid == 0) return result;
  int index = self->_nextAddIndex - 1;
  if (index < 0) index += length;
  float const startTime = data[index].time;
  float time = startTime;
  auto const normal = data[index].segmentNormal;
  float const angle = overrideSegmentAngle ? overrideValue : data[index].segmentAngle;
  float rating = SaberSwingRating::BeforeCutStepRating(angle, 0.0f);
  for (int i = 2; startTime - time < 0.4 && i < valid; ++i) {
    if (--index < 0) index += length;
    float const normalDiff = UnityEngine::Vector3::Angle(data[index].segmentNormal, normal);
    if (normalDiff > 90.0f) break;
    rating += SaberSwingRating::BeforeCutStepRating(data[index].segmentAngle, normalDiff);
    time = data[index].time;
  }
  preSwing[self] = rating;
  return result;
}

MAKE_HOOK_MATCH(Recorder_ProcessSwing, &SaberSwingRatingCounter::ProcessNewData, void, SaberSwingRatingCounter* self,
                BladeMovementDataElement newData, BladeMovementDataElement prevData, bool prevDataAreValid) {
  bool const alreadyCut = self->_notePlaneWasCut;
  Recorder_ProcessSwing(self, newData, prevData, prevDataAreValid);
  if (!replay) return;
  float swing = postSwing[self];
  if (!alreadyCut && !self->_notePlane.SameSide(newData.topPos, prevData.topPos)) {
    float const angle = UnityEngine::Vector3::Angle(
        UnityEngine::Vector3::op_Subtraction(self->_cutTopPos, self->_cutBottomPos),
        UnityEngine::Vector3::op_Subtraction(self->_afterCutTopPos, self->_afterCutBottomPos));
    if (self->_rateAfterCut) swing = SaberSwingRating::AfterCutStepRating(angle, 0.0f);
  } else {
    float const normalDiff = UnityEngine::Vector3::Angle(newData.segmentNormal, self->_cutPlaneNormal);
    if (self->_rateAfterCut) swing += SaberSwingRating::AfterCutStepRating(newData.segmentAngle, normalDiff);
  }
  postSwing[self] = swing;
}

MAKE_HOOK_MATCH(Recorder_SpawnControllerStart, &BeatmapObjectSpawnController::Start, void,
                BeatmapObjectSpawnController* self) {
  Recorder_SpawnControllerStart(self);
  if (!replay) return;
  if (self->_variableMovementDataProvider) replay->info.jumpDistance = self->_variableMovementDataProvider->get_jumpDistance();
  currentPause.reset();
  inWall = false;
}

MAKE_HOOK_MATCH(Recorder_ShowPause, &PauseMenuManager::ShowMenu, void, PauseMenuManager* self) {
  Recorder_ShowPause(self);
  if (!replay || audio == nullptr) return;
  currentPause = RF::Pause{0, audio->get_songTime()};
  pauseStarted = std::chrono::steady_clock::now();
}

MAKE_HOOK_MATCH(Recorder_Resume, &PauseMenuManager::HandleResumeFromPauseAnimationDidFinish, void,
                PauseMenuManager* self) {
  Recorder_Resume(self);
  if (!replay || !currentPause) return;
  currentPause->duration =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - pauseStarted).count();
  replay->pauses.push_back(*currentPause);
  currentPause.reset();
}

MAKE_HOOK_MATCH(Recorder_Finish, &StandardLevelScenesTransitionSetupDataSO::Finish, void,
                StandardLevelScenesTransitionSetupDataSO* self, LevelCompletionResults* results) {
  Recorder_Finish(self, results);
  if (!replay) return;
  auto finished = std::move(*replay);
  float const songTime = audio != nullptr ? audio->get_songTime() : 0.0f;
  Reset();
  try {
    if (results == nullptr) return;
    if (GetReplayPlaying()) return;  // a replay that started after recording began
    using EndState = LevelCompletionResults::LevelEndStateType;
    using EndAction = LevelCompletionResults::LevelEndAction;
    bool const failed = results->levelEndStateType == EndState::Failed;
    bool const cleared = results->levelEndStateType == EndState::Cleared;
    bool const quit = !failed && !cleared &&
                      (results->levelEndAction == EndAction::Quit || results->levelEndAction == EndAction::Restart);
    if (!failed && !cleared && !quit) return;
    if (quit && songTime < kMinQuitSongTime) {
      PaperLogger.info("Vivify replay recorder: quit at {:.1f} s; not kept", songTime);
      return;
    }
    if (finished.frames.empty()) return;

    auto& info = finished.info;
    info.score = results->multipliedScore;
    if (failed) info.failTime = songTime;
    auto key = self->beatmapKey;
    std::string hash = key.levelId;
    if (hash.starts_with("custom_level_")) hash = hash.substr(13);
    info.hash = hash;
    if (auto* level = self->beatmapLevel) {
      info.songName = std::string(level->songName);
      info.mapper = std::string(level->songAuthorName);
      info.environment = std::string(level->GetEnvironmentName(key.beatmapCharacteristic, key.difficulty)._environmentName);
    }
    info.difficulty = RF::DifficultyName(static_cast<int>(key.difficulty));
    if (key.beatmapCharacteristic) info.mode = std::string(key.beatmapCharacteristic->serializedName);
    info.modifiers = Modifiers(self->gameplayModifiers, results->energy);
    if (auto* setup = self->gameplayCoreSceneSetupData) {
      if (auto* player = setup->playerSpecificSettings) {
        info.leftHanded = player->leftHanded;
        info.height = player->automaticPlayerHeight ? 0 : player->playerHeight;
      }
    }
    if (auto* practice = self->practiceSettings) {
      info.startTime = practice->startSongTime;
      info.speed = practice->songSpeedMul;
    }
    Save(std::move(finished), failed, quit);
  } catch (std::exception const& ex) {
    PaperLogger.error("Vivify replay recorder: finishing failed: {}", ex.what());
  }
}

}  // namespace

std::string Directory() { return "/sdcard/ModData/com.beatgames.beatsaber/Mods/bl/replays/"; }

std::string LastSaved() {
  std::lock_guard lock(lastSavedMutex);
  return lastSaved;
}

void InstallHooks() {
  if constexpr (kLiteBuild) return;
  FetchHmd();
  INSTALL_HOOK(PaperLogger, Recorder_InstallBindings);
  INSTALL_HOOK(PaperLogger, Recorder_HeightLateUpdate);
  INSTALL_HOOK(PaperLogger, Recorder_SpawnNote);
  INSTALL_HOOK(PaperLogger, Recorder_SpawnObstacle);
  INSTALL_HOOK(PaperLogger, Recorder_NoteWasCut);
  INSTALL_HOOK(PaperLogger, Recorder_ScoreLateUpdate);
  INSTALL_HOOK(PaperLogger, Recorder_ScoreStart);
  INSTALL_HOOK(PaperLogger, Recorder_ComputeSwingRating);
  INSTALL_HOOK(PaperLogger, Recorder_ProcessSwing);
  INSTALL_HOOK(PaperLogger, Recorder_SpawnControllerStart);
  INSTALL_HOOK(PaperLogger, Recorder_ShowPause);
  INSTALL_HOOK(PaperLogger, Recorder_Resume);
  INSTALL_HOOK(PaperLogger, Recorder_Finish);
}

// Called from the ticker: a frame, the end of a wall hit.
void Tick() {
  if (!replay || saberManager == nullptr || playerTransforms == nullptr || audio == nullptr) return;
  if (!currentPause) {
    auto* origin = playerTransforms->_originParentTransform.ptr();
    auto* head = playerTransforms->_headTransform.ptr();
    auto* left = saberManager->get_leftSaber().unsafePtr();
    auto* right = saberManager->get_rightSaber().unsafePtr();
    if (origin != nullptr && head != nullptr && left != nullptr && right != nullptr) {
      RF::Frame frame;
      frame.time = audio->get_songTime();
      float const dt = UnityEngine::Time::get_deltaTime();
      frame.fps = dt > 0 ? static_cast<int32_t>(1.0f / dt) : 0;
      frame.head = Local(origin, head);
      frame.leftHand = Local(origin, left->get_transform());
      frame.rightHand = Local(origin, right->get_transform());
      replay->frames.push_back(frame);
    }
  }
  if (inWall && headObstacles != nullptr && headObstacles->_intersectingObstacles &&
      headObstacles->_intersectingObstacles->get_Count() == 0) {
    // BeatLeader's Quest recorder stores the leaving time in `energy`; kept
    // the same so the Replay mod reads these as it reads BeatLeader's.
    if (!replay->walls.empty()) replay->walls.back().energy = audio->get_songTime();
    inWall = false;
  }
}

}  // namespace Vivify::ReplayRecorder

void Vivify::ReplayRecorderTicker::LateUpdate() {
  try {
    Vivify::ReplayRecorder::Tick();
  } catch (...) {
  }
}
