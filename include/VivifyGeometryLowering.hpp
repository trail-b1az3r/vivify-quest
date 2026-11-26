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

// A geometry stage that draws nothing in most cameras (0.16.4). AreaLit's
// light-mesh pass starts with
//   r0.x = (unity_OrthoParams.w != 1.0) ? -1 : 0;  if (r0.x != 0) { return; }
// so it only ever writes the light data texture from the map's orthographic
// light camera. Where the stage is dropped (Flat Geometry Effects, or a stage
// that does not translate), the plain mesh used to be drawn in every camera:
// the light meshes -- a sky sphere around the player, the light box inside
// the TV -- appeared in the headset view. The check is kept in the vertex
// stage instead: when it holds, every vertex goes to the same point and the
// triangle is not drawn.
struct Gate {
  std::string condition;     // a GLSL bool expression over uniforms only
  std::string declarations;  // the uniform declarations it needs, one per line
};

// The geometry program's opening early return, when its condition reads only
// plain uniforms and literals.
std::optional<Gate> OpeningUniformReturn(std::string const& geometry);

// Wraps the vertex program's main so that, after it runs, gl_Position is
// zero when the gate's condition holds. Adds the uniform declarations the
// vertex program lacks. False when the source does not have the expected
// shape (it is then left as it was).
bool WithGate(std::string& vertex, Gate const& gate);

}  // namespace Vivify::GeometryLowering
