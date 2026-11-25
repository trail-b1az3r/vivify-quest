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
  if (argc != 3) return 2;
  auto result = Vivify::GeometryLowering::LowerTriangleGeometry(Slurp(argv[1]), Slurp(argv[2]));
  if (!result.ok) {
    std::cerr << "refused: " << result.reason << "\n";
    return 1;
  }
  std::cout << result.source;
  return 0;
}
