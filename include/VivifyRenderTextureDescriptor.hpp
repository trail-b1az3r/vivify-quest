#pragma once
#include "VivifyRenderTexturePolicy.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"

namespace Vivify::RenderTexturePolicy {
inline Descriptor Describe(UnityEngine::RenderTextureDescriptor const& d) {
  return {d._width_k__BackingField, d._height_k__BackingField, d._msaaSamples_k__BackingField,
          d._volumeDepth_k__BackingField, d._mipCount_k__BackingField, d._graphicsFormat.value__,
          d._stencilFormat_k__BackingField.value__, d._depthStencilFormat_k__BackingField.value__,
          d._dimension_k__BackingField.value__, d._shadowSamplingMode_k__BackingField.value__,
          d._vrUsage_k__BackingField.value__, d._flags.value__, d._memoryless_k__BackingField.value__};
}
}
