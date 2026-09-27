#pragma once

// AudioLink for Beat Saber 1.40.8 on Quest.
//
// A port of Aeroluna's BSAudioLink (github.com/Aeroluna/BSAudioLink), itself a
// port of llealloo's VRChat AudioLink, following RedBrumbler's Quest port
// (github.com/RedBrumbler/BSAudioLink-Quest, last built for 1.28) where the
// Quest side differs.
//
// AudioLink turns the playing song into a 128x64 texture, _AudioTexture, that
// any AudioLink-compatible shader samples: beat-reactive bands, waveform,
// chronotensity, theme colours. The analysis runs on the GPU in a
// CustomRenderTexture driven by AudioLink's own shader; this module feeds it
// the audio samples, timing and colours every frame, the way AudioLink.cs does.
//
// The shader is Aeroluna's (AudioLink 3.1.2), shipped as a PC bundle and run
// through this mod's own shader converter at build time
// (tools/audiolink/build_bundle.py) into GLES programs a Quest can run.
namespace Vivify::AudioLink {

// Called once per frame, in the menu and in play.
void Tick();

// Whether AudioLink has loaded and is publishing _AudioTexture.
bool IsActive();

}  // namespace Vivify::AudioLink
