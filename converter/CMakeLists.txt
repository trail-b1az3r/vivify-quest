# The Vivify Quest converter as a standalone library and command-line tool,
# for desktops (Windows, Linux, macOS) and for embedding in other projects.
#
#   cmake -S converter -B build-converter -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-converter --config Release
#
# Builds:
#   vivifyconvert         shared library (vivifyconvert.dll / libvivifyconvert.so / .dylib)
#   vivifyconvert_static  static library (define VIVIFY_CONVERT_STATIC when using it)
#   vivify-convert        command-line tool
#
# The sources are the mod's own (../src): the library converts exactly as the
# mod does on the headset. The mod's build is separate and unchanged.
cmake_minimum_required(VERSION 3.20)
project(vivify_convert LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(VIVIFY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(VIVIFY_CONVERT_SOURCES
    ${VIVIFY_ROOT}/src/VivifyBundleConvert.cpp
    ${VIVIFY_ROOT}/src/VivifySerializedFile.cpp
    ${VIVIFY_ROOT}/src/VivifyDxbc.cpp
    ${VIVIFY_ROOT}/src/VivifyTriangleTessellation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vivify_convert.cpp)

option(VIVIFY_CONVERT_LTO "Link-time optimisation" ON)
include(CheckIPOSupported)
if(VIVIFY_CONVERT_LTO)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_message LANGUAGES CXX)
endif()

function(vivify_convert_setup target)
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                       PRIVATE ${VIVIFY_ROOT}/include)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W3 /permissive- $<$<CONFIG:Release>:/O2 /Oi>)
  else()
    target_compile_options(${target} PRIVATE -Wall $<$<CONFIG:Release>:-O3>)
  endif()
endfunction()

# Link-time optimisation for what is linked here (the shared library and the
# tools), never for the static library: LTO objects only link with the exact
# compiler that made them, which another project's build may not be.
function(vivify_convert_lto target)
  if(VIVIFY_CONVERT_LTO AND ipo_ok)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  endif()
endfunction()

add_library(vivifyconvert SHARED ${VIVIFY_CONVERT_SOURCES})
target_compile_definitions(vivifyconvert PRIVATE VIVIFY_CONVERT_BUILD)
vivify_convert_setup(vivifyconvert)
vivify_convert_lto(vivifyconvert)
if(MINGW)
  # A self-contained DLL: no libstdc++/libgcc DLLs to ship beside it, and the
  # name P/Invoke looks for (vivifyconvert.dll, not libvivifyconvert.dll).
  target_link_options(vivifyconvert PRIVATE -static -static-libgcc -static-libstdc++)
  set_target_properties(vivifyconvert PROPERTIES PREFIX "")
endif()
if(NOT MSVC AND NOT APPLE)
  target_link_options(vivifyconvert PRIVATE $<$<CONFIG:Release>:-s>)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Loads into any Unity Editor or tool whatever libstdc++ the system has.
  target_link_options(vivifyconvert PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
endif()

add_library(vivifyconvert_static STATIC ${VIVIFY_CONVERT_SOURCES})
target_compile_definitions(vivifyconvert_static PUBLIC VIVIFY_CONVERT_STATIC)
vivify_convert_setup(vivifyconvert_static)

# The tool compiles the sources itself rather than linking the static library,
# so it gets link-time optimisation across all of them.
add_executable(vivify-convert ${CMAKE_CURRENT_SOURCE_DIR}/src/cli.cpp ${VIVIFY_CONVERT_SOURCES})
target_compile_definitions(vivify-convert PRIVATE VIVIFY_CONVERT_STATIC)
vivify_convert_setup(vivify-convert)
vivify_convert_lto(vivify-convert)
if(MINGW)
  target_link_options(vivify-convert PRIVATE -static)
endif()
if(NOT MSVC AND NOT APPLE)
  target_link_options(vivify-convert PRIVATE $<$<CONFIG:Release>:-s>)
endif()

# The desktop app (converter/gui): the converter in a window. Off by default
# so the library and CLI build with no extra downloads; the Converter library
# workflow turns it on. Its libraries are fetched at fixed versions.
option(VIVIFY_CONVERT_GUI "Build the Vivify Quest Converter desktop app" OFF)
if(VIVIFY_CONVERT_GUI)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  # X11 only on Linux: Wayland desktops run it through XWayland, and the
  # build needs no wayland-scanner.
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE
                       SOURCE_SUBDIR none)
  # A fixed commit (2025-03-21): the 0.1.0 tag predates settings::available().
  FetchContent_Declare(pfd GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
                       GIT_TAG c12ea8c9a727f5320a2b4570aee863bbede2a204 SOURCE_SUBDIR none)
  # SOURCE_SUBDIR none: fetch Dear ImGui and the dialogs without building
  # their own projects; their sources are compiled into the app below.
  FetchContent_MakeAvailable(glfw imgui pfd)
  find_package(OpenGL REQUIRED)

  add_executable(vivify-convert-gui MACOSX_BUNDLE
      ${CMAKE_CURRENT_SOURCE_DIR}/gui/main.cpp
      ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
      ${imgui_SOURCE_DIR}/imgui_widgets.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
      ${VIVIFY_CONVERT_SOURCES})
  target_compile_definitions(vivify-convert-gui PRIVATE VIVIFY_CONVERT_STATIC)
  target_include_directories(vivify-convert-gui PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                                                        ${pfd_SOURCE_DIR})
  vivify_convert_setup(vivify-convert-gui)
  vivify_convert_lto(vivify-convert-gui)
  target_link_libraries(vivify-convert-gui PRIVATE glfw OpenGL::GL)
  if(WIN32)
    # windows.h (pulled in by the file dialogs) must not define min/max.
    target_compile_definitions(vivify-convert-gui PRIVATE NOMINMAX)
    target_link_libraries(vivify-convert-gui PRIVATE comdlg32 shell32 ole32)
  endif()
  set_target_properties(vivify-convert-gui PROPERTIES
      OUTPUT_NAME "Vivify Quest Converter"
      WIN32_EXECUTABLE ON
      MACOSX_BUNDLE_BUNDLE_NAME "Vivify Quest Converter"
      MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.vivifyquest.converter")
  if(MSVC)
    # A window app with a plain main(): no console window, no WinMain.
    target_link_options(vivify-convert-gui PRIVATE /ENTRY:mainCRTStartup)
  elseif(MINGW)
    target_link_options(vivify-convert-gui PRIVATE -static)
  endif()
  if(NOT MSVC AND NOT APPLE)
    target_link_options(vivify-convert-gui PRIVATE $<$<CONFIG:Release>:-s>)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(vivify-convert-gui PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  install(TARGETS vivify-convert-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
endif()

# The C API smoke test (converter/test). Off when this directory is pulled into
# another project with add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
endif()
if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(vivify-convert-capi-test ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_test.cpp)
  target_link_libraries(vivify-convert-capi-test PRIVATE vivifyconvert)
  vivify_convert_setup(vivify-convert-capi-test)
  if(MINGW)
    target_link_options(vivify-convert-capi-test PRIVATE -static)
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    set(fixture ${CMAKE_CURRENT_BINARY_DIR}/capi_fixture.vivify)
    add_test(NAME capi_fixture
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/make_fixture.py ${fixture})
    add_test(NAME capi COMMAND vivify-convert-capi-test ${fixture})
    set_tests_properties(capi_fixture PROPERTIES FIXTURES_SETUP capi_bundle)
    set_tests_properties(capi PROPERTIES FIXTURES_REQUIRED capi_bundle)
  endif()
endif()

install(TARGETS vivifyconvert vivifyconvert_static vivify-convert)
install(FILES include/vivify_convert.h DESTINATION include)
