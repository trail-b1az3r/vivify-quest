#include "VivifyBundleConvert.hpp"
#include "VivifySerializedFile.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Vivify::BundleConvert;

static std::string fmt_hex(uint8_t c) {
  static char const digits[] = "0123456789abcdef";
  return {digits[c >> 4], digits[c & 15]};
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: conv [--repack|--shaders] <src> <dst>\n");
    return 2;
  }
  // --inspect <serialized-file> prints every shader's m_ParsedForm program
  // references and store entries, so the tests can check what conversion wrote
  // rather than only that it said it succeeded.
  if (std::string(argv[1]) == "--inspect") {
    std::ifstream in(argv[2], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto report = SerializedFileParse::InspectSerializedFile(bytes.data(), bytes.size());
    for (auto const& shader : report.shaders) {
      std::printf("shader=%s platforms=", shader.name.c_str());
      for (size_t i = 0; i < shader.platforms.size(); i++) std::printf("%s%d", i ? "," : "", shader.platforms[i]);
      std::printf("\n");
      std::printf("pptrs=%zu\n", shader.pptrFileOffsets.size());
      for (auto const& ref : shader.programRefs) {
        std::string keywords;
        for (uint16_t k : ref.keywordIndices) {
          if (!keywords.empty()) keywords += ",";
          keywords += k < shader.keywordNames.size() ? shader.keywordNames[k] : std::to_string(k);
        }
        std::printf("ref stage=%d list=%d index=%d blob=%u type=%d keywords=%s\n", ref.stage, ref.list,
                    ref.index, ref.blobIndex, ref.gpuProgramType, keywords.c_str());
      }
      auto decoded = SerializedFileParse::DecodeShaderPrograms(bytes.data(), bytes.size(), shader);
      std::printf("decodeOk=%d\n", decoded.ok ? 1 : 0);
      for (auto const& program : decoded.programs) {
        // Newlines are written as the two characters \n; GLSL has no other
        // use for a backslash, and '|' (the old stand-in) is its bitwise or.
        std::string code;
        for (uint8_t c : program.code) {
          if (c == '\n') code += "\\n";
          else if (c < 0x20 || c > 0x7e || c == '\\') code += '.';
          else code += static_cast<char>(c);
        }
        // What follows the code's alignment padding, as hex ("-" for none),
        // so a test can see the parameter tables of a 2019 blob survive intact.
        std::string trailing = program.trailing.empty() ? "-" : "";
        for (uint8_t c : program.trailing) trailing += fmt_hex(c);
        std::printf("entry blob=%d raw=%d rawSize=%zu type=%d trailing=%s code=%s\n", program.blobIndex,
                    program.raw ? 1 : 0, program.rawBytes.size(), program.programType, trailing.c_str(),
                    code.c_str());
      }
    }
    return 0;
  }
  // --merge <quest> <converted-pc> <dst> <name>... puts the named shaders of
  // a converted PC bundle into a Quest bundle that shipped them empty.
  if (std::string(argv[1]) == "--merge") {
    if (argc < 6) {
      std::fprintf(stderr, "usage: conv --merge <quest> <converted-pc> <dst> <shader-name>...\n");
      return 2;
    }
    std::vector<std::string> names(argv + 5, argv + argc);
    ShaderMerge m = MergeShadersInto(argv[2], argv[3], names, argv[4]);
    std::printf("status=%s\nmessage=%s\nmerged=%d outBytes=%llu\n", std::string(StatusText(m.status)).c_str(),
                m.message.c_str(), m.merged, (unsigned long long)m.outputBytes);
    for (auto const& name : m.mergedNames) std::printf("mergedName=%s\n", name.c_str());
    for (auto const& skip : m.skipped) std::printf("skipped=%s\n", skip.c_str());
    for (auto const& note : m.notes) std::printf("note=%s\n", note.c_str());
    return m.status == Status::Success ? 0 : 1;
  }
  // --repack runs the bundle through the step-4 rewrite path with no shader
  // edits, which must leave a bundle that reads back the same.
  // --shaders runs the whole conversion: translate the DirectX programs to
  // GLSL ES and rebuild the archive around the shaders that changed size.
  std::string const first = argv[1];
  bool const repack = first == "--repack";
  // --shaders-split is --shaders with ShaderConversionOptions::separateStereoVariants.
  // --shaders-split2019 also splits Unity 2019 bundles (splitUnity2019).
  bool const split2019 = first == "--shaders-split2019";
  bool const split = first == "--shaders-split" || split2019;
  // --shaders-single is replay render mode (singleViewOnly).
  // --shaders-companion is the blit companion: single-view, internal files renamed.
  bool const companion = first == "--shaders-companion";
  bool const single = first == "--shaders-single" || companion;
  bool const shaders = first == "--shaders" || split || single;
  bool const flagged = repack || shaders;
  if (flagged && argc < 4) {
    std::fprintf(stderr, "usage: conv %s <src> <dst>\n", first.c_str());
    return 2;
  }
  char const* const src = flagged ? argv[2] : argv[1];
  char const* const dst = flagged ? argv[3] : argv[2];

  if (shaders) {
    ShaderConversionOptions options;
    options.separateStereoVariants = split;
    options.splitUnity2019 = split2019;
    options.singleViewOnly = single;
    options.renameArchiveFiles = companion;
    options.dropUntranslatableGeometry = std::getenv("VIVIFY_DROP_GEOMETRY") != nullptr;
    ShaderConversion c = ConvertShadersToGles(src, dst, options);
    std::printf("status=%s\nmessage=%s\nseen=%d translated=%d leftAlone=%d refused=%d "
                "programs=%d outBytes=%llu\ntexSeen=%d texReadable=%d texStreamed=%d\n"
                "linked=%d variantsLinked=%d variantsRefused=%d stereoRemapped=%d stereoSplit=%d geometryDropped=%d\n",
                std::string(StatusText(c.status)).c_str(), c.message.c_str(), c.shadersSeen,
                c.shadersTranslated, c.shadersLeftAlone, c.shadersRefused, c.programsTranslated,
                (unsigned long long)c.outputBytes, c.texturesSeen, c.texturesMarkedReadable,
                c.texturesStreamed, c.shadersLinked, c.variantsLinked, c.variantsRefused,
                c.stereoVariantsRemapped, c.stereoVariantsSplit, c.geometryStagesDropped);
    for (auto const& refusal : c.refusals) std::printf("refusal=%s\n", refusal.c_str());
    for (auto const& refusal : c.variantRefusals) std::printf("variantRefusal=%s\n", refusal.c_str());
    return c.ok() ? 0 : 1;
  }

  Result r = repack ? RepackBundle(src, dst) : ConvertToAndroid(src, dst);
  std::printf("status=%s\nmessage=%s\nsourcePlatform=%s\nseen=%d retargeted=%d outBytes=%llu\n",
              std::string(StatusText(r.status)).c_str(), r.message.c_str(), r.sourcePlatform.c_str(),
              r.serializedFilesSeen, r.serializedFilesRetargeted,
              (unsigned long long)r.outputBytes);
  return r.ok() ? 0 : 1;
}
