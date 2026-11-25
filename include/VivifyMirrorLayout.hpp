#pragma once

// How the game's mirror shader reads its reflection texture on the Quest
// (0.15.54). The game draws each eye's reflection into one half of a
// double-wide texture and the mirror material samples it by screen position.
// The Quest never uses the game's mirrors itself (GraphicsTweaks turns them
// on), so the multiview build of that shader may read the texture in another
// layout: without picking the eye's half, upside down, or one eye above the
// other. A one-time test draws the mirror material over a 2 x 2 texture of
// four colours and reads back which texel each quarter of each eye shows.
// These functions turn those readings into what the mirror has to change.

#include <cmath>

namespace Vivify::MirrorLayout {

// The probe texture's texels: x left/right, y bottom/top (Unity's v).
enum class Texel { BottomLeft, BottomRight, TopLeft, TopRight, Unknown };

inline int TexelX(Texel t) { return t == Texel::BottomRight || t == Texel::TopRight ? 1 : 0; }
inline int TexelY(Texel t) { return t == Texel::TopLeft || t == Texel::TopRight ? 1 : 0; }

// The probe colours: bottom-left red, bottom-right green, top-left blue,
// top-right yellow. The mirror may tint, darken or add to them; the closest
// direction wins when it is clearly the closest.
inline Texel Classify(int r, int g, int b) {
  float const length = std::sqrt(static_cast<float>(r * r + g * g + b * b));
  if (length < 24.0f) return Texel::Unknown;
  float const v[3] = {r / length, g / length, b / length};
  constexpr float kHalf = 0.70710678f;
  float const patterns[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {kHalf, kHalf, 0}};
  int best = -1;
  float bestDot = -1.0f, secondDot = -1.0f;
  for (int i = 0; i < 4; i++) {
    float const dot = v[0] * patterns[i][0] + v[1] * patterns[i][1] + v[2] * patterns[i][2];
    if (dot > bestDot) {
      secondDot = bestDot;
      bestDot = dot;
      best = i;
    } else if (dot > secondDot) {
      secondDot = dot;
    }
  }
  if (bestDot < 0.93f || bestDot - secondDot < 0.08f) return Texel::Unknown;
  return static_cast<Texel>(best);
}

enum class StereoMode {
  AsGame,     // each eye reads its own half: the game's layout is right
  Swapped,    // each eye reads the other eye's half
  FullWidth,  // each eye reads the whole width: one centred reflection fills it
  OverUnder,  // each eye reads the whole width of one vertical half
};

struct StereoRemedy {
  bool known = false;
  StereoMode mode = StereoMode::AsGame;
  bool flipV = false;
  int row[2] = {0, 1};  // OverUnder: the vertical half (0 bottom) each eye reads
};

struct MonoRemedy {
  bool known = false;
  bool flipV = false;
  // -1: reads the whole width. 0 / 1: reads only the left / right half
  // across the whole view (the shader expects the two-eye side-by-side
  // texture even in one-eye form), so a one-eye reflection is drawn into
  // that half (0.16.0; the game's Custom/Mirror on the Quest reads the left).
  int half = -1;
};

// samples[layer][row][col]: layer 0 left eye; row 0 the lower quarter of the
// screen, col 0 the left quarter.
inline StereoRemedy Stereo(Texel const (&samples)[2][2][2]) {
  StereoRemedy remedy;
  int half[2] = {-1, -1};      // the texture half a layer reads at every point, or -1
  bool full[2] = {false, false};
  int flip[2] = {-1, -1};      // 0 upright, 1 upside down, -1 neither
  int row[2] = {-1, -1};       // a texel row a layer reads at every point
  for (int layer = 0; layer < 2; layer++) {
    auto const& s = samples[layer];
    for (int r = 0; r < 2; r++)
      for (int c = 0; c < 2; c++)
        if (s[r][c] == Texel::Unknown) return remedy;
    bool sameHalf = true, wholeWidth = true;
    for (int r = 0; r < 2; r++) {
      if (TexelX(s[r][0]) != TexelX(s[r][1])) sameHalf = false;
      if (TexelX(s[r][0]) != 0 || TexelX(s[r][1]) != 1) wholeWidth = false;
    }
    if (sameHalf && TexelX(s[0][0]) == TexelX(s[1][0])) half[layer] = TexelX(s[0][0]);
    full[layer] = wholeWidth;
    bool upright = true, flipped = true, constant = true;
    for (int c = 0; c < 2; c++) {
      if (TexelY(s[0][c]) != 0 || TexelY(s[1][c]) != 1) upright = false;
      if (TexelY(s[0][c]) != 1 || TexelY(s[1][c]) != 0) flipped = false;
      if (TexelY(s[0][c]) != TexelY(s[1][c]) || TexelY(s[0][c]) != TexelY(s[0][0])) constant = false;
    }
    flip[layer] = upright ? 0 : (flipped ? 1 : -1);
    if (constant) row[layer] = TexelY(s[0][0]);
  }
  if (flip[0] >= 0 && flip[0] == flip[1]) {
    remedy.flipV = flip[0] == 1;
    if (half[0] == 0 && half[1] == 1) {
      remedy.known = true;
      remedy.mode = StereoMode::AsGame;
    } else if (half[0] == 1 && half[1] == 0) {
      remedy.known = true;
      remedy.mode = StereoMode::Swapped;
    } else if (full[0] && full[1]) {
      remedy.known = true;
      remedy.mode = StereoMode::FullWidth;
    }
    return remedy;
  }
  if (full[0] && full[1] && row[0] >= 0 && row[1] >= 0 && row[0] != row[1]) {
    remedy.known = true;
    remedy.mode = StereoMode::OverUnder;
    remedy.row[0] = row[0];
    remedy.row[1] = row[1];
  }
  return remedy;
}

// samples[row][col] of the one-eye program drawn into a plain texture.
inline MonoRemedy Mono(Texel const (&samples)[2][2]) {
  MonoRemedy remedy;
  bool upright = true, flipped = true, whole = true, left = true, right = true;
  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 2; c++) {
      if (samples[r][c] == Texel::Unknown) return remedy;
      int const x = TexelX(samples[r][c]);
      if (x != c) whole = false;
      if (x != 0) left = false;
      if (x != 1) right = false;
      if (TexelY(samples[r][c]) != r) upright = false;
      if (TexelY(samples[r][c]) != 1 - r) flipped = false;
    }
  }
  if (!(upright || flipped) || !(whole || left || right)) return remedy;
  remedy.known = true;
  remedy.flipV = flipped;
  remedy.half = whole ? -1 : (left ? 0 : 1);
  return remedy;
}

inline char const* ModeName(StereoMode mode) {
  switch (mode) {
    case StereoMode::AsGame: return "each eye its own half (the game's layout)";
    case StereoMode::Swapped: return "each eye the other eye's half";
    case StereoMode::FullWidth: return "the whole width in each eye";
    case StereoMode::OverUnder: return "one eye above the other";
  }
  return "?";
}

}  // namespace Vivify::MirrorLayout
