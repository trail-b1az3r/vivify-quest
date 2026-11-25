#pragma once

// Geometry-stage lowering (0.15.55).
//
// The Quest draws a map in one multiview pass, where a geometry stage cannot
// run, so the converter drops it and the rest of the shader draws the plain
// mesh. For the commonest kind -- a stage that takes one triangle and emits
// exactly its three corners, as every "solid wireframe" shader does -- the
// stage's work can be done in the vertex stage instead, if each vertex knows
// all three corners of its triangle. The lowered vertex program:
//   - runs the original vertex program once for each corner of the triangle,
//     the corners read from UV channels 5, 6 and 7, and which corner this
//     vertex is from UV channel 4 (x; w = 1 marks a prepared mesh);
//   - then runs the original geometry program over those three results and
//     keeps what it emitted for this vertex's corner.
// The mod gives a renderer drawn with such a shader a copy of its mesh with
// every triangle's corners unshared and those channels filled (Runtime::
// PrepareLoweredGeometry). On a mesh without them (w = 0) it draws as
// before: the plain mesh, with the geometry stage's outputs zero.

#include <optional>
#include <string>

namespace Vivify::GeometryLowering {

// The marker comment a lowered vertex program carries; the bundle scan finds
// lowered shaders by it.
inline constexpr char const* kMarker = "// vivify:lowered-triangle-geometry";

struct Result {
  bool ok = false;
  std::string source;  // the lowered vertex program, a whole GLSL source
  std::string reason;  // why not, when !ok
};

// vertex and geometry are the translated GLSL sources of one variant (each
// with its own #version line). Lowers only a geometry program that takes
// triangles, emits exactly three vertices, has no loops, and a vertex program
// whose only input is in_POSITION0 and that writes gl_Position.
Result LowerTriangleGeometry(std::string const& vertex, std::string const& geometry);

}  // namespace Vivify::GeometryLowering
