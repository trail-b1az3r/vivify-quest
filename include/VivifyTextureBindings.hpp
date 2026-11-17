#pragma once
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Texture.hpp"

namespace Vivify {
// A later camera or replacement screen may have reused the property name.
// Compare the current binding; deleting an owner must not unset its successor.
// No managed/native member of the owned texture is dereferenced here.
inline bool ClearOwnedTextureBinding(int property, UnityEngine::Texture* owned) {
  if (!owned || UnityEngine::Shader::GetGlobalTexture(property).unsafePtr() != owned) return false;
  UnityEngine::Shader::SetGlobalTexture(property, static_cast<UnityEngine::Texture*>(nullptr));
  return true;
}
}
