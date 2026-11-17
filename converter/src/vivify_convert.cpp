// The C interface of the standalone converter (include/vivify_convert.h): a
// thin layer over the same BundleConvert code the Vivify Quest mod compiles.
#include "vivify_convert.h"

#include "VivifyBundleConvert.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <new>
#include <string>

namespace {

using namespace Vivify::BundleConvert;

void CopyText(std::string const& text, char* out, size_t size) {
  if (out == nullptr || size == 0) return;
  size_t const n = std::min(text.size(), size - 1);
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

}  // namespace

extern "C" {

int32_t vivify_convert_api_version(void) {
  return VIVIFY_CONVERT_API_VERSION;
}

int32_t vivify_convert_revision(void) {
  return kConverterRevision;
}

void vivify_convert_default_options(VivifyConvertOptions* options) {
  if (options == nullptr) return;
  std::memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->translate_shaders = 1;
  options->geometry_right_eye_copy = 1;
  options->skip_quest_unused_variants = 1;
}

namespace {

int32_t ConvertInto(const char* source_path, const char* dest_path, VivifyConvertOptions const& options,
                    VivifyConvertResult& out, std::string& message) {
  using namespace Vivify::BundleConvert;
  if (!options.translate_shaders) {
    auto const converted = ConvertToAndroid(source_path, dest_path);
    out.output_bytes = converted.outputBytes;
    message = converted.message;
    return static_cast<int32_t>(converted.status);
  }
  ShaderConversionOptions conversion;
  conversion.separateStereoVariants = options.separate_stereo_variants != 0;
  conversion.splitUnity2019 = options.split_unity_2019 != 0;
  conversion.singleViewOnly = options.single_view_only != 0;
  conversion.renameArchiveFiles = options.rename_archive_files != 0;
  conversion.dropUntranslatableGeometry = options.drop_untranslatable_geometry != 0;
  conversion.dropGeometryStages = options.drop_geometry_stages != 0;
  conversion.geometryRightEyeCopy = options.geometry_right_eye_copy != 0;
  conversion.skipQuestUnusedVariants = options.skip_quest_unused_variants != 0;
  auto const converted = ConvertShadersToGles(source_path, dest_path, conversion);
  out.shaders_seen = converted.shadersSeen;
  out.shaders_translated = converted.shadersTranslated;
  out.shaders_left_alone = converted.shadersLeftAlone;
  out.shaders_refused = converted.shadersRefused;
  out.programs_translated = converted.programsTranslated;
  out.variants_linked = converted.variantsLinked;
  out.variants_refused = converted.variantsRefused;
  out.variants_skipped = converted.variantsSkipped;
  out.geometry_stages_dropped = converted.geometryStagesDropped;
  out.textures_seen = converted.texturesSeen;
  out.textures_marked_readable = converted.texturesMarkedReadable;
  out.output_bytes = converted.outputBytes;
  message = converted.message;
  for (auto const& refusal : converted.refusals) message += "\nleft as it was: " + refusal;
  for (auto const& refusal : converted.variantRefusals) message += "\nsome variants left on Direct3D: " + refusal;
  return static_cast<int32_t>(converted.status);
}

}  // namespace

int32_t vivify_convert_bundle(const char* source_path, const char* dest_path, const VivifyConvertOptions* options,
                              VivifyConvertResult* result, char* message, size_t message_size) {
  // Options and result are read and written only as far as the caller's
  // struct_size reaches, so a caller built against an older, smaller struct
  // keeps working: the fields it does not know take their defaults.
  VivifyConvertOptions settings;
  vivify_convert_default_options(&settings);
  if (options != nullptr && options->struct_size >= sizeof(uint32_t)) {
    std::memcpy(&settings, options, std::min<size_t>(options->struct_size, sizeof(settings)));
    settings.struct_size = sizeof(settings);
  }
  VivifyConvertResult out{};
  out.struct_size = sizeof(out);
  std::string text;
  int32_t status;
  if (source_path == nullptr || dest_path == nullptr || *source_path == '\0' || *dest_path == '\0') {
    text = "a source and a destination path are needed";
    status = VIVIFY_CONVERT_BAD_ARGUMENT;
  } else {
    try {
      status = ConvertInto(source_path, dest_path, settings, out, text);
    } catch (std::bad_alloc const&) {
      text = "out of memory";
      status = VIVIFY_CONVERT_OUT_OF_MEMORY;
    } catch (std::exception const& error) {
      text = std::string("conversion failed: ") + error.what();
      status = VIVIFY_CONVERT_CORRUPT;
    }
  }
  out.status = status;
  if (result != nullptr) {
    size_t const size = result->struct_size == 0 ? sizeof(out) : std::min<size_t>(result->struct_size, sizeof(out));
    std::memcpy(result, &out, size);
    result->struct_size = static_cast<uint32_t>(size);
  }
  CopyText(text, message, message_size);
  return status;
}

size_t vivify_convert_describe_bundle(const char* path, char* text, size_t text_size) {
  std::string description;
  try {
    description = path == nullptr ? std::string("no path") : DescribeShaderScan(ScanShaders(path));
  } catch (std::exception const& error) {
    description = std::string("scan failed: ") + error.what();
  }
  CopyText(description, text, text_size);
  return description.size();
}

int32_t vivify_convert_is_bundle(const char* path) {
  return path != nullptr && IsUnityBundleFile(path) ? 1 : 0;
}

const char* vivify_convert_status_text(int32_t status) {
  if (status == VIVIFY_CONVERT_BAD_ARGUMENT) return "bad argument";
  if (status < 0 || status > VIVIFY_CONVERT_OUT_OF_MEMORY) return "unknown status";
  // StatusText returns string literals, so the view is NUL-terminated.
  return StatusText(static_cast<Status>(status)).data();
}

}  // extern "C"
