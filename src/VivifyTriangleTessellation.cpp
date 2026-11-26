#include "VivifyTriangleTessellation.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace Vivify::Dxbc {
namespace {
void Require(bool ok, char const* why) { if (!ok) throw std::runtime_error(why); }
std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool Identifier(std::string const& s) {
  auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  return !s.empty() && s.size() <= 256 && letter(s.front()) &&
      std::all_of(s.begin(), s.end(), [&](unsigned char c) { return letter(c) || (c >= '0' && c <= '9'); });
}
std::string Name(SignatureElement const& s) {
  Require(Identifier(s.semanticName), "unsupported tessellation semantic identifier");
  return s.semanticName + std::to_string(s.semanticIndex);
}
bool Plain(Operand const& o) { return o.modifier == Modifier::None && o.minPrecision == 0 && o.immediates.empty(); }
bool Index(OperandIndex const& i, uint32_t value) {
  return i.hasImmediate && !i.hasRelative && !i.relative && i.immediate == value;
}
bool Register(Operand const& o, uint32_t type, uint32_t index, uint8_t mask) {
  return Plain(o) && o.type == type && o.numComponents == 4 && o.selectionMode == 0 && o.mask == mask &&
      o.indices.size() == 1 && Index(o.indices[0], index);
}
unsigned Component(Operand const& o, unsigned c) {
  Require(Plain(o) && o.numComponents == 4 && (o.selectionMode == 1 || o.selectionMode == 2), "unsupported tessellation source selection");
  unsigned const result = o.selectionMode == 2 ? o.swizzle[0] : o.swizzle[c];
  Require(result < 4, "invalid tessellation swizzle");
  return result;
}
bool ScalarX(Operand const& o) {
  return Plain(o) && o.numComponents == 4 && (o.selectionMode == 1 || o.selectionMode == 2) &&
      Component(o, 0) == 0;
}
using Signature = std::map<uint32_t, SignatureElement const*>;
Signature CheckSignature(std::vector<SignatureElement> const& signature) {
  Require(!signature.empty() && signature.size() <= 32, "missing or excessive control-point signature");
  Signature result; std::set<std::string> names; int positions = 0;
  for (auto const& s : signature) {
    Require(s.registerIndex < 32 && s.stream == 0 && s.mask && s.mask <= 15 &&
        (s.componentType == 1 || s.componentType == 3) && (s.systemValueType == 0 || s.systemValueType == 1),
        "unsupported control-point signature element");
    Require(result.emplace(s.registerIndex, &s).second && names.insert(Name(s)).second, "duplicate tessellation signature element");
    if (s.systemValueType == 1) { ++positions; Require(s.mask == 15, "position must carry all four components"); }
  }
  Require(positions == 1, "control points require exactly one position");
  return result;
}
void Match(Signature const& a, Signature const& b) {
  Require(a.size() == b.size(), "control-point interfaces differ");
  for (auto const& [reg, s] : a) {
    auto at = b.find(reg); Require(at != b.end(), "control-point register missing"); auto* t = at->second;
    Require(Lower(s->semanticName) == Lower(t->semanticName) && s->semanticIndex == t->semanticIndex &&
        s->mask == t->mask && s->componentType == t->componentType && s->systemValueType == t->systemValueType,
        "control-point passthrough cannot be proved");
    if (s->systemValueType == 0) Require(Name(*s) == Name(*t), "varying names differ between tessellation stages");
  }
}
void CheckPatchSignature(Program const& p) {
  Require(p.patchSignature.size() == 4, "triangle patch factor signature required");
  std::set<unsigned> seen;
  for (auto const& s : p.patchSignature) {
    Require(s.registerIndex < 4 && seen.insert(s.registerIndex).second && s.mask == 1 && s.componentType == 3 && s.stream == 0 &&
        s.systemValueType == (s.registerIndex == 3 ? 14u : 13u) &&
        s.semanticIndex == (s.registerIndex == 3 ? 0u : s.registerIndex), "unsupported triangle patch factors");
  }
}
bool Clean(Instruction const& i) { return !i.extended && i.extendedTokens.empty() && !i.hasSampleOffsets; }
std::string Factor(Program const& hull, ExternalReflection const* reflection) {
  constexpr std::array<unsigned, 23> pattern = {113,147,148,149,150,151,106,89,115,153,95,103,103,103,104,91,54,54,62,115,103,54,62};
  Require(hull.instructions.size() == pattern.size(), "unsupported hull phase structure");
  for (size_t n = 0; n < pattern.size(); ++n) {
    auto const& i = hull.instructions[n];
    Require(i.opcode == pattern[n] && Clean(i), "unsupported hull phase instruction");
    if (n != 1 && n != 2 && n != 3 && n != 4 && n != 5 && n != 6)
      Require(i.controls == 0, "unsupported hull instruction controls");
  }
  auto const& is = hull.instructions;
  for (unsigned n : {0u,1u,2u,3u,4u,5u,6u,8u,18u,19u,22u})
    Require(is[n].operands.empty() && is[n].extra.empty(), "unexpected hull declaration payload");
  Require(is[1].controls == 3 && is[2].controls == 3 && is[3].controls == 2 && is[4].controls == 1 &&
      is[5].controls == 3 && is[6].controls <= 1 && is[0].controls == 0 && is[8].controls == 0 &&
      is[19].controls == 0 && hull.tempCount == 1, "only three-point integer clockwise patches are supported");
  Require(is[9].operands.empty() && is[9].extra == std::vector<uint32_t>{3} &&
      is[14].operands.empty() && is[14].extra == std::vector<uint32_t>{1}, "unsupported hull fork instance or temp count");
  Require(is[10].operands.size() == 1 && is[10].extra.empty() && Plain(is[10].operands[0]) &&
      is[10].operands[0].type == kOperandInputForkInstanceID && is[10].operands[0].indices.empty() &&
      (is[10].operands[0].numComponents == 0 ||
       ((is[10].operands[0].numComponents == 1 || is[10].operands[0].numComponents == 4) && is[10].operands[0].mask == 1)),
      "hull fork index declaration missing");
  for (unsigned reg = 0; reg < 4; ++reg) {
    auto const& decl = is[reg < 3 ? 11 + reg : 20];
    Require(decl.operands.size() == 1 && Register(decl.operands[0], kOperandOutput, reg, 1) &&
        // Token-stream SIV names distinguish each triangle edge (17..19)
        // and inside (20); PCSG's D3D_NAME values are instead 13 and 14.
        decl.extra == std::vector<uint32_t>{17u + reg}, "hull factor output declaration differs");
  }
  Require(is[15].operands.size() == 1 && Register(is[15].operands[0], kOperandOutput, 0, 1) &&
      is[15].extra == std::vector<uint32_t>{3}, "hull factor index range differs");
  for (unsigned n : {16u,17u,21u}) Require(is[n].operands.size() == 2 && is[n].extra.empty(), "invalid hull factor assignment");
  auto const& indexWrite = is[16];
  Require(Register(indexWrite.operands[0], kOperandTemp, 0, 1) && indexWrite.operands[1].type == kOperandInputForkInstanceID &&
      indexWrite.operands[1].indices.empty() && ScalarX(indexWrite.operands[1]), "fork index is not passed through");
  auto const& dynamic = is[17].operands[0];
  Require(Plain(dynamic) && dynamic.type == kOperandOutput && dynamic.numComponents == 4 && dynamic.selectionMode == 0 &&
      dynamic.mask == 1 && dynamic.indices.size() == 1, "unsupported dynamic tessellation factor output");
  auto const& index = dynamic.indices[0];
  Require(index.hasRelative && index.immediate == 0 && index.relative && index.relative->type == kOperandTemp &&
      index.relative->indices.size() == 1 && Index(index.relative->indices[0], 0) && ScalarX(*index.relative), "factor output does not use the fork index");
  Require(Register(is[21].operands[0], kOperandOutput, 3, 1), "inside factor output differs");
  auto const& read = is[17].operands[1]; auto const& inside = is[21].operands[1];
  Require(Plain(read) && Plain(inside) && Component(read, 0) == Component(inside, 0) && read.type == kOperandConstantBuffer && inside.type == read.type &&
      read.indices.size() == 2 && inside.indices.size() == 2, "factors must be uniform constant-buffer scalars");
  Require(read.indices[0].immediate < 16 && read.indices[1].immediate < (1u << 20), "factor address out of range");
  unsigned const bind = static_cast<unsigned>(read.indices[0].immediate), row = static_cast<unsigned>(read.indices[1].immediate);
  unsigned const byteOffset = row * 16u + 4u * Component(read, 0);
  for (auto const* operand : {&read, &inside})
    Require(Index(operand->indices[0], bind) && Index(operand->indices[1], row), "outer and inner tessellation factors differ");
  Require(is[7].extra.empty() && is[7].operands.size() == 1 && is[7].operands[0].type == kOperandConstantBuffer &&
      Plain(is[7].operands[0]) && is[7].operands[0].indices.size() == 2 && Index(is[7].operands[0].indices[0], bind) &&
      is[7].operands[0].indices[1].immediate > row && is[7].operands[0].indices[1].hasImmediate &&
      !is[7].operands[0].indices[1].hasRelative, "factor exceeds declared constant buffer");
  auto const& buffers = hull.constantBuffers.empty() && reflection ? reflection->constantBuffers : hull.constantBuffers;
  std::string name; int matches = 0;
  for (auto const& b : buffers) if (b.bindPoint == bind && b.size >= byteOffset + 4u) {
    for (auto const& v : b.variables) if (v.startOffset == byteOffset && v.size == 4 && v.variableClass == 0 &&
        v.variableType == 3 && v.rows == 1 && v.columns == 1 && v.elements == 0) { name = v.name; ++matches; }
  }
  Require(matches == 1 && Identifier(name) && name.front() == '_' && !name.starts_with("__"), "factor needs unambiguous scalar material reflection");
  return name;
}


// Bytesized/Grass (Lifelike, 0.16.5): each edge's factor from its world-space
// length over the camera's distance to its midpoint,
//   min(length / (distance / D), M) + A
// with D, M, A material scalars, and the inside factor the mean of the three
// edges. No control-point phase. Matched instruction by instruction; the
// matrix and camera position are proved by the hull's reflection names.
struct EdgeFactorNames { std::string divisor, maximum, offset, spacing; };
std::string ReflectedName(Program const& hull, ExternalReflection const* reflection, unsigned bind, unsigned byteOffset,
                          unsigned minSize) {
  auto const& buffers = hull.constantBuffers.empty() && reflection ? reflection->constantBuffers : hull.constantBuffers;
  std::string name; int matches = 0;
  for (auto const& b : buffers) if (b.bindPoint == bind)
    for (auto const& v : b.variables) if (v.startOffset == byteOffset && v.size >= minSize) { name = v.name; ++matches; }
  Require(matches == 1 && Identifier(name), "edge factor needs unambiguous reflection");
  return name;
}
EdgeFactorNames EdgeFactors(Program const& hull, ExternalReflection const* reflection) {
  constexpr std::array<unsigned, 52> pattern = {
      113,147,148,149,150,151,106,89,89,89,115,153,95,95,103,103,103,104,91,30,78,56,50,50,0,56,50,50,0,0,0,16,
      50,16,75,14,14,51,0,54,54,62,116,95,95,95,103,104,0,0,56,62};
  Require(hull.instructions.size() == pattern.size(), "unsupported hull phase structure");
  auto const& is = hull.instructions;
  for (size_t n = 0; n < pattern.size(); ++n) Require(is[n].opcode == pattern[n] && Clean(is[n]), "unsupported hull phase instruction");
  // Partitioning: 1 integer, 3 fractional odd, 4 fractional even.
  Require(is[1].controls == 3 && is[2].controls == 3 && is[3].controls == 2 &&
      (is[4].controls == 1 || is[4].controls == 3 || is[4].controls == 4) &&
      is[5].controls == 3 && is[6].controls <= 1 && hull.tempCount == 3, "only three-point clockwise triangle patches are supported");
  std::string const spacing = is[4].controls == 3 ? "fractional_odd_spacing" :
      is[4].controls == 4 ? "fractional_even_spacing" : "equal_spacing";
  Require(is[11].extra == std::vector<uint32_t>{3} && is[47].extra == std::vector<uint32_t>{1}, "unsupported hull fork instance or temp count");
  for (unsigned reg = 0; reg < 4; ++reg) {
    auto const& decl = is[reg < 3 ? 14 + reg : 46];
    Require(decl.operands.size() == 1 && Register(decl.operands[0], kOperandOutput, reg, 1) &&
        decl.extra == std::vector<uint32_t>{17u + reg}, "hull factor output declaration differs");
  }
  auto cb = [&](size_t n, size_t operand) -> Operand const& {
    Require(is[n].operands.size() > operand && is[n].operands[operand].type == kOperandConstantBuffer &&
        is[n].operands[operand].indices.size() == 2 && !is[n].operands[operand].indices[0].hasRelative &&
        !is[n].operands[operand].indices[1].hasRelative, "edge factor operand is not a constant");
    return is[n].operands[operand];
  };
  // World transform rows 1, 0, 2, 3 for both corners.
  unsigned const matrixBind = static_cast<unsigned>(cb(21, 1).indices[0].immediate);
  unsigned const matrixRow = static_cast<unsigned>(cb(22, 1).indices[1].immediate);
  for (auto [n, row] : std::array<std::pair<size_t, unsigned>, 8>{{{21,1},{22,0},{23,2},{24,3},{25,1},{26,0},{27,2},{28,3}}}) {
    auto const& o = cb(n, n == 24 || n == 28 ? 2 : 1);
    Require(o.indices[0].immediate == matrixBind && o.indices[1].immediate == matrixRow + row, "edge factor world transform differs");
  }
  Require(ReflectedName(hull, reflection, matrixBind, matrixRow * 16, 64) == "unity_ObjectToWorld", "edge factor does not use the object-to-world matrix");
  auto const& camera = cb(32, 3);
  std::string const cameraName = ReflectedName(hull, reflection, static_cast<unsigned>(camera.indices[0].immediate),
                                               static_cast<unsigned>(camera.indices[1].immediate) * 16, 12);
  Require(cameraName == "_WorldSpaceCameraPos" || cameraName == "unity_StereoWorldSpaceCameraPos", "edge factor does not use the camera position");
  auto scalar = [&](size_t n, size_t operand) {
    auto const& o = cb(n, operand);
    Require(o.selectionMode == 2, "edge factor material value must be a scalar");
    unsigned const component = Component(o, 0);
    std::string name = ReflectedName(hull, reflection, static_cast<unsigned>(o.indices[0].immediate),
                                     static_cast<unsigned>(o.indices[1].immediate) * 16 + component * 4, 4);
    Require(name.front() == '_' && !name.starts_with("__") && !name.starts_with("unity_"), "edge factor material value is not a material property");
    return name;
  };
  EdgeFactorNames names{scalar(35, 2), scalar(37, 2), scalar(38, 2), spacing};
  // The join: (e0 + e1 + e2) * 1/3 into the inside factor.
  Require(is[50].operands.size() == 3 && Register(is[50].operands[0], kOperandOutput, 3, 1) &&
      is[50].operands[2].type == kOperandImmediate32, "inside factor is not the edge mean");
  return names;
}

std::string Evaluation(Program const& domain, Signature const& inputs, Signature const& outputs,
                       std::string const& spacing = "equal_spacing") {
  Require(domain.instructions.size() <= 512 && domain.tempCount <= 64 && domain.indexableTemps.empty() &&
      domain.constantBuffers.empty() && domain.resourceBindings.empty() && domain.immediateConstantBuffer.empty(), "unsupported domain resources or size");
  std::map<std::pair<unsigned,unsigned>, std::string> temps, written;
  std::set<unsigned> declaredInputs, declaredOutputs;
  bool count = false, layout = false, flags = false, tempsDeclared = false, coords = false, bodyStarted = false, ended = false;
  std::string body; unsigned serial = 0;
  auto source = [&](Operand const& o, unsigned lane) {
    unsigned const component = Component(o, lane); char const letter = "xyzw"[component];
    if (o.type == kOperandInputDomainPoint) {
      Require(o.indices.empty() && component < 3 && coords, "invalid tessellation coordinate read");
      return std::string("gl_TessCoord.") + letter;
    }
    if (o.type == kOperandInputControlPoint) {
      Require(o.indices.size() == 2 && o.indices[0].immediate < 3 && o.indices[1].immediate < 32, "invalid control-point read");
      unsigned const cp = static_cast<unsigned>(o.indices[0].immediate), reg = static_cast<unsigned>(o.indices[1].immediate);
      Require(Index(o.indices[0], cp) && Index(o.indices[1], reg), "dynamic control points not supported");
      auto at = inputs.find(reg); Require(at != inputs.end() && declaredInputs.contains(reg) && (at->second->mask & (1 << component)), "undeclared control-point component");
      return (at->second->systemValueType == 1 ? "gl_in[" + std::to_string(cp) + "].gl_Position" :
          "tc_" + Name(*at->second) + "[" + std::to_string(cp) + "]") + "." + letter;
    }
    Require(o.type == kOperandTemp && o.indices.size() == 1 && o.indices[0].immediate < domain.tempCount &&
        Index(o.indices[0], static_cast<unsigned>(o.indices[0].immediate)), "unsupported domain source");
    auto at = temps.find({static_cast<unsigned>(o.indices[0].immediate), component});
    Require(at != temps.end(), "domain reads an uninitialized temporary"); return at->second;
  };
  for (auto const& i : domain.instructions) {
    Require(Clean(i) && !ended, "domain has extended instructions or code after return");
    if (i.opcode == 147 || i.opcode == 149 || i.opcode == 106 || i.opcode == 104 || i.opcode == 95 || i.opcode == 101 || i.opcode == 103) {
      Require(!bodyStarted, "domain declaration follows executable code");
      if (i.opcode == 147 || i.opcode == 149 || i.opcode == 106) {
        Require(i.operands.empty() && i.extra.empty(), "unexpected domain layout payload");
        if (i.opcode == 147) { Require(!count && i.controls == 3, "domain requires three control points"); count = true; }
        if (i.opcode == 149) { Require(!layout && i.controls == 2, "domain must be triangular"); layout = true; }
        if (i.opcode == 106) { Require(!flags && i.controls <= 1, "unsupported domain global flags"); flags = true; }
      } else if (i.opcode == 104) {
        Require(!tempsDeclared && i.controls == 0 && i.operands.empty() && i.extra == std::vector<uint32_t>{domain.tempCount}, "invalid domain temporary declaration"); tempsDeclared = true;
      } else {
        Require(i.controls == 0 && i.operands.size() == 1, "invalid domain I/O declaration"); auto const& o = i.operands[0];
        Require(Plain(o) && o.numComponents == 4 && o.selectionMode == 0, "invalid domain declaration selection");
        if (i.opcode == 95 && o.type == kOperandInputDomainPoint) {
          Require(!coords && o.indices.empty() && o.mask == 7 && i.extra.empty(), "invalid domain coordinate declaration"); coords = true;
        } else if (i.opcode == 95) {
          Require(o.type == kOperandInputControlPoint && o.indices.size() == 2 && Index(o.indices[0], 3) && o.indices[1].immediate < 32 &&
              Index(o.indices[1], static_cast<unsigned>(o.indices[1].immediate)) && i.extra.empty(), "invalid domain control-point declaration");
          unsigned const reg = static_cast<unsigned>(o.indices[1].immediate); auto at = inputs.find(reg);
          Require(at != inputs.end() && o.mask == at->second->mask && declaredInputs.insert(reg).second, "domain input signature differs from declaration");
        } else {
          Require(o.type == kOperandOutput && o.indices.size() == 1 && o.indices[0].immediate < 32 &&
              Index(o.indices[0], static_cast<unsigned>(o.indices[0].immediate)), "invalid domain output declaration");
          unsigned const reg = static_cast<unsigned>(o.indices[0].immediate); auto at = outputs.find(reg);
          Require(at != outputs.end() && o.mask == at->second->mask && declaredOutputs.insert(reg).second &&
              (i.opcode == 103 ? i.extra == std::vector<uint32_t>{at->second->systemValueType} : i.extra.empty() && at->second->systemValueType == 0), "domain output signature differs from declaration");
        }
      }
      continue;
    }
    bodyStarted = true;
    Require(count && layout && coords && tempsDeclared && i.controls == 0 && i.extra.empty(), "domain layouts missing or instruction controls unsupported");
    if (i.opcode == 62) { Require(i.operands.empty(), "invalid domain return"); ended = true; continue; }
    unsigned const arity = i.opcode == 54 ? 2 : (i.opcode == 56 ? 3 : (i.opcode == 50 ? 4 : 0));
    Require(arity && i.operands.size() == arity, "domain supports MOV, MUL and MAD only");
    auto const& dest = i.operands[0];
    Require(Plain(dest) && dest.numComponents == 4 && dest.selectionMode == 0 && dest.mask && dest.mask <= 15 &&
        dest.indices.size() == 1 && dest.indices[0].immediate < 64 && Index(dest.indices[0], static_cast<unsigned>(dest.indices[0].immediate)), "invalid domain destination");
    unsigned const reg = static_cast<unsigned>(dest.indices[0].immediate);
    if (dest.type == kOperandTemp) Require(reg < domain.tempCount, "domain temporary exceeds declaration");
    else Require(dest.type == kOperandOutput && outputs.contains(reg) && declaredOutputs.contains(reg) &&
        (dest.mask & ~outputs.at(reg)->mask) == 0, "invalid domain output write");
    std::array<std::string,4> values;
    for (unsigned c = 0; c < 4; ++c) if (dest.mask & (1 << c)) {
      auto expression = source(i.operands[1], c);
      if (arity >= 3) expression = "(" + expression + " * " + source(i.operands[2], c) + ")";
      if (arity == 4) expression = "(" + expression + " + " + source(i.operands[3], c) + ")";
      values[c] = "tess_value" + std::to_string(serial++);
      body += "  float " + values[c] + " = " + expression + ";\n";
    }
    // Read every source before publishing any destination component: vector
    // register writes are simultaneous, including r0.xy <- r0.yx.
    auto& into = dest.type == kOperandTemp ? temps : written;
    for (unsigned c = 0; c < 4; ++c) if (dest.mask & (1 << c)) into[{reg,c}] = values[c];
  }
  Require(ended && declaredInputs.size() == inputs.size() && declaredOutputs.size() == outputs.size(), "incomplete domain program");
  std::string declarations = "#version 320 es\nprecision highp float;\nprecision highp int;\nlayout(triangles, " + spacing + ", cw) in;\n";
  for (auto const& [reg, s] : inputs) if (s->systemValueType == 0) declarations += "in vec4 tc_" + Name(*s) + "[];\n";
  for (auto const& [reg, s] : outputs) {
    if (s->systemValueType == 0) declarations += "out vec4 vs_" + Name(*s) + ";\n";
    for (unsigned c = 0; c < 4; ++c) if (s->mask & (1 << c)) {
      auto at = written.find({reg,c}); Require(at != written.end(), "domain leaves an output component undefined");
      body += "  " + (s->systemValueType == 1 ? std::string("gl_Position") : "vs_" + Name(*s)) + "." + "xyzw"[c] + " = " + at->second + ";\n";
    }
  }
  return declarations + "void main() {\n" + body + "}\n";
}
}

TriangleTessellationResult TranslateTriangleTessellation(Program const& hull, Program const& domain, ExternalReflection const* reflection) {
  TriangleTessellationResult result;
  try {
    Require(hull.ok && domain.ok && hull.stage == Stage::Hull && domain.stage == Stage::Domain &&
        hull.majorVersion == 5 && hull.minorVersion == 0 && domain.majorVersion == 5 && domain.minorVersion == 0, "expected parsed shader-model-5 hull/domain pair");
    Require(std::all_of(hull.resourceBindings.begin(), hull.resourceBindings.end(), [](auto const& r) { return r.type == 0; }) &&
        hull.indexableTemps.empty() && hull.immediateConstantBuffer.empty(), "unsupported hull resources");
    auto inputs = CheckSignature(hull.inputSignature), outputs = CheckSignature(hull.outputSignature);
    Match(inputs, outputs); Match(outputs, CheckSignature(domain.inputSignature));
    auto evaluated = CheckSignature(domain.outputSignature);
    CheckPatchSignature(hull); CheckPatchSignature(domain);
    std::optional<EdgeFactorNames> edges;
    try {
      result.factorUniform = Factor(hull, reflection);
    } catch (std::exception const& uniformFailure) {
      std::string const uniformReason = uniformFailure.what();
      try {
        edges = EdgeFactors(hull, reflection);
      } catch (std::exception const& edgeFailure) {
        // Both shapes refused: the uniform pattern's reason, as before, and the edge pattern's.
        throw std::runtime_error(uniformReason + " (edge-length factors: " + edgeFailure.what() + ")");
      }
    }
    std::string declarations = "#version 320 es\nprecision highp float;\nprecision highp int;\nlayout(vertices = 3) out;\n";
    if (!edges) declarations += "uniform highp float " + result.factorUniform + ";\n";
    std::string body = "void main() {\n";
    for (auto const& [reg, s] : inputs) {
      if (s->systemValueType == 1) body += "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n";
      else {
        declarations += "in vec4 vs_" + Name(*s) + "[];\nout vec4 tc_" + Name(*s) + "[];\n";
        body += "  tc_" + Name(*s) + "[gl_InvocationID] = vs_" + Name(*s) + "[gl_InvocationID];\n";
      }
    }
    body += "  if (gl_InvocationID == 0) {\n";
    if (!edges) {
      for (int edge = 0; edge < 3; ++edge) body += "    gl_TessLevelOuter[" + std::to_string(edge) + "] = " + result.factorUniform + ";\n";
      body += "    gl_TessLevelInner[0] = " + result.factorUniform + ";\n  }\n}\n";
    } else {
      // Edge i joins corners i+1 and i+2, as the fork phase indexes them.
      auto position = inputs.find(0);
      Require(position != inputs.end() && (position->second->mask & 7) == 7, "edge factor position input missing");
      std::string const corner = position->second->systemValueType == 1 ? "gl_in[c].gl_Position" : "vs_" + Name(*position->second) + "[c]";
      declarations += "uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];\nuniform vec3 _WorldSpaceCameraPos;\n";
      for (auto const* name : {&edges->divisor, &edges->maximum, &edges->offset})
        if (declarations.find(" " + *name + ";") == std::string::npos) declarations += "uniform highp float " + *name + ";\n";
      declarations += "vec3 vivify_corner_world(int c) {\n  vec4 p = " + corner + ";\n"
          "  return (hlslcc_mtx4x4unity_ObjectToWorld[1] * p.y + hlslcc_mtx4x4unity_ObjectToWorld[0] * p.x +\n"
          "      hlslcc_mtx4x4unity_ObjectToWorld[2] * p.z + hlslcc_mtx4x4unity_ObjectToWorld[3]).xyz;\n}\n"
          "float vivify_edge_factor(int a, int b) {\n"
          "  vec3 wa = vivify_corner_world(a), wb = vivify_corner_world(b);\n"
          "  float distance = length((wa + wb) * 0.5 - _WorldSpaceCameraPos) / " + edges->divisor + ";\n"
          "  return min(length(wa - wb) / distance, " + edges->maximum + ") + " + edges->offset + ";\n}\n";
      body += "    float e0 = vivify_edge_factor(1, 2), e1 = vivify_edge_factor(2, 0), e2 = vivify_edge_factor(0, 1);\n"
              "    gl_TessLevelOuter[0] = e0;\n    gl_TessLevelOuter[1] = e1;\n    gl_TessLevelOuter[2] = e2;\n"
              "    gl_TessLevelInner[0] = (e0 + e1 + e2) * 0.333333343;\n  }\n}\n";
      result.factorUniform = edges->maximum;
    }
    auto evaluation = Evaluation(domain, CheckSignature(domain.inputSignature), evaluated,
                                 edges ? edges->spacing : std::string("equal_spacing"));
    result.control.source = declarations + body; result.control.uniforms = {result.factorUniform};
    result.evaluation.source = std::move(evaluation);
    result.control.ok = result.evaluation.ok = true; result.control.version = result.evaluation.version = 320;
    result.ok = true;
  } catch (std::exception const& ex) {
    result = {}; result.error = ex.what();
  }
  return result;
}
}
