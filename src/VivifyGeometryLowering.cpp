#include "VivifyGeometryLowering.hpp"

#include <cctype>
#include <map>
#include <set>
#include <regex>
#include <sstream>
#include <vector>

namespace Vivify::GeometryLowering {
namespace {

bool IsName(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void Rename(std::string& text, std::string const& from, std::string const& to) {
  size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) {
    bool const starts = at == 0 || !IsName(text[at - 1]);
    bool const ends = at + from.size() >= text.size() || !IsName(text[at + from.size()]);
    if (starts && ends) {
      text.replace(at, from.size(), to);
      at += to.size();
    } else {
      at += from.size();
    }
  }
}

size_t Count(std::string const& text, std::string const& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) n++;
  return n;
}

std::string Trim(std::string const& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::vector<std::string> Lines(std::string const& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

// A top-level declaration "[flat] in|out [precision] type name[[]];".
struct Declaration {
  std::string type;       // with any precision qualifier, e.g. "highp int"
  std::string name;
  std::string qualifiers; // "flat " or ""
  bool array = false;
};

std::optional<Declaration> ParseInterface(std::string const& line, std::string const& direction) {
  static std::regex const pattern(
      R"(^\s*(flat\s+|smooth\s+|noperspective\s+)?(in|out)\s+((?:highp\s+|mediump\s+|lowp\s+)?\w+)\s+(\w+)\s*(\[\s*\d*\s*\])?\s*;\s*$)");
  std::smatch m;
  if (!std::regex_match(line, m, pattern) || m[2].str() != direction) return std::nullopt;
  Declaration d;
  d.qualifiers = m[1].matched ? Trim(m[1].str()) + " " : "";
  d.type = m[3].str();
  d.name = m[4].str();
  d.array = m[5].matched;
  return d;
}

// The body of "void main()" ... its closing brace, and everything before it.
bool SplitMain(std::string const& text, std::string& header, std::string& body) {
  size_t const at = text.find("void main()");
  if (at == std::string::npos) return false;
  size_t const open = text.find('{', at);
  if (open == std::string::npos) return false;
  int depth = 0;
  for (size_t i = open; i < text.size(); i++) {
    if (text[i] == '{') depth++;
    else if (text[i] == '}' && --depth == 0) {
      header = text.substr(0, at);
      body = text.substr(open, i - open + 1);
      return true;
    }
  }
  return false;
}

// Removes every "if (VIVIFY_EYE_COPY != 0) { ... }" block (the converter's
// per-eye copy for a geometry stage under multiview; a vertex stage has no
// gl_Layer).
void RemoveEyeCopyBlocks(std::string& body) {
  static std::string const head = "if (VIVIFY_EYE_COPY != 0) {";
  size_t at;
  while ((at = body.find(head)) != std::string::npos) {
    int depth = 0;
    size_t end = std::string::npos;
    for (size_t i = at + head.size() - 1; i < body.size(); i++) {
      if (body[i] == '{') depth++;
      else if (body[i] == '}' && --depth == 0) {
        end = i;
        break;
      }
    }
    if (end == std::string::npos) return;
    body.erase(at, end - at + 1);
  }
}

}  // namespace

Result LowerTriangleGeometry(std::string const& vertex, std::string const& geometry) {
  Result result;
  auto refuse = [&](std::string why) {
    result.reason = std::move(why);
    return result;
  };
  if (geometry.find("layout(triangles) in;") == std::string::npos) return refuse("the geometry stage does not take triangles");
  if (geometry.find("max_vertices = 3)") == std::string::npos) return refuse("the geometry stage emits more than 3 vertices");
  if (Count(geometry, "EmitVertex();") != 3) return refuse("the geometry stage does not emit exactly 3 vertices");
  for (auto const* loop : {"for (", "while (", "do {", "for(", "while("})
    if (geometry.find(loop) != std::string::npos) return refuse("the geometry stage loops");
  if (vertex.find("gl_Position") == std::string::npos) return refuse("the vertex stage does not write gl_Position");
  if (vertex.find(kMarker) != std::string::npos) return refuse("already lowered");

  std::string vsHeader, vsBody, gsHeader, gsBody;
  if (!SplitMain(vertex, vsHeader, vsBody) || !SplitMain(geometry, gsHeader, gsBody))
    return refuse("no main() to lower");

  // The vertex program's header: inputs (only in_POSITION0) and outputs
  // (each becomes a plain global the corner runs fill).
  std::vector<Declaration> vsOutputs;
  std::string vsGlobals;      // header lines kept, outputs turned into globals
  std::string vsPreamble;     // #version, #extension, layout(num_views), precision
  bool position = false;
  for (auto const& line : Lines(vsHeader)) {
    std::string const t = Trim(line);
    if (t.empty()) continue;
    if (t.starts_with("#version") || t.starts_with("#extension") || t.starts_with("layout(num_views") ||
        t.starts_with("precision ")) {
      vsPreamble += t + "\n";
      continue;
    }
    if (auto in = ParseInterface(t, "in")) {
      if (in->name != "in_POSITION0" || in->array) return refuse("the vertex stage reads " + in->name);
      position = true;
      continue;
    }
    if (auto out = ParseInterface(t, "out")) {
      if (out->array) return refuse("an array output from the vertex stage");
      vsOutputs.push_back(*out);
      continue;
    }
    if (t.starts_with("in ") || t.starts_with("out ") || t.find(" in ") != std::string::npos ||
        t.find(" out ") != std::string::npos) {
      if (t.find("uniform") == std::string::npos) return refuse("an interface declaration not understood: " + t);
    }
    vsGlobals += t + "\n";
  }
  if (!position) return refuse("the vertex stage has no in_POSITION0");

  // The geometry program's header: inputs (arrays of the vertex outputs),
  // outputs (the lowered program's own), uniforms.
  std::vector<Declaration> gsOutputs;
  std::vector<std::string> gsInputs;
  std::string gsGlobals;
  {
    int skipDepth = 0;  // inside "#ifdef GL_EXT_multiview_tessellation_geometry_shader"
    for (auto const& line : Lines(gsHeader)) {
      std::string const t = Trim(line);
      if (t.empty()) continue;
      if (skipDepth > 0) {
        if (t.starts_with("#if")) skipDepth++;
        else if (t.starts_with("#endif")) skipDepth--;
        continue;
      }
      if (t == "#ifdef GL_EXT_multiview_tessellation_geometry_shader") {
        skipDepth = 1;
        continue;
      }
      if (t.starts_with("#version") || t.starts_with("#extension") || t.starts_with("precision ") ||
          t.starts_with("layout(triangles") || t.starts_with("layout(triangle_strip") ||
          t.starts_with("layout(invocations") || t.starts_with("layout(num_views") ||
          t.starts_with("#define VIVIFY_EYE_COPY"))
        continue;
      if (t.starts_with("#")) return refuse("a preprocessor line in the geometry stage: " + t);
      if (auto in = ParseInterface(t, "in")) {
        if (!in->array) return refuse("a geometry input that is not per vertex: " + in->name);
        gsInputs.push_back(in->name);
        continue;
      }
      if (auto out = ParseInterface(t, "out")) {
        if (out->array) return refuse("an array output from the geometry stage");
        gsOutputs.push_back(*out);
        continue;
      }
      if (t.find("uniform") == std::string::npos) return refuse("a geometry declaration not understood: " + t);
      // The same uniform twice is fine; a different one under a taken name is not.
      if (vsGlobals.find(t) != std::string::npos) continue;
      gsGlobals += t + "\n";
    }
  }
  for (auto const& name : gsInputs) {
    bool found = false;
    for (auto const& out : vsOutputs) found = found || out.name == name;
    if (!found) return refuse("the geometry stage reads " + name + ", which the vertex stage does not write");
  }

  // The vertex body, as a function over globals.
  std::string vs = vsBody;
  Rename(vs, "in_POSITION0", "vivify_corner_position");
  Rename(vs, "gl_Position", "vivify_v_Position");
  for (auto const& out : vsOutputs) Rename(vs, out.name, "vivify_v_" + out.name);
  Rename(vsGlobals, "in_POSITION0", "vivify_corner_position");

  // The geometry body: inputs from the three corner runs, outputs into
  // temporaries kept for this vertex's corner when it emits.
  std::string gs = gsBody;
  RemoveEyeCopyBlocks(gs);
  {
    std::string kept;
    for (auto const& line : Lines(gs)) {
      if (line.find("gl_InvocationID") != std::string::npos || line.find("vivify_LeftToRight") != std::string::npos ||
          line.find("EndPrimitive") != std::string::npos)
        continue;
      kept += line + "\n";
    }
    gs = kept;
  }
  if (gs.find("gl_Layer") != std::string::npos || gs.find("VIVIFY_EYE_COPY") != std::string::npos ||
      gs.find("gl_PrimitiveIDIn") != std::string::npos)
    return refuse("the geometry stage uses a per-primitive or per-layer value");
  gs = std::regex_replace(gs, std::regex(R"(gl_in\[\s*([^\]]+?)\s*\]\.gl_Position)"), "vivify_c_Position[$1]");
  if (gs.find("gl_in") != std::string::npos) return refuse("the geometry stage reads gl_in beyond gl_Position");
  for (auto const& name : gsInputs) Rename(gs, name, "vivify_c_" + name);
  for (auto const& out : gsOutputs) Rename(gs, out.name, "vivify_g_" + out.name);
  Rename(gs, "gl_Position", "vivify_g_Position");
  Rename(gs, "EmitVertex", "vivify_emit_vertex");

  std::string text;
  text += vsPreamble;
  text += std::string(kMarker) + "\n";
  text += "in vec4 in_POSITION0;\nin vec4 in_TEXCOORD4;\nin vec4 in_TEXCOORD5;\nin vec4 in_TEXCOORD6;\nin vec4 in_TEXCOORD7;\n";
  for (auto const& out : gsOutputs) text += out.qualifiers + "out " + out.type + " " + out.name + ";\n";
  text += "vec4 vivify_corner_position;\nvec4 vivify_v_Position;\nvec4 vivify_c_Position[3];\nvec4 vivify_g_Position;\n";
  text += "int vivify_corner;\nint vivify_emit;\n";
  for (auto const& out : vsOutputs) {
    text += out.type + " vivify_v_" + out.name + ";\n";
    text += out.type + " vivify_c_" + out.name + "[3];\n";
  }
  for (auto const& out : gsOutputs) text += out.type + " vivify_g_" + out.name + ";\n";
  text += vsGlobals;
  text += gsGlobals;
  text += "void vivify_vertex() " + vs + "\n";
  text += "void vivify_emit_vertex() {\n  if (vivify_emit == vivify_corner) {\n    gl_Position = vivify_g_Position;\n";
  for (auto const& out : gsOutputs) text += "    " + out.name + " = vivify_g_" + out.name + ";\n";
  text += "  }\n  vivify_emit++;\n}\n";
  text += "void vivify_geometry() " + gs + "\n";
  text += "void main() {\n";
  text += "  bool vivify_prepared = in_TEXCOORD4.w > 0.5;\n";
  text += "  vivify_corner = vivify_prepared ? clamp(int(in_TEXCOORD4.x + 0.5), 0, 2) : 0;\n";
  char const* corners[3] = {"in_TEXCOORD5", "in_TEXCOORD6", "in_TEXCOORD7"};
  for (int c = 0; c < 3; c++) {
    text += "  vivify_corner_position = vivify_prepared ? " + std::string(corners[c]) + " : in_POSITION0;\n";
    text += "  vivify_vertex();\n";
    text += "  vivify_c_Position[" + std::to_string(c) + "] = vivify_v_Position;\n";
    for (auto const& out : vsOutputs)
      text += "  vivify_c_" + out.name + "[" + std::to_string(c) + "] = vivify_v_" + out.name + ";\n";
  }
  text += "  if (!vivify_prepared) {\n    gl_Position = vivify_c_Position[0];\n";
  for (auto const& out : gsOutputs) text += "    " + out.name + " = " + out.type.substr(out.type.find_last_of(' ') + 1) + "(0);\n";
  text += "    return;\n  }\n";
  // A triangle the geometry stage emits nothing for is not drawn: every
  // corner left at w = 0 is clipped away.
  text += "  gl_Position = vec4(0.0);\n";
  text += "  vivify_emit = 0;\n  vivify_geometry();\n}\n";
  result.ok = true;
  result.source = std::move(text);
  return result;
}

std::optional<Gate> OpeningUniformReturn(std::string const& geometry) {
  size_t const mainAt = geometry.find("void main()");
  if (mainAt == std::string::npos) return std::nullopt;
  size_t const open = geometry.find('{', mainAt);
  if (open == std::string::npos) return std::nullopt;
  // Plain uniforms declared before main: name -> declaration line.
  std::map<std::string, std::string> uniforms;
  {
    std::istringstream head(geometry.substr(0, mainAt));
    static std::regex const decl(R"(^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)\s*(\[\d+\])?\s*;\s*$)");
    for (std::string line; std::getline(head, line);) {
      std::smatch m;
      if (std::regex_match(line, m, decl) && m[1] != "sampler2D") uniforms[m[2]] = line;
    }
  }
  std::istringstream body(geometry.substr(open + 1));
  std::vector<std::string> lines;
  for (std::string line; std::getline(body, line) && lines.size() < 64;) {
    size_t const first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    lines.push_back(line.substr(first));
  }
  static std::regex const declaration(R"(^(?:ivec4|uvec4|vec4|ivec3|vec3|ivec2|vec2|int|uint|float|bool|mat4)\s+\w.*;$)");
  size_t i = 0;
  while (i < lines.size() && std::regex_match(lines[i], declaration)) i++;
  if (i + 3 >= lines.size()) return std::nullopt;
  static std::regex const test(R"(^(r\d+)\.([xyzw]) = \((.+)\) \? -1 : 0;$)");
  std::smatch m;
  if (!std::regex_match(lines[i], m, test)) return std::nullopt;
  std::string const reg = m[1].str() + "." + m[2].str();
  std::string const condition = m[3];
  if (lines[i + 1] != "if (" + reg + " != 0) {" || lines[i + 2] != "return;" || lines[i + 3] != "}")
    return std::nullopt;
  // Only uniforms, literals and operators: nothing the vertex stage does not have.
  static std::set<std::string> const allowed = {"float", "int", "uint", "abs", "floor", "intBitsToFloat",
                                                "floatBitsToInt", "uintBitsToFloat", "floatBitsToUint"};
  Gate gate;
  gate.condition = condition;
  std::set<std::string> needed;
  for (size_t at = 0; at < condition.size();) {
    char const c = condition[at];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t end = at;
      while (end < condition.size() &&
             (std::isalnum(static_cast<unsigned char>(condition[end])) || condition[end] == '_'))
        end++;
      std::string const word = condition.substr(at, end - at);
      // A swizzle or member after a dot is not a name.
      bool const member = at > 0 && condition[at - 1] == '.';
      if (!member && !allowed.contains(word)) {
        auto it = uniforms.find(word);
        if (it == uniforms.end()) return std::nullopt;
        needed.insert(word);
      }
      at = end;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      // A literal, including 1.0, 9.99e-06 and 0x3f800000.
      while (at < condition.size() &&
             (std::isalnum(static_cast<unsigned char>(condition[at])) || condition[at] == '.' ||
              ((condition[at] == '-' || condition[at] == '+') && at > 0 &&
               (condition[at - 1] == 'e' || condition[at - 1] == 'E'))))
        at++;
    } else {
      at++;
    }
  }
  if (needed.empty()) return std::nullopt;
  for (auto const& name : needed) gate.declarations += uniforms[name] + "\n";
  return gate;
}

bool WithGate(std::string& vertex, Gate const& gate) {
  size_t const mainAt = vertex.find("void main()");
  if (mainAt == std::string::npos || vertex.find("vivify_gated_main") != std::string::npos) return false;
  size_t const end = vertex.rfind('}');
  if (end == std::string::npos || end < mainAt) return false;
  std::string declarations;
  std::istringstream lines(gate.declarations);
  static std::regex const name(R"(\b(\w+)\s*(\[\d+\])?\s*;\s*$)");
  for (std::string line; std::getline(lines, line);) {
    std::smatch m;
    if (!std::regex_search(line, m, name)) return false;
    // Already declared (as a uniform or anything else): the vertex stage's own wins.
    if (std::regex_search(vertex, std::regex("\\b" + m[1].str() + "\\b"))) continue;
    declarations += line + "\n";
  }
  vertex.insert(end + 1, "\nvoid main() {\n  vivify_gated_main();\n  if (" + gate.condition +
                             ") gl_Position = vec4(0.0);\n}\n");
  vertex.replace(mainAt, std::string("void main()").size(), "void vivify_gated_main()");
  vertex.insert(mainAt, declarations);
  return true;
}

}  // namespace Vivify::GeometryLowering
