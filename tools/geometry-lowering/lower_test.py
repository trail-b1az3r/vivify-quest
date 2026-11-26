#!/usr/bin/env python3
"""Geometry-stage lowering (src/VivifyGeometryLowering.cpp).

Lowers hand-written vertex/geometry pairs shaped like the converter's output
(a wireframe and an exploding-triangles stage under multiview, with the
converter's per-eye copy blocks) and checks with glslangValidator that the
lowered vertex program compiles and links with the fragment program. Shapes
the lowering must refuse are checked too.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

VS = """#version 320 es
#extension GL_OVR_multiview2 : require
layout(num_views = 2) in;
precision highp float;
precision highp int;
in vec4 in_POSITION0;
out vec4 vs_TEXCOORD1;
flat out highp int vivify_ViewIndex;
uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];
layout(std140) uniform UnityStereoGlobals { vec4 vivify_cb_UnityStereoGlobals[68]; };
void main() {
  vivify_ViewIndex = int(gl_ViewID_OVR);
  vec4 world = hlslcc_mtx4x4unity_ObjectToWorld[0] * in_POSITION0.x + hlslcc_mtx4x4unity_ObjectToWorld[1] * in_POSITION0.y +
      hlslcc_mtx4x4unity_ObjectToWorld[2] * in_POSITION0.z + hlslcc_mtx4x4unity_ObjectToWorld[3];
  vs_TEXCOORD1 = world;
  int row = 24 + 4 * int(gl_ViewID_OVR);
  gl_Position = vivify_cb_UnityStereoGlobals[row] * world.x + vivify_cb_UnityStereoGlobals[row + 1] * world.y +
      vivify_cb_UnityStereoGlobals[row + 2] * world.z + vivify_cb_UnityStereoGlobals[row + 3] * world.w;
  return;
}
"""

GS_HEAD = """#version 320 es
#extension GL_OVR_multiview2 : enable
#extension GL_EXT_multiview_tessellation_geometry_shader : enable
#ifdef GL_EXT_multiview_tessellation_geometry_shader
layout(num_views = 2) in;
#endif
precision highp float;
precision highp int;
in vec4 vs_TEXCOORD1[];
flat in highp int vivify_ViewIndex[];
out vec4 gs_TEXCOORD1;
out vec4 vs_TEXCOORD2;
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform float _WireThickness;
#ifdef GL_EXT_multiview_tessellation_geometry_shader
#define VIVIFY_EYE_COPY 0
#else
#define VIVIFY_EYE_COPY 1
layout(invocations = 2) in;
#endif
layout(std140) uniform UnityStereoGlobals { vec4 vivify_cb_UnityStereoGlobals[68]; };
"""

EYE_COPY = """  if (VIVIFY_EYE_COPY != 0) {
    if (vivify_EyeCopy != 0) gl_Position = vivify_LeftToRight * gl_Position;
    gl_Layer = vivify_EyeCopy;
  }
"""

def emit(i, one_hot):
    return (f"  gl_Position = gl_in[{i}].gl_Position;\n"
            f"  gs_TEXCOORD1 = vec4({one_hot}, 1.0 / gl_in[{i}].gl_Position.w);\n"
            f"  vs_TEXCOORD2 = vs_TEXCOORD1[{i}] * _WireThickness;\n" + EYE_COPY + "  EmitVertex();\n")

GS_WIREFRAME = GS_HEAD + """void main() {
  int vivify_EyeCopy = VIVIFY_EYE_COPY != 0 ? gl_InvocationID : 0;
  mat4 vivify_LeftToRight = mat4(vivify_cb_UnityStereoGlobals[28], vivify_cb_UnityStereoGlobals[29], vivify_cb_UnityStereoGlobals[30], vivify_cb_UnityStereoGlobals[31]);
""" + emit(0, "1.0, 0.0, 0.0") + emit(1, "0.0, 1.0, 0.0") + emit(2, "0.0, 0.0, 1.0") + "  return;\n}\n"

GS_EXPLODE = GS_HEAD + """void main() {
  int vivify_EyeCopy = VIVIFY_EYE_COPY != 0 ? gl_InvocationID : 0;
  mat4 vivify_LeftToRight = mat4(vivify_cb_UnityStereoGlobals[28], vivify_cb_UnityStereoGlobals[29], vivify_cb_UnityStereoGlobals[30], vivify_cb_UnityStereoGlobals[31]);
  vec4 centre = (gl_in[0].gl_Position + gl_in[1].gl_Position + gl_in[2].gl_Position) / 3.0;
  if (vivify_ViewIndex[0] < 0) return;
""" + "".join(
    f"  gl_Position = gl_in[{i}].gl_Position + (gl_in[{i}].gl_Position - centre) * _WireThickness;\n"
    f"  gs_TEXCOORD1 = vs_TEXCOORD1[{i}];\n  vs_TEXCOORD2 = centre;\n" + EYE_COPY + "  EmitVertex();\n"
    for i in range(3)) + "  EndPrimitive();\n  return;\n}\n"

FS = """#version 320 es
precision highp float;
precision highp int;
in vec4 gs_TEXCOORD1;
in vec4 vs_TEXCOORD2;
layout(location = 0) out vec4 SV_Target0;
void main() {
  SV_Target0 = vec4(min(min(gs_TEXCOORD1.x, gs_TEXCOORD1.y), gs_TEXCOORD1.z) * gs_TEXCOORD1.w) + vs_TEXCOORD2 * 0.0;
}
"""

REFUSED = {
    "four vertices": GS_WIREFRAME.replace("max_vertices = 3", "max_vertices = 4"),
    "a loop": GS_WIREFRAME.replace("  return;\n}\n", "  for (int i = 0; i < 1; i++) {}\n  return;\n}\n"),
    "points in": GS_WIREFRAME.replace("layout(triangles) in;", "layout(points) in;"),
}


def main():
    work = tempfile.mkdtemp(prefix="vivify-lower-")
    tool = os.path.join(work, "lower")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", os.path.join(ROOT, "include"), "-o", tool,
                    os.path.join(HERE, "lower_main.cpp"), os.path.join(ROOT, "src", "VivifyGeometryLowering.cpp")],
                   check=True)

    def write(name, text):
        path = os.path.join(work, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    vs = write("in.vert.txt", VS)
    frag = write("in.frag", FS)
    failures = 0
    for label, gs_text in (("wireframe", GS_WIREFRAME), ("exploding triangles", GS_EXPLODE)):
        gs = write(label.replace(" ", "_") + ".geom.txt", gs_text)
        run = subprocess.run([tool, vs, gs], capture_output=True, text=True)
        if run.returncode != 0:
            print(f"FAIL {label}: {run.stderr.strip()}")
            failures += 1
            continue
        lowered = write(label.replace(" ", "_") + ".vert", run.stdout)
        if "vivify:lowered-triangle-geometry" not in run.stdout or "gl_Layer" in run.stdout or \
                "gl_InvocationID" in run.stdout:
            print(f"FAIL {label}: lowered source is missing the marker or kept geometry-only builtins")
            failures += 1
            continue
        check = subprocess.run(["glslangValidator", "-l", lowered, frag], capture_output=True, text=True)
        if check.returncode != 0:
            print(f"FAIL {label}: glslangValidator\n{check.stdout}{check.stderr}")
            failures += 1
            continue
        print(f"ok   {label}: lowered, compiles and links")
    for label, gs_text in REFUSED.items():
        gs = write("refuse.geom.txt", gs_text)
        run = subprocess.run([tool, vs, gs], capture_output=True, text=True)
        if run.returncode == 0:
            print(f"FAIL refuse {label}: was lowered")
            failures += 1
        else:
            print(f"ok   refuses {label}: {run.stderr.strip()}")
    # A stage that only draws in orthographic cameras (AreaLit's light
    # meshes): its opening check moves into the flattened vertex program.
    gs_gated = GS_HEAD.replace("uniform float _WireThickness;",
                               "uniform float _WireThickness;\nuniform vec4 unity_OrthoParams;") + """void main() {
  ivec4 r0, r1;
  int vivify_EyeCopy = VIVIFY_EYE_COPY != 0 ? gl_InvocationID : 0;
  r0.x = (unity_OrthoParams.w != 1.0) ? -1 : 0;
  if (r0.x != 0) {
    return;
  }
""" + emit(0, "1.0, 0.0, 0.0") + emit(1, "0.0, 1.0, 0.0") + emit(2, "0.0, 0.0, 1.0") + "  return;\n}\n"
    gs = write("gated.geom.txt", gs_gated)
    run = subprocess.run([tool, "--gate", vs, gs], capture_output=True, text=True)
    plain_fs = write("gated.frag", FS.replace("in vec4 gs_TEXCOORD1;\nin vec4 vs_TEXCOORD2;\n", "in vec4 vs_TEXCOORD1;\n")
                     .replace("min(min(gs_TEXCOORD1.x, gs_TEXCOORD1.y), gs_TEXCOORD1.z) * gs_TEXCOORD1.w) + vs_TEXCOORD2 * 0.0",
                              "vs_TEXCOORD1.x)"))
    if run.returncode != 0 or "if (unity_OrthoParams.w != 1.0) gl_Position = vec4(0.0);" not in run.stdout \
            or run.stdout.count("uniform vec4 unity_OrthoParams;") != 1:
        print(f"FAIL gate: {run.stderr.strip()}\n{run.stdout}")
        failures += 1
    else:
        gated = write("gated.vert", run.stdout)
        check = subprocess.run(["glslangValidator", "-l", gated, plain_fs], capture_output=True, text=True)
        if check.returncode != 0:
            print(f"FAIL gate: glslangValidator\n{check.stdout}{check.stderr}")
            failures += 1
        else:
            print("ok   gate: the orthographic-camera check is kept and compiles")
    # A check that reads a varying is not a camera check: no gate.
    gs = write("ungated.geom.txt", gs_gated.replace("(unity_OrthoParams.w != 1.0)", "(vs_TEXCOORD1[0].w != 1.0)"))
    run = subprocess.run([tool, "--gate", vs, gs], capture_output=True, text=True)
    if run.returncode == 0:
        print("FAIL gate: a varying-based check was taken as a gate")
        failures += 1
    else:
        print("ok   no gate for a varying-based check")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
