// Host test for Vivify::ExtractZip: tools/ziptest/run_tests.py drives it.
#include "VivifyZip.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: ziptest <zip> <dest>\n";
    return 2;
  }
  std::ifstream is(argv[1], std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  std::string error;
  if (!Vivify::ExtractZip(data, argv[2], error)) {
    std::cout << "error: " << error << "\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
