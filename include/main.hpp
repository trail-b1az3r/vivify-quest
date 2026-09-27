#pragma once

#include <string>
// paper2_scotland2 4.7.0's shared/backtrace.hpp calls std::ifstream but includes
// only <sstream>, and beatsaber-hook pulls that header in from
// config-utils.hpp -> utils-functions.h. libc++ used to leak <fstream> through
// <sstream>; under NDK 27 it no longer does, so the template is incomplete at
// the point of use and every translation unit that reaches beatsaber-hook fails
// to compile. Including it here, ahead of that chain, fixes all of them without
// editing a vendored header that restore-deps.py regenerates.
#include <fstream>
#include "scotland2/shared/modloader.h"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "paper2_scotland2/shared/logger.hpp"
#include "_config.hpp"
Configuration &getConfig();
bool GetMultipassRenderingEnabled();
bool GetVivifyDebugLogging();
bool GetDisableBeat0FilmgrainBlit();
bool GetDisableAllBlits();
bool GetDisableCreateCameraDepth();
bool GetDisableCustomNoteVisuals();
bool GetDisableVisualsInMultiplayer();
bool GetDisableVRCenterAdjust();
bool GetConvertPcBundlesOnDevice();
bool GetStandInShading();
// Whether the main camera renders _CameraDepthTexture while a Vivify map plays.
bool GetSceneDepthTexture();
bool GetTranslateShadersOnConversion();
// Whether the built-in AudioLink port (VivifyAudioLink) runs and publishes
// _AudioTexture.
bool GetAudioLinkEnabled();
// Whether SetRenderingSettings may turn on realtime shadows (off: the
// shadow* quality settings in it are ignored).
bool GetMapRealtimeShadows();
// Whether converted Unity 2019 bundles get single-view programs too (0.14.8).
bool GetSplitUnity2019Shaders();
// Whether a shader a map's Quest bundle shipped empty is replaced by the PC
// build of it, converted (VivifyAssets.cpp).
bool GetUsePcShadersForEmptyShaders();
// Empty when the stand-in shader should be chosen automatically.
std::string GetStandInShaderName();
// Whether a map carrying the Vivify requirement still submits its score.
// Submission gates replay recording, so turning it off leaves no replay behind.
bool GetSubmitScoresOnVivifyMaps();
void EnsureConfigDefaults();
constexpr auto PaperLogger = Paper::ConstLoggerContext("Vivify");

#define VIVIFY_DEBUG(...)                                          \
  do {                                                             \
    if (GetVivifyDebugLogging()) PaperLogger.info(__VA_ARGS__);    \
  } while (false)
