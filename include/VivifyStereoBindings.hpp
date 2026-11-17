#pragma once
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace Vivify::StereoBindings {
using Mask = uint16_t;
inline constexpr std::array<std::string_view, 12> Names = {
    "unity_StereoMatrixV", "unity_StereoMatrixP", "unity_StereoMatrixVP",
    "unity_StereoMatrixInvV", "unity_StereoMatrixInvP", "unity_StereoMatrixInvVP",
    "unity_StereoCameraProjection", "unity_StereoCameraInvProjection",
    "unity_StereoWorldToCamera", "unity_StereoCameraToWorld",
    "unity_StereoWorldSpaceCameraPos", "unity_StereoScaleOffset"};
inline constexpr Mask All = (Mask(1) << Names.size()) - 1;
inline constexpr std::string_view MatrixPrefix = "hlslcc_mtx4x4";
constexpr bool Needs(Mask mask, unsigned index) { return index < Names.size() && (mask & (Mask(1) << index)) != 0; }
inline bool Identifier(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline void SkipSpace(std::string_view text, size_t& at) {
  while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
}
inline std::string_view PreviousWord(std::string_view text, size_t& at) {
  while (at && std::isspace(static_cast<unsigned char>(text[at - 1]))) --at;
  auto const end = at;
  while (at && Identifier(text[at - 1])) --at;
  return text.substr(at, end - at);
}
inline bool Declaration(std::string_view text, size_t start, size_t end, unsigned index) {
  auto before = start;
  auto type = PreviousWord(text, before);
  if (PreviousWord(text, before) != "uniform") return false;
  auto name = text.substr(start, end - start);
  char count = '2';
  if (index < 10) {
    if (name == Names[index]) {
      if (type != "mat4") return false;
    } else if (name.starts_with(MatrixPrefix) && name.substr(MatrixPrefix.size()) == Names[index]) {
      if (type != "vec4") return false;
      count = '8';
    } else return false;
  } else if (name != Names[index] || type != "vec4") return false;
  for (char expected : {'[', count, ']', ';'}) {
    SkipSpace(text, end);
    if (end == text.size() || text[end++] != expected) return false;
  }
  return true;
}
// This does not change shader code or guess from display names. An unused
// uniform declaration does not require a Material.Set* call. Unknown forms,
// blocks, macro expansion and incomplete programs keep every existing binding.
inline Mask UsedByProgram(std::string_view text, Mask metadataBindings = 0) {
  if (text.find("void main(") == text.npos || text.find("UnityStereoGlobals") != text.npos ||
      text.find('\\') != text.npos || text.find('"') != text.npos || text.find("/*") != text.npos ||
      text.find("//") != text.npos)
    return All;
  for (size_t at = text.find('#'); at != text.npos; at = text.find('#', at)) {
    ++at; SkipSpace(text, at); auto end = at;
    while (end < text.size() && Identifier(text[end])) ++end;
    auto directive = text.substr(at, end - at);
    if (directive != "version" && directive != "extension" && directive != "ifdef" && directive != "ifndef" &&
        directive != "if" && directive != "elif" && directive != "else" && directive != "endif") return All;
    at = end;
  }
  Mask mask = 0;
  for (unsigned index = 0; index < Names.size(); ++index) {
    bool declared = false;
    for (size_t at = text.find(Names[index]); at != text.npos; at = text.find(Names[index], at)) {
      auto start = at, end = at + Names[index].size();
      while (start && Identifier(text[start - 1])) --start;
      while (end < text.size() && Identifier(text[end])) ++end;
      // VP must not imply V/P. A different identifier only conservatively
      // counts when it is not another known stereo property/representation.
      auto token = text.substr(start, end - start);
      auto canonical = token.starts_with(MatrixPrefix) ? token.substr(MatrixPrefix.size()) : token;
      bool other = false;
      for (unsigned j = 0; j < Names.size(); ++j)
        if (j != index && canonical == Names[j]) other = true;
      if (!other) {
        if (!Declaration(text, start, end, index)) { mask |= Mask(1) << index; break; }
        declared = true;
      }
      at = end;
    }
    // If Unity's parameter table names this binding but the program does not
    // expose its expected declaration, an unknown alias/layout may be in use.
    if (Needs(metadataBindings, index) && !declared) mask |= Mask(1) << index;
  }
  return mask;
}
}
