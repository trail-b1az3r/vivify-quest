#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace Vivify::GeometrySpecialization {
inline bool SideEffectFreeVertex(std::string_view source) {
  // Reject storage resources and synchronization conservatively, including
  // readonly buffers. Ordinary uniforms/attributes have no observable writes.
  return source.find("buffer") == std::string_view::npos && source.find("image") == std::string_view::npos &&
         source.find("atomic") == std::string_view::npos && source.find("barrier") == std::string_view::npos && source.find("Barrier") == std::string_view::npos;
}
// Recognize a deliberately narrow prefix of OUR translator's output, rather
// than guessing from shader names. Before the guard only local declarations
// and a PrimitiveID read are permitted; emit/storage writes are not permitted.
// With Unity's perspective-camera unity_OrthoParams.w == 0, the guard returns
// before any primitive can be emitted. All variants/passes must pass this proof.
inline bool EmptyForPerspective(std::string_view translated) {
  std::string compact;
  compact.reserve(translated.size());
  for (unsigned char c : translated) if (!std::isspace(c)) compact.push_back(static_cast<char>(c));
  if (compact.find("uniformvec4unity_OrthoParams;") == std::string::npos) return false;
  auto at = compact.find("voidmain(){");
  if (at == std::string::npos) return false;
  std::string_view body(compact.data() + at + 11, compact.size() - at - 11);
  auto digits = [](std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  auto registers = [&](std::string_view text) {
    while (!text.empty()) {
      auto comma = text.find(','); auto item = text.substr(0, comma);
      if (!item.starts_with('r') || !digits(item.substr(1))) return false;
      if (comma == std::string_view::npos) return true;
      text.remove_prefix(comma + 1);
    }
    return false;
  };
  for (;;) {
    auto end = body.find(';');
    if (end == std::string_view::npos) return false;
    auto statement = body.substr(0, end);
    bool local = statement == "ivec4vPrimitiveID=ivec4(gl_PrimitiveID)";
    if (statement.starts_with("ivec4r")) local = registers(statement.substr(5));
    if (statement.starts_with("ivec4x")) {
      auto bracket = statement.find('[');
      local = bracket != std::string_view::npos && statement.ends_with(']') &&
              digits(statement.substr(6, bracket - 6)) && digits(statement.substr(bracket + 1, statement.size() - bracket - 2));
    }
    if (local) { body.remove_prefix(end + 1); continue; }
    auto equal = statement.find('=');
    if (equal == std::string_view::npos) return false;
    auto target = statement.substr(0, equal);
    if (target.size() < 4 || target.front() != 'r' || target[target.size() - 2] != '.' ||
        std::string_view("xyzw").find(target.back()) == std::string_view::npos ||
        !digits(target.substr(1, target.size() - 3)) ||
        statement.substr(equal + 1) != "(unity_OrthoParams.w!=1.0)?-1:0") return false;
    body.remove_prefix(end + 1);
    return body.starts_with("if(" + std::string(target) + "!=0){return;}");
  }
}

inline std::string NativeEmptyProgram() {
  // Every vertex is clipped outside the view volume. There is no geometry
  // stage, no emitted primitive and no depth/color/stencil contribution.
  return "#ifdef VERTEX\n#version 300 es\n#extension GL_OVR_multiview2 : require\n"
         "precision highp float;\nlayout(num_views = 2) in;\n"
         "void main() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }\n#endif\n"
         "#ifdef FRAGMENT\n#version 300 es\nprecision highp float;\n"
         "layout(location = 0) out vec4 SV_Target0;\nvoid main() { SV_Target0 = vec4(0.0); }\n#endif\n";
}
}
