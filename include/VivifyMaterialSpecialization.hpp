#pragma once
#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace Vivify::MaterialSpecialization {
inline constexpr std::string_view kShader = "Sunao Shader/Cutout";
inline constexpr std::string_view kPrefix = "__VivifyFixedMaterialV1__/";
inline constexpr std::string_view kCacheProof = "fixed-material-features-proof-v2";
inline bool IsFixed(std::string_view name) {
  if (name.starts_with(kPrefix)) return true;
  // The six isolated donor namespaces wrap the same marked profile.
  return name.starts_with("__Vivify") && name.find(kPrefix) != std::string_view::npos;
}
inline constexpr std::array<std::pair<std::string_view, float>, 12> kFlags{{
    {"_DecalEnable", 0}, {"_EmissionEnable", 0}, {"_FurEnable", 0},
    {"_LimitterEnable", 0}, {"_MatCapMaskEnable", 1}, {"_OutLineEnable", 0},
    {"_ParallaxEnable", 0}, {"_ReflectionEnable", 0}, {"_RimLitEnable", 0},
    {"_SubTexEnable", 0}, {"_ToonEnable", 0}, {"_ToonGlossEnable", 0}}};
inline std::string Canonical(std::string name) {
  if (name.starts_with(kPrefix)) name.erase(0, kPrefix.size());
  return name;
}
inline bool Matches(std::map<std::string, float> const& values) {
  for (auto const& [name, expected] : kFlags) {
    auto found = values.find(std::string(name));
    if (found == values.end() || found->second != expected) return false;
  }
  return true;
}
// Only exact scalar uniform declarations are rewritten. The author-provided
// value and all texture/geometry instructions are retained; the GLES optimizer
// can then remove unreachable work. Caller must establish immutability first.
inline void Specialize(std::string& source) {
  for (auto const& [name, value] : kFlags) for (auto const* type : {"int", "float"}) {
    std::string const declaration = std::string("uniform ") + type + " " + std::string(name) + ";";
    std::string const literal = std::string(type) == "int" ? (value == 0 ? "0" : "1") : (value == 0 ? "0.0" : "1.0");
    std::string const replacement = std::string("const ") + type + " " + std::string(name) + " = " + literal + ";";
    size_t at = 0;
    while ((at = source.find(declaration, at)) != std::string::npos) {
      if (at && source[at - 1] != '\n') { at += declaration.size(); continue; }
      source.replace(at, declaration.size(), replacement); at += replacement.size();
    }
  }
}
// JSON DOM adapter: requires only the usual IsObject/GetObject/etc. API.
// Kept independent of a JSON library so the eligibility policy is unit tested
// without Unity or the game SDK. Syntax errors must be rejected by the caller.
template<class Json> bool ChartAllows(Json const& document) {
  auto safeJson = [&](auto&& self, auto const& value, int depth) -> bool {
    if (depth > 32) return false;
    if (value.IsString()) for (auto const& [flag, _] : MaterialSpecialization::kFlags)
      if (std::string_view(value.GetString(), value.GetStringLength()) == flag) return false;
    if (value.IsObject()) {
      std::set<std::string> names;
      for (auto const& member : value.GetObject()) {
        std::string name(member.name.GetString(), member.name.GetStringLength());
        if (!names.insert(name).second || !self(self, member.name, depth + 1) || !self(self, member.value, depth + 1)) return false;
      }
    } else if (value.IsArray()) for (auto const& item : value.GetArray()) if (!self(self, item, depth + 1)) return false;
    return true;
  };
  if (!document.IsObject() || !safeJson(safeJson, document, 0)) return false;
  bool const legacy = document.HasMember("_customData");
  if (legacy && document.HasMember("customData")) return false;
  char const* versionKey = legacy ? "_version" : "version";
  if (!document.HasMember(versionKey) || !document[versionKey].IsString() ||
    !std::string_view(document[versionKey].GetString()).starts_with(legacy ? "2." : "3.")) return false;
  char const* customKey = legacy ? "_customData" : "customData";
  if (!document.HasMember(customKey) || !document[customKey].IsObject()) return false;
  auto const& custom = document[customKey];
  if (custom.HasMember("customEvents") && custom.HasMember("_customEvents")) return false;
  for (auto const& member : custom.GetObject()) {
    std::string_view key(member.name.GetString(), member.name.GetStringLength());
    if (key != "customEvents" && key != "_customEvents" && key != "environment" && key != "_environment" && key != "time" && key != "_time") return false;
  }
  char const* eventsKey = legacy ? "_customEvents" : "customEvents";
  if (!custom.HasMember(eventsKey) || !custom[eventsKey].IsArray()) return false;
  for (auto const& event : custom[eventsKey].GetArray()) {
    char const* typeKey = legacy ? "_type" : "t";
    if (!event.IsObject() || !event.HasMember(typeKey) || !event[typeKey].IsString()) return false;
    std::string_view type(event[typeKey].GetString(), event[typeKey].GetStringLength());
    if (type != "SetRenderingSettings" && type != "InstantiatePrefab" && type != "DestroyObject") return false;
  }
  return true;
}
}
