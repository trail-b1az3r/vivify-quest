cmake_minimum_required(VERSION 3.22)

option(QUEST "Build for quest" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_compile_options(-frtti -fexceptions -fvisibility=hidden -fPIE -fPIC)

# QPM defines - sets up NDK, RECURSE_FILES macro, EXTERN_DIR, SHARED_DIR
include(${CMAKE_CURRENT_SOURCE_DIR}/qpm_defines.cmake)

if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
	add_compile_options(-O3)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	add_compile_options(-flto)
endif()

if(${CMAKE_BUILD_TYPE} STREQUAL "DEBUG" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
	add_compile_options(-g)
endif()

project(Vivify
	VERSION ${MOD_VERSION})

add_compile_definitions(MOD_ID="${CMAKE_PROJECT_NAME}")
add_compile_definitions(VERSION="${CMAKE_PROJECT_VERSION}")
# Linked into the library by src/VivifyAudioLinkBundle.cpp (.incbin).
add_compile_definitions(VIVIFY_AUDIOLINK_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/assets/audiolink/AudioLink.bundle")
add_compile_definitions(VIVIFY_DEPTH_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/assets/depth/DepthBlitQuest.bundle")
set_property(SOURCE src/VivifyDepthCopy.cpp APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/depth/DepthBlitQuest.bundle")
# The lite build (VIVIFY_LITE=1 in the environment, set by build.yml for the
# lite QMOD) keeps 0.14.7's feature set; see include/main.hpp.
#
# The choice is cached in the build directory. `qpm s qmod` runs build.ps1,
# so CMake configures again when the QMOD is packed; reading only the
# environment then turned a lite build back into the full one, and every
# "Vivify Lite" QMOD up to 0.15.0 held the full library (0.15.1).
if(DEFINED ENV{VIVIFY_LITE})
	set(VIVIFY_LITE "$ENV{VIVIFY_LITE}" CACHE STRING "1 for the lite build" FORCE)
endif()
if("${VIVIFY_LITE}" STREQUAL "1")
	message(STATUS "Vivify: lite build")
	add_compile_definitions(VIVIFY_LITE=1)
endif()

# The game version built for. Unset is 1.40.8_7379, what qpm.json and the
# vendored extern/ describe. VIVIFY_GAME_TARGET=1.45.3 is set by
# .github/workflows/build-1.45.3.yml after scripts/use-game-target.py has
# pointed qpm.json at that game's dependencies; code that differs between the
# two tests VIVIFY_GAME_1_45_3. Cached for the same reason as VIVIFY_LITE.
if(DEFINED ENV{VIVIFY_GAME_TARGET})
	set(VIVIFY_GAME_TARGET "$ENV{VIVIFY_GAME_TARGET}" CACHE STRING "Beat Saber version built for" FORCE)
endif()
if("${VIVIFY_GAME_TARGET}" STREQUAL "1.45.3")
	message(STATUS "Vivify: building for Beat Saber 1.45.3")
	add_compile_definitions(VIVIFY_GAME_1_45_3=1)
endif()

set(COMPILE_ID ${CMAKE_PROJECT_NAME})

# Recursively get all src files
RECURSE_FILES(cpp_file_list ${SOURCE_DIR}/*.cpp)
RECURSE_FILES(c_file_list ${SOURCE_DIR}/*.c)

add_library(
	${CMAKE_PROJECT_NAME}
	SHARED
	${cpp_file_list}
	${c_file_list}
)

# Local diagnostics are excluded from distributable builds. Gameplay control
# additionally requires its own explicit opt-in, authorized for regression tests.
option(VIVIFY_MENU_DIAGNOSTICS "Enable file-triggered menu bundle diagnostics" OFF)
option(VIVIFY_GAMEPLAY_DIAGNOSTICS "Enable explicitly authorized bounded practice runs (diagnostic builds only)" OFF)
if(VIVIFY_GAMEPLAY_DIAGNOSTICS AND NOT VIVIFY_MENU_DIAGNOSTICS)
  message(FATAL_ERROR "Gameplay diagnostics require the separate menu diagnostic build")
endif()
if(VIVIFY_MENU_DIAGNOSTICS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE tools/separate-eye/menu_diagnostics.cpp)
  set_property(SOURCE src/VivifyComponents.cpp APPEND PROPERTY COMPILE_DEFINITIONS VIVIFY_MENU_DIAGNOSTICS=1)
  if(VIVIFY_GAMEPLAY_DIAGNOSTICS)
    set_property(SOURCE tools/separate-eye/menu_diagnostics.cpp APPEND PROPERTY COMPILE_DEFINITIONS VIVIFY_GAMEPLAY_DIAGNOSTICS=1)
  endif()
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE -llog -lz)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${INCLUDE_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SHARED_DIR})

# beatsaber hook inline hook
RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_c ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.c)
RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_cpp ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.cpp)

set(vivify_hook_dir "${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook")
set(vivify_fixed_hook "${CMAKE_CURRENT_BINARY_DIR}/generated/And64InlineHook.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${vivify_hook_dir}/And64InlineHook.cpp")
include(cmake/FixArm64Relocator.cmake)
vivify_fix_arm64_relocator("${vivify_hook_dir}/And64InlineHook.cpp" "${vivify_fixed_hook}")
list(REMOVE_ITEM src_inline_hook_beatsaber_hook_local_extra_cpp "${vivify_hook_dir}/And64InlineHook.cpp")
target_sources(${COMPILE_ID} PRIVATE "${vivify_fixed_hook}")
target_include_directories(${COMPILE_ID} PRIVATE "${vivify_hook_dir}")

target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_c})
target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_cpp})

# Strip debug symbols in release
if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE")
	add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_STRIP} -g -S -d --strip-debug
		"${CMAKE_CURRENT_BINARY_DIR}/lib${CMAKE_PROJECT_NAME}.so"
	)
endif()

# QPM extern includes and libs
include(${CMAKE_CURRENT_SOURCE_DIR}/extern.cmake)
