// Host harness for the DXBC -> GLSL translator.
//
// There is no way to run a Quest from here and no DirectX compiler on this
// host, so the translator is exercised against hand-assembled bytecode (see
// mkdxbc.py) through this tool. Every mode prints one field per line so the
// test harness never has to guess where a value ends.
//
// Build (matching CI):
//   g++ -std=gnu++20 -fsanitize=address,undefined -g -I include \
//       tools/dxbc/main.cpp src/VivifyDxbc.cpp src/VivifyTriangleTessellation.cpp -o /tmp/dxbctool
//
// Modes:
//   dxbctool parse  <file>   container, signatures, reflection
//   dxbctool disasm <file>   one line per decoded instruction
//   dxbctool glsl   <file>   translated GLSL ES, or the reason there is none
#include "VivifyDxbc.hpp"
#include "VivifyGeometrySpecialization.hpp"
#include "VivifyTriangleTessellation.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadFile(char const* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

void PrintProgram(Vivify::Dxbc::Program const& program) {
  std::cout << "ok=" << (program.ok ? 1 : 0) << "\n";
  if (!program.error.empty()) std::cout << "error=" << program.error << "\n";
  if (!program.ok) return;
  std::cout << "stage=" << Vivify::Dxbc::StageName(program.stage) << "\n";
  std::cout << "version=" << program.majorVersion << "." << program.minorVersion << "\n";
  std::cout << "containerOffset=" << program.containerOffset << "\n";
  std::cout << "instructions=" << program.instructions.size() << "\n";
  std::cout << "temps=" << program.tempCount << "\n";
  std::cout << "creator=" << program.creator << "\n";
  for (auto const& element : program.inputSignature) {
    std::cout << "input=" << element.semanticName << " index=" << element.semanticIndex
              << " reg=" << element.registerIndex << " sv=" << element.systemValueType
              << " mask=" << static_cast<int>(element.mask) << "\n";
  }
  for (auto const& element : program.outputSignature) {
    std::cout << "output=" << element.semanticName << " index=" << element.semanticIndex
              << " reg=" << element.registerIndex << " sv=" << element.systemValueType
              << " mask=" << static_cast<int>(element.mask) << "\n";
  }
  for (auto const& buffer : program.constantBuffers) {
    std::cout << "cbuffer=" << buffer.name << " size=" << buffer.size
              << " bind=" << buffer.bindPoint << " variables=" << buffer.variables.size() << "\n";
    for (auto const& variable : buffer.variables) {
      std::cout << "cbvar=" << variable.name << " offset=" << variable.startOffset
                << " size=" << variable.size << " class=" << variable.variableClass
                << " type=" << variable.variableType << " rows=" << variable.rows
                << " columns=" << variable.columns << " elements=" << variable.elements << "\n";
    }
  }
  for (auto const& binding : program.resourceBindings) {
    std::cout << "binding=" << binding.name << " type=" << binding.type
              << " dimension=" << binding.dimension << " bind=" << binding.bindPoint << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Offline prototype only. Optional scalar reflection is explicit input,
  // checked against the bytecode's actual constant-buffer address. Real-map
  // audits must obtain it from that hull program's Unity parameter table.
  if ((argc == 5 || argc == 8) && std::string(argv[1]) == "tess-pair") {
    auto h = ReadFile(argv[2]), d = ReadFile(argv[3]);
    Vivify::Dxbc::ExternalReflection reflection;
    try {
      if (argc == 8) {
        auto number = [](char const* text) {
          std::string s(text); size_t end = 0; auto n = std::stoul(s, &end);
          if (end != s.size() || n > 1048576 || s.starts_with('-')) throw std::runtime_error("invalid scalar reflection address");
          return static_cast<uint32_t>(n);
        };
        Vivify::Dxbc::ConstantBufferInfo b; b.bindPoint = number(argv[5]);
        Vivify::Dxbc::ConstantBufferVariable v; v.startOffset = number(argv[6]);
        v.name = argv[7]; v.size = 4; v.rows = v.columns = 1; v.variableType = 3;
        b.size = v.startOffset + 4; b.variables.push_back(v); reflection.constantBuffers.push_back(b);
      }
      auto result = Vivify::Dxbc::TranslateTriangleTessellation(
          Vivify::Dxbc::ParseProgram(h.data(), h.size()), Vivify::Dxbc::ParseProgram(d.data(), d.size()),
          argc == 8 ? &reflection : nullptr);
      std::cout << "ok=" << result.ok << "\nerror=" << result.error << "\nfactor=" << result.factorUniform << "\n";
      if (result.ok) {
        std::ofstream control(std::string(argv[4]) + ".tesc"), evaluation(std::string(argv[4]) + ".tese");
        control << result.control.source; evaluation << result.evaluation.source;
        if (!control || !evaluation) return 2;
      }
      return 0;
    } catch (std::exception const& ex) { std::cerr << ex.what() << "\n"; return 2; }
  }
  if (argc == 2 && std::string(argv[1]) == "native-empty-program") {
    std::cout << Vivify::GeometrySpecialization::NativeEmptyProgram();
    return 0;
  }
  if (argc < 3) {
    std::cerr << "usage: dxbctool <parse|disasm|glsl|glsl-mv|glsl-eye-left|glsl-eye-right> <file>\n";
    return 2;
  }
  std::string const mode = argv[1];
  std::vector<uint8_t> const data = ReadFile(argv[2]);

  Vivify::Dxbc::Program const program =
      Vivify::Dxbc::ParseProgram(data.empty() ? nullptr : data.data(), data.size());

  if (mode == "parse") {
    PrintProgram(program);
    return 0;
  }
  if (mode == "disasm") {
    std::cout << "ok=" << (program.ok ? 1 : 0) << "\n";
    if (!program.error.empty()) std::cout << "error=" << program.error << "\n";
    if (program.ok) std::cout << Vivify::Dxbc::DisassembleProgram(program);
    return 0;
  }
  if (mode == "glsl" || mode == "glsl-mv" || mode == "glsl-mv-spi" ||
      mode == "glsl-eye-left" || mode == "glsl-eye-right" ||
      mode == "glsl-eye-left-spi" || mode == "glsl-eye-right-spi" ||
      mode == "glsl-eye-left-blit" || mode == "glsl-eye-right-blit") {
    Vivify::Dxbc::GlslOptions options;
    options.multiview = mode == "glsl-mv" || mode == "glsl-mv-spi";
    options.stereoInstancedProgram = mode == "glsl-mv-spi" || mode == "glsl-eye-left-spi" ||
                                    mode == "glsl-eye-right-spi";
    if (mode.starts_with("glsl-eye-left")) options.singleViewStereoEye = 0;
    if (mode.starts_with("glsl-eye-right")) options.singleViewStereoEye = 1;
    if (options.singleViewStereoEye >= 0)
      options.singleViewTextures = {"_CameraDepthTexture", "_CameraDepthNormalsTexture", "_CameraMotionVectorsTexture",
                                    "_CameraOpaqueTexture", "_GrabTexture"};
    if (mode.ends_with("-blit")) options.singleViewTextures.push_back("_MainTex");
    options.geometryRightEyeCopy = std::getenv("VIVIFY_GEOMETRY_EYE_COPY") != nullptr;
    Vivify::Dxbc::GlslResult const result = Vivify::Dxbc::TranslateToGlsl(program, options);
    std::cout << "ok=" << (result.ok ? 1 : 0) << "\n";
    std::cout << "stereoInstanced=" << (result.stereoInstanced ? 1 : 0) << "\n";
    if (!result.error.empty()) std::cout << "error=" << result.error << "\n";
    for (auto const& name : result.uniforms) std::cout << "uniform=" << name << "\n";
    for (auto const& name : result.samplers) std::cout << "sampler=" << name << "\n";
    if (result.ok) {
      std::cout << "---\n";
      std::cout << result.source;
    }
    return 0;
  }
  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
