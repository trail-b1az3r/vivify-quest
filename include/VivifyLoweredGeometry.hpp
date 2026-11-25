#pragma once

// Run-time half of geometry-stage lowering (0.15.55); see
// VivifyGeometryLowering.hpp for the converter's half.

#include <string>
#include <vector>

namespace UnityEngine {
class GameObject;
}

namespace Vivify::LoweredGeometry {

// Shader names a bundle scan found lowered (any thread).
void NoteShaders(std::vector<std::string> const& names);

// Gives every renderer under root that draws with a lowered shader the
// per-triangle copy of its mesh (main thread). Cheap when nothing is lowered.
void Prepare(UnityEngine::GameObject* root);

}  // namespace Vivify::LoweredGeometry
