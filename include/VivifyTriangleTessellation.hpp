#pragma once
#include "VivifyDxbc.hpp"

namespace Vivify::Dxbc {
// Offline prototype, deliberately separate from TranslateToGlsl and bundle
// conversion. A successful pair is NOT authorization to discard retained DXBC
// stages: Unity program metadata, patch draws and stereo still need validation.
struct TriangleTessellationResult {
  bool ok = false;
  std::string error;
  std::string factorUniform;
  GlslResult control, evaluation;
};

// Three unchanged control points, integer partitioning, clockwise triangles,
// two uniform-factor fork phases, and a MOV/MUL/MAD domain program. All other
// phases, topology, resources and instructions fail closed. Factor binding
// must be proved by RDEF or the actual hull program's external reflection.
TriangleTessellationResult TranslateTriangleTessellation(
    Program const& hull, Program const& domain, ExternalReflection const* hullReflection = nullptr);
}
