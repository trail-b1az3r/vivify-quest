/*
 * vivify_convert: the Vivify Quest asset bundle converter as a library.
 *
 * Turns a PC (Windows) Unity asset bundle -- a Vivify map's bundleWindows2019
 * or bundleWindows2021 -- into one Beat Saber on Quest can load: the bundle is
 * retargeted to Android, and every shader's Direct3D bytecode is translated to
 * GLSL ES 3 with OpenGL ES multiview for single-pass stereo. It is the same
 * code the Vivify Quest mod runs on the headset, built here for a desktop, so
 * a map can be converted ahead of time in the Unity project that makes it, or
 * by any other tool.
 *
 * A plain C interface, so it can be called from C, C++, C# (P/Invoke; see
 * unity/Editor/VivifyQuestConverter.cs) or anything else with a C FFI. Paths
 * are UTF-8. Every call is self-contained and thread-safe: it reads one file
 * and writes another, with no global state.
 */
#ifndef VIVIFY_CONVERT_H
#define VIVIFY_CONVERT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(VIVIFY_CONVERT_BUILD)
#    define VIVIFY_CONVERT_API __declspec(dllexport)
#  elif defined(VIVIFY_CONVERT_STATIC)
#    define VIVIFY_CONVERT_API
#  else
#    define VIVIFY_CONVERT_API __declspec(dllimport)
#  endif
#else
#  define VIVIFY_CONVERT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped when a struct below changes shape. */
#define VIVIFY_CONVERT_API_VERSION 1

/* Status codes, as the mod's BundleConvert::Status. */
enum {
  VIVIFY_CONVERT_SUCCESS = 0,
  VIVIFY_CONVERT_ALREADY_ANDROID = 1,     /* nothing to do: the bundle already targets Android */
  VIVIFY_CONVERT_SOURCE_UNREADABLE = 2,
  VIVIFY_CONVERT_NOT_A_UNITY_BUNDLE = 3,
  VIVIFY_CONVERT_UNSUPPORTED_COMPRESSION = 4,
  VIVIFY_CONVERT_CORRUPT = 5,
  VIVIFY_CONVERT_DEST_UNWRITABLE = 6,
  VIVIFY_CONVERT_OUT_OF_MEMORY = 7,
  VIVIFY_CONVERT_BAD_ARGUMENT = 100
};

typedef struct VivifyConvertOptions {
  /* sizeof(VivifyConvertOptions); vivify_convert_default_options sets it. */
  uint32_t struct_size;
  /* 0: only retarget the bundle to Android, leaving shaders as they are. */
  int32_t translate_shaders;
  /* Separate single-view and multiview programs for stereo-aware shaders,
   * selected by the STEREO_MULTIVIEW_ON keyword (only for a game whose own
   * shaders use that keyword; Beat Saber 1.40 does not). */
  int32_t separate_stereo_variants;
  /* As above for Unity 2019 bundles too. */
  int32_t split_unity_2019;
  /* Single-view programs only: for cameras that draw into plain textures
   * (replay rendering, screen textures). */
  int32_t single_view_only;
  /* Give the archive's files new names, so the result can be loaded beside
   * the multiview conversion of the same bundle. */
  int32_t rename_archive_files;
  /* Draw geometry-shader variants that cannot be translated without that
   * stage, instead of leaving them on Direct3D. */
  int32_t drop_untranslatable_geometry;
  /* Draw every geometry-shader variant without that stage (flat). */
  int32_t drop_geometry_stages;
  /* Geometry stages draw a second copy of their output into the right eye
   * (the Quest's driver runs them for the left eye only). */
  int32_t geometry_right_eye_copy;
  /* Leave out the variants a Quest never selects (GPU instancing, unsplit
   * single-pass stereo): most of a PC bundle's programs. Turn it off for a
   * bundle whose materials keep GPU instancing on. */
  int32_t skip_quest_unused_variants;
} VivifyConvertOptions;

typedef struct VivifyConvertResult {
  /* Set to sizeof(VivifyConvertResult) before the call (0 is taken as the
   * full size); only that much is written. */
  uint32_t struct_size;
  int32_t status;
  int32_t shaders_seen;
  int32_t shaders_translated;
  int32_t shaders_left_alone;
  int32_t shaders_refused;
  int32_t programs_translated;
  int32_t variants_linked;
  int32_t variants_refused;
  int32_t variants_skipped;
  int32_t geometry_stages_dropped;
  int32_t textures_seen;
  int32_t textures_marked_readable;
  uint64_t output_bytes;
} VivifyConvertResult;

/* VIVIFY_CONVERT_API_VERSION of the library. */
VIVIFY_CONVERT_API int32_t vivify_convert_api_version(void);

/* The converter's revision (as the mod's cache: a bundle converted by an
 * older revision should be converted again). */
VIVIFY_CONVERT_API int32_t vivify_convert_revision(void);

/* The options the Vivify Quest mod converts a map's PC bundle with. */
VIVIFY_CONVERT_API void vivify_convert_default_options(VivifyConvertOptions* options);

/* Converts the bundle at `source_path` and writes it to `dest_path` (written
 * to a temporary name and renamed into place). `options` may be NULL for the
 * defaults, `result` may be NULL. A readable summary, or the reason for a
 * failure, is written to `message` (UTF-8, NUL-terminated, cut to
 * `message_size`); it may be NULL. Returns the status. */
VIVIFY_CONVERT_API int32_t vivify_convert_bundle(const char* source_path, const char* dest_path,
                                                 const VivifyConvertOptions* options, VivifyConvertResult* result,
                                                 char* message, size_t message_size);

/* What a bundle's shaders were compiled for, as readable text: Unity version,
 * platforms, program counts, shaders shipped empty. Returns the length of the
 * full text (call with text_size 0 to size a buffer); writes what fits. */
VIVIFY_CONVERT_API size_t vivify_convert_describe_bundle(const char* path, char* text, size_t text_size);

/* Non-zero if the file starts with the UnityFS archive signature. */
VIVIFY_CONVERT_API int32_t vivify_convert_is_bundle(const char* path);

/* A short English name for a status code. */
VIVIFY_CONVERT_API const char* vivify_convert_status_text(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* VIVIFY_CONVERT_H */
