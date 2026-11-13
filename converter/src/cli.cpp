// vivify-convert: the converter from a command line.
//
//   vivify-convert [options] <source bundle> <destination>
//   vivify-convert --describe <bundle>
//
// Options (the defaults are what the Vivify Quest mod uses on a map):
//   --retarget-only          retarget to Android, leave shaders as they are
//   --single-view            single-view programs only (screen textures, replays)
//   --split-stereo           separate single-view and multiview variants
//   --split-2019             the same for Unity 2019 bundles
//   --rename-files           new archive file names (load beside another copy)
//   --no-right-eye-copy      geometry stages draw the left eye only
//   --flat-geometry          draw geometry-shader variants without that stage
//   --drop-bad-geometry      only those whose geometry stage cannot translate
//   --keep-unused-variants   also translate GPU-instancing and SPI variants
#include "vivify_convert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  VivifyConvertOptions options;
  vivify_convert_default_options(&options);
  std::vector<char const*> paths;
  bool describe = false;
  for (int i = 1; i < argc; i++) {
    std::string const arg = argv[i];
    if (arg == "--describe") describe = true;
    else if (arg == "--retarget-only") options.translate_shaders = 0;
    else if (arg == "--single-view") options.single_view_only = 1;
    else if (arg == "--split-stereo") options.separate_stereo_variants = 1;
    else if (arg == "--split-2019") options.split_unity_2019 = 1;
    else if (arg == "--rename-files") options.rename_archive_files = 1;
    else if (arg == "--no-right-eye-copy") options.geometry_right_eye_copy = 0;
    else if (arg == "--flat-geometry") options.drop_geometry_stages = 1;
    else if (arg == "--drop-bad-geometry") options.drop_untranslatable_geometry = 1;
    else if (arg == "--keep-unused-variants") options.skip_quest_unused_variants = 0;
    else if (arg == "--version") {
      std::printf("vivify-convert, converter revision %d, API %d\n", vivify_convert_revision(),
                  vivify_convert_api_version());
      return 0;
    } else if (arg.rfind("--", 0) == 0) {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 2;
    } else {
      paths.push_back(argv[i]);
    }
  }
  if (describe && paths.size() == 1) {
    size_t const size = vivify_convert_describe_bundle(paths[0], nullptr, 0);
    std::string text(size + 1, '\0');
    vivify_convert_describe_bundle(paths[0], text.data(), text.size());
    std::printf("%s\n", text.c_str());
    return 0;
  }
  if (paths.size() != 2) {
    std::fprintf(stderr,
                 "usage: vivify-convert [options] <source bundle> <destination>\n"
                 "       vivify-convert --describe <bundle>\n"
                 "       vivify-convert --version\n");
    return 2;
  }
  VivifyConvertResult result{};
  result.struct_size = sizeof(result);
  std::vector<char> message(1 << 16);
  int const status = vivify_convert_bundle(paths[0], paths[1], &options, &result, message.data(), message.size());
  std::printf("%s: %s\n", vivify_convert_status_text(status), message.data());
  if (status == VIVIFY_CONVERT_SUCCESS && options.translate_shaders) {
    std::printf("shaders %d translated of %d, %d refused; variants %d linked, %d refused, %d skipped; "
                "%llu bytes\n",
                result.shaders_translated, result.shaders_seen, result.shaders_refused, result.variants_linked,
                result.variants_refused, result.variants_skipped,
                static_cast<unsigned long long>(result.output_bytes));
  }
  return status == VIVIFY_CONVERT_SUCCESS || status == VIVIFY_CONVERT_ALREADY_ANDROID ? 0 : 1;
}
