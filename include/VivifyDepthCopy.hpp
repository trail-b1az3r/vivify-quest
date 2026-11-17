#pragma once
namespace UnityEngine { class RenderTexture; }
namespace Vivify::DepthCopy {
// Main thread only. Uses the original PC Vivify depth-copy materials,
// translated to GLES, including explicit per-layer array destinations.
bool Prepare();
bool Copy(UnityEngine::RenderTexture* destination);
}
