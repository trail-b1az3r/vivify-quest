// Smoke test of the C interface, against the shared library:
//   vivify-convert-capi-test <PC bundle>
// The bundle is made by converter/test/make_fixture.py.
#include "vivify_convert.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                         \
    }                                                                     \
  } while (0)

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <PC bundle>\n", argv[0]);
    return 2;
  }
  std::string const source = argv[1];
  std::string const converted = source + ".quest";
  std::string const again = source + ".again";
  std::string const retargeted = source + ".retargeted";

  CHECK(vivify_convert_api_version() == VIVIFY_CONVERT_API_VERSION);
  CHECK(vivify_convert_revision() > 0);
  CHECK(std::strcmp(vivify_convert_status_text(VIVIFY_CONVERT_BAD_ARGUMENT), "bad argument") == 0);
  CHECK(std::strcmp(vivify_convert_status_text(12345), "unknown status") == 0);

  VivifyConvertOptions options;
  vivify_convert_default_options(&options);
  CHECK(options.struct_size == sizeof(options));
  CHECK(options.translate_shaders == 1 && options.geometry_right_eye_copy == 1);

  char message[4096];
  CHECK(vivify_convert_bundle(nullptr, converted.c_str(), &options, nullptr, message, sizeof message) ==
        VIVIFY_CONVERT_BAD_ARGUMENT);
  CHECK(vivify_convert_is_bundle(source.c_str()) == 1);
  CHECK(vivify_convert_is_bundle(argv[0]) == 0);
  CHECK(vivify_convert_bundle(argv[0], converted.c_str(), &options, nullptr, message, sizeof message) ==
        VIVIFY_CONVERT_NOT_A_UNITY_BUNDLE);

  VivifyConvertResult result{};
  result.struct_size = sizeof(result);
  int const status = vivify_convert_bundle(source.c_str(), converted.c_str(), &options, &result, message, sizeof message);
  std::printf("convert: %s: %s\n", vivify_convert_status_text(status), message);
  CHECK(status == VIVIFY_CONVERT_SUCCESS);
  CHECK(result.status == status);
  CHECK(result.output_bytes > 0);
  CHECK(result.shaders_seen >= 1);
  CHECK(result.shaders_translated >= 1);

  // The output already targets Android.
  CHECK(vivify_convert_bundle(converted.c_str(), again.c_str(), &options, nullptr, nullptr, 0) ==
        VIVIFY_CONVERT_ALREADY_ANDROID);

  // An older caller's smaller result struct is written only as far as it reaches.
  struct {
    VivifyConvertResult head;
    uint32_t guard;
  } small{};
  small.head.struct_size = 2 * sizeof(uint32_t);  // struct_size and status only
  small.head.shaders_seen = -7;
  small.guard = 0xA5A5A5A5u;
  VivifyConvertOptions retarget = options;
  retarget.translate_shaders = 0;
  CHECK(vivify_convert_bundle(source.c_str(), retargeted.c_str(), &retarget, &small.head, nullptr, 0) ==
        VIVIFY_CONVERT_SUCCESS);
  CHECK(small.head.status == VIVIFY_CONVERT_SUCCESS);
  CHECK(small.head.shaders_seen == -7);
  CHECK(small.guard == 0xA5A5A5A5u);

  size_t const length = vivify_convert_describe_bundle(source.c_str(), nullptr, 0);
  CHECK(length > 0);
  std::string text(length + 1, '\0');
  CHECK(vivify_convert_describe_bundle(source.c_str(), text.data(), text.size()) == length);
  std::printf("describe: %s\n", text.c_str());

  std::remove(converted.c_str());
  std::remove(again.c_str());
  std::remove(retargeted.c_str());
  if (failures == 0) std::printf("C API smoke test passed\n");
  return failures == 0 ? 0 : 1;
}
