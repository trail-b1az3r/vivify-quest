// Lowers a vertex + geometry GLSL pair and prints the lowered vertex program.
#include <fstream>
#include <iostream>
#include <sstream>

#include "VivifyGeometryLowering.hpp"

static std::string Slurp(char const* path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main(int argc, char** argv) {
  // --gate <vertex> <geometry>: the vertex program with the geometry
  // program's opening uniform check (0.16.4).
  if (argc == 4 && std::string(argv[1]) == "--gate") {
    std::string vertex = Slurp(argv[2]);
    auto gate = Vivify::GeometryLowering::OpeningUniformReturn(Slurp(argv[3]));
    if (!gate) {
      std::cerr << "no gate\n";
      return 1;
    }
    if (!Vivify::GeometryLowering::WithGate(vertex, *gate)) {
      std::cerr << "not gated\n";
      return 1;
    }
    std::cout << vertex;
    return 0;
  }
  if (argc != 3) return 2;
  auto result = Vivify::GeometryLowering::LowerTriangleGeometry(Slurp(argv[1]), Slurp(argv[2]));
  if (!result.ok) {
    std::cerr << "refused: " << result.reason << "\n";
    return 1;
  }
  std::cout << result.source;
  return 0;
}
