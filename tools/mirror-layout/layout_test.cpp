// g++ -std=c++17 -Iinclude tools/mirror-layout/layout_test.cpp && ./a.out
#include <cassert>
#include <cstdio>

#include "VivifyMirrorLayout.hpp"

using namespace Vivify::MirrorLayout;
using T = Texel;

int main() {
  assert(Classify(200, 10, 10) == T::BottomLeft);
  assert(Classify(10, 180, 10) == T::BottomRight);
  assert(Classify(0, 0, 90) == T::TopLeft);
  assert(Classify(150, 140, 5) == T::TopRight);
  assert(Classify(5, 5, 5) == T::Unknown);
  assert(Classify(100, 100, 100) == T::Unknown);

  // The game's layout, upright.
  T asGame[2][2][2] = {{{T::BottomLeft, T::BottomLeft}, {T::TopLeft, T::TopLeft}},
                       {{T::BottomRight, T::BottomRight}, {T::TopRight, T::TopRight}}};
  auto r = Stereo(asGame);
  assert(r.known && r.mode == StereoMode::AsGame && !r.flipV);

  // Upside down.
  T flipped[2][2][2] = {{{T::TopLeft, T::TopLeft}, {T::BottomLeft, T::BottomLeft}},
                        {{T::TopRight, T::TopRight}, {T::BottomRight, T::BottomRight}}};
  r = Stereo(flipped);
  assert(r.known && r.mode == StereoMode::AsGame && r.flipV);

  // No eye offset: each eye reads across the whole texture.
  T full[2][2][2] = {{{T::BottomLeft, T::BottomRight}, {T::TopLeft, T::TopRight}},
                     {{T::BottomLeft, T::BottomRight}, {T::TopLeft, T::TopRight}}};
  r = Stereo(full);
  assert(r.known && r.mode == StereoMode::FullWidth && !r.flipV);

  // Eyes swapped and upside down.
  T swapped[2][2][2] = {{{T::TopRight, T::TopRight}, {T::BottomRight, T::BottomRight}},
                        {{T::TopLeft, T::TopLeft}, {T::BottomLeft, T::BottomLeft}}};
  r = Stereo(swapped);
  assert(r.known && r.mode == StereoMode::Swapped && r.flipV);

  // One eye above the other.
  T overUnder[2][2][2] = {{{T::BottomLeft, T::BottomRight}, {T::BottomLeft, T::BottomRight}},
                          {{T::TopLeft, T::TopRight}, {T::TopLeft, T::TopRight}}};
  r = Stereo(overUnder);
  assert(r.known && r.mode == StereoMode::OverUnder && r.row[0] == 0 && r.row[1] == 1);

  // Both eyes reading the same half: nothing the mirror can be told to do.
  T same[2][2][2] = {{{T::BottomLeft, T::BottomLeft}, {T::TopLeft, T::TopLeft}},
                     {{T::BottomLeft, T::BottomLeft}, {T::TopLeft, T::TopLeft}}};
  assert(!Stereo(same).known);

  T unknown[2][2][2] = {{{T::Unknown, T::BottomLeft}, {T::TopLeft, T::TopLeft}},
                        {{T::BottomRight, T::BottomRight}, {T::TopRight, T::TopRight}}};
  assert(!Stereo(unknown).known);

  T mono[2][2] = {{T::BottomLeft, T::BottomRight}, {T::TopLeft, T::TopRight}};
  auto m = Mono(mono);
  assert(m.known && !m.flipV);
  T monoFlipped[2][2] = {{T::TopLeft, T::TopRight}, {T::BottomLeft, T::BottomRight}};
  m = Mono(monoFlipped);
  assert(m.known && m.flipV);
  assert(m.half == -1);
  // The Quest game's Custom/Mirror, read back on a headset (0.15.55 log):
  // red red / blue blue -- the left half, upright.
  T monoHalf[2][2] = {{T::BottomLeft, T::BottomLeft}, {T::TopLeft, T::TopLeft}};
  m = Mono(monoHalf);
  assert(m.known && !m.flipV && m.half == 0);
  assert(Classify(134, 2, 2) == T::BottomLeft && Classify(1, 1, 135) == T::TopLeft && Classify(0, 0, 134) == T::TopLeft);
  T monoMixed[2][2] = {{T::BottomLeft, T::BottomRight}, {T::TopRight, T::TopRight}};
  assert(!Mono(monoMixed).known);

  std::puts("mirror layout: all passed");
  return 0;
}
