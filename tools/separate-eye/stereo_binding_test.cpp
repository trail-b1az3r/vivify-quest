#include "VivifyStereoBindings.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace Vivify::StereoBindings;

int main() {
  int checks = 0;
  auto check = [&](bool ok, char const* description) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", description); std::exit(1); }
    ++checks;
  };
  auto program = [](std::string const& declarations, std::string const& body) {
    return "#ifdef VERTEX\n#version 300 es\n" + declarations + "\nvoid main() {\n" + body + "\n}\n#endif\n";
  };
  std::string declarations;
  for (unsigned i = 0; i < Names.size(); ++i)
    declarations += i < 10 ? "uniform vec4 hlslcc_mtx4x4" + std::string(Names[i]) + "[8];\n"
                          : "uniform vec4 " + std::string(Names[i]) + "[2];\n";
  check(UsedByProgram(program(declarations, "")) == 0, "declarations alone need no per-material uploads");
  check(UsedByProgram(program(declarations, "gl_Position = hlslcc_mtx4x4unity_StereoMatrixVP[0];")) == (1 << 2), "VP read does not imply V or P");
  check(UsedByProgram(program(declarations, "gl_Position = hlslcc_mtx4x4unity_StereoMatrixInvVP[0];")) == (1 << 5), "inverse VP does not imply inverse V or P");
  check(UsedByProgram(program(declarations, "gl_Position = unity_StereoWorldSpaceCameraPos[0];")) == (1 << 10), "camera-position array remains bound when read");
  check(UsedByProgram(program(declarations, "gl_Position = unity_StereoScaleOffset[1];")) == (1 << 11), "per-eye scale/offset remains bound when read");
  check(UsedByProgram(program("uniform mat4 unity_StereoMatrixV[2];", "gl_Position = unity_StereoMatrixV[0][0];")) == 1, "native matrix-array form remains bound when read");
  check(UsedByProgram(program("uniform mat4 unity_StereoMatrixV[2];", ""), 1) == 0, "metadata and recognized unused declaration permit omission");
  check(UsedByProgram(program("", "gl_Position=vec4(1.0);"), 1) == 1, "metadata without a recognized declaration preserves an unknown alias binding");
  check(UsedByProgram(program("", "gl_Position=vec4(1.0);"), All) == All, "unreadable parameter metadata keeps all unknown bindings");
  check(UsedByProgram(program("uniform vec4 hlslcc_mtx4x4unity_StereoMatrixVP[7];", "")) == (1 << 2), "unrecognized array shape keeps the binding");
  check(UsedByProgram(program("uniform vec4 custom_unity_StereoMatrixVP[8];", "")) != 0, "unknown identifier aliases are conservative");
  check(UsedByProgram(program("# define ALIAS unity_StereoMatrixVP", "")) == All, "spaced macro directive cannot hide a use");
  check(UsedByProgram(program("#include <opaque>", "")) == All, "includes prevent unknown external use from being omitted");
  check(UsedByProgram(program("// uniform vec4\n", "unity_StereoScaleOffset[2];")) == All, "a comment cannot masquerade as an unused declaration");
  check(UsedByProgram(program("/* hidden */", "")) == All, "unparsed comments keep every binding");
  check(UsedByProgram(program("uniform UnityStereoGlobals { mat4 view; };", "")) == All, "opaque engine stereo blocks retain existing bindings");
  check(UsedByProgram("truncated") == All, "incomplete code is never proof of unused uniforms");
  check(!Needs(All, 12) && !Needs(All, 100), "out-of-range properties cannot shift outside the mask");
  auto left = UsedByProgram(program(declarations, "gl_Position=hlslcc_mtx4x4unity_StereoMatrixV[0];"));
  auto right = UsedByProgram(program(declarations, "gl_Position=hlslcc_mtx4x4unity_StereoMatrixP[0];"));
  check((left | right) == 3, "variant union preserves all conditional uses");
  std::mt19937 random(0x5163);
  for (int trial = 0; trial < 5000; ++trial) {
    auto mutated = declarations;
    for (int n = 0; n < 4; ++n) mutated.insert(random() % (mutated.size() + 1), 1, static_cast<char>(random() & 127));
    auto mask = UsedByProgram(program(mutated, "gl_Position=hlslcc_mtx4x4unity_StereoMatrixVP[0];"));
    check(Needs(mask, 2), "perturbed declarations cannot suppress an unchanged live matrix read");
  }
  std::printf("%d stereo-binding proof and mutation checks passed\n", checks);
}
