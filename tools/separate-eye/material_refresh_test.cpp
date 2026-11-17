// The runner extracts the actual production material method bodies. These small
// stand-ins model documented Unity material-copy semantics, not GPU drawing.
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ActualNativePolicy.hpp"

template<class T> struct Ptr {
  T* value = nullptr;
  Ptr() = default;
  Ptr(T* p) : value(p) {}
  T* ptr() const { return value; }
  T* unsafePtr() const { return value; }
  T* operator->() const { return value; }
  explicit operator bool() const { return value != nullptr; }
};
namespace UnityEngine {
struct Shader { std::string name; bool supported = true; bool get_isSupported() const { return supported; } std::string get_name() const { return name; } };
struct Color { float r = 0, g = 0, b = 0, a = 1; };
enum class HideFlags { None, DontUnloadUnusedAsset };
namespace Rendering { enum class TextureDimension { Tex2D, Tex2DArray }; }
struct Descriptor {
  Rendering::TextureDimension dimension = Rendering::TextureDimension::Tex2D;
  int depth = 1;
  auto get_dimension() const { return dimension; }
  int get_volumeDepth() const { return depth; }
};
struct RenderTexture { Descriptor descriptor; Descriptor get_descriptor() const { return descriptor; } };
struct Material {
  Shader* shader = nullptr;
  std::string name = "fixture";
  int texture = 0, scalar = 0, keywords = 0, queue = 2000;
  int copies = 0, binds = 0, shaderAssignments = 0;
  bool instancing = true, throwOnCopy = false;
  HideFlags flags = HideFlags::None;
  Color color;
  static std::vector<Material*> allocated;
  static Material* New_ctor(Material* source) {
    auto* result = new Material(*source); allocated.push_back(result); return result;
  }
  Ptr<Shader> get_shader() const { return shader; }
  void set_shader(Shader* value) { shader = value; ++shaderAssignments; }
  void set_enableInstancing(bool value) { instancing = value; }
  void set_hideFlags(HideFlags value) { flags = value; }
  std::string get_name() const { return name; }
  bool HasProperty(char16_t const*) const { return true; }
  Color GetColor(char16_t const*) const { return color; }
  void SetColor(char16_t const*, Color value) { color = value; }
  void CopyPropertiesFromMaterial(Material* source) {
    if (source->throwOnCopy) throw std::runtime_error("injected copy failure");
    texture = source->texture; scalar = source->scalar; keywords = source->keywords;
    queue = source->queue; instancing = source->instancing; color = source->color;
    ++copies;
    // Unity documents that shader and Object.hideFlags are not replaced.
  }
};
std::vector<Material*> Material::allocated;
}
template<class T> bool Alive(T* p) { return p != nullptr; }
template<class T> bool IsAlive(T* p) { return p != nullptr; }
std::string ToStdString(std::string const& s) { return s; }
std::string ShaderNameForLog(UnityEngine::Shader* s) { return s->name; }
struct Logger { template<class... T> void info(T const&...) {} } PaperLogger;

namespace Vivify {
namespace MaterialSpecialization { std::string Canonical(std::string const& name) { return name; } }
namespace StereoBindings { using Mask = uint16_t; constexpr Mask All = 4095; }
namespace SeparateEye {
using UnityEngine::Material;
using UnityEngine::Shader;
struct Original {
  std::optional<UnityEngine::Color> color, fallbackColor;
  Ptr<Shader> sourceShader;
  std::string shader;
};
struct Pair {
  Ptr<Material> source;
  std::array<Ptr<Material>, 6> copies;
  std::array<uint64_t, 6> synchronized{};
};
std::unordered_map<Material*, Original> originals;
std::unordered_map<Material*, Pair> materials;
std::unordered_map<Material*, Material*> cloneSources;
std::unordered_map<Shader*, StereoBindings::Mask> shaderBindings;
std::unordered_map<Shader*, std::array<Shader*, 6>> donors;
uint64_t materialEpoch = 1;
struct NativeCheck { uint64_t epoch = 0; bool compatible = false; };
std::unordered_map<Material*, NativeCheck> nativeChecks;
uint64_t nativeCheckEpoch = 1;
bool completeStageInfo = true;
std::unordered_set<std::string> separateEyeShaderNames, activeSceneEyeTessellationNames;
int selectedEye = -1;
std::string MaterialShaderName(Material* source) { return source->shader->name; }
Shader* FindDonor(Material* source, int mode) { return donors.at(source->shader).at(mode); }
void BindStereo(Material* material, StereoBindings::Mask, bool) { ++material->binds; }
#include "ActualCopyMaterial.inc"
#include "ActualCanUseNativeMaterial.inc"
Material* BlitMaterial(Material* source) { return selectedEye < 0 ? source : CopyMaterial(source, selectedEye + 3); }
}
struct Runtime {
  std::unordered_map<std::string, UnityEngine::Shader*> _singleViewShaders;
  std::unordered_map<UnityEngine::Material*, UnityEngine::Material*> _singleViewBlitMaterials;
  UnityEngine::Material* BlitMaterialFor(UnityEngine::Material*, UnityEngine::RenderTexture*);
};
#include "ActualBlitMaterialFor.inc"
}

int main() {
  using namespace UnityEngine;
  using namespace Vivify;
  using namespace Vivify::SeparateEye;
  int checks = 0, failures = 0;
  auto check = [&](bool condition, char const* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
  };
  Shader sourceA{"sourceA"}, sourceB{"sourceB"}, companionA{"monoA"}, companionB{"monoB"};
  std::array<Shader, 6> a, b;
  for (int i = 0; i != 6; ++i) {
    a[i].name = "a" + std::to_string(i); b[i].name = "b" + std::to_string(i);
    donors[&sourceA][i] = &a[i]; donors[&sourceB][i] = &b[i];
  }
  Material source; source.shader = &sourceA; source.texture = 1; source.scalar = 7;
  originals[&source] = {};
  Runtime runtime;
  runtime._singleViewShaders = {{sourceA.name, &companionA}, {sourceB.name, &companionB}};
  RenderTexture target;
  auto* first = runtime.BlitMaterialFor(&source, &target);
  check(first != &source && first->shader == &companionA && first->texture == 1, "initial companion");
  auto const firstAssignments = first->shaderAssignments;
  source.texture = 2;
  check(runtime.BlitMaterialFor(&source, &target) == first && first->texture == 2, "companion property refresh");
  check(first->shaderAssignments == firstAssignments, "unchanged companion avoids redundant shader assignment");
  source.shader = &sourceB;
  check(runtime.BlitMaterialFor(&source, &target) == first && first->shader == &companionB, "companion follows live source shader");
  first->shader = &sourceA;
  check(runtime.BlitMaterialFor(&source, &target) == first && first->shader == &companionB, "companion repairs stale cached shader");
  source.shader = &sourceA;
  check(runtime.BlitMaterialFor(&source, &target) == first && first->shader == &companionA, "companion switches back");
  check(first->flags == HideFlags::DontUnloadUnusedAsset && source.flags == HideFlags::None, "private lifetime flags preserved");
  target.descriptor.dimension = Rendering::TextureDimension::Tex2DArray;
  check(runtime.BlitMaterialFor(&source, &target) == &source, "ordinary layered destination retains native path");
  target.descriptor = {};

  for (int mode : {3, 4}) {
    source.texture = 10; source.scalar = 11; source.keywords = 12; source.queue = 2400;
    auto* left = CopyMaterial(&source, mode);
    int const epoch = materialEpoch, originalCopies = left->copies, assignments = left->shaderAssignments;
    source.texture = 20; source.scalar = 21; source.keywords = 22; source.queue = 2900;
    auto* refreshed = CopyMaterial(&source, mode);
    check(materialEpoch == static_cast<uint64_t>(epoch) && refreshed == left, "same epoch keeps blit copy identity");
    check(left->texture == 20 && left->scalar == 21 && left->keywords == 22 && left->queue == 2900,
          "same-cycle blit refreshes all material values");
    check(left->copies > originalCopies && !left->instancing && left->binds >= 2, "blit restores explicit-eye bindings after refresh");
    check(left->shaderAssignments == assignments, "unchanged eye donor avoids redundant shader assignment");
    source.shader = &sourceB;
    check(CopyMaterial(&source, mode) == left && left->shader == &b[mode], "same-cycle blit follows source shader");
    source.shader = &sourceA;
    check(CopyMaterial(left, mode) == left && left->shader == &a[mode], "blit clone resolves authored source");
    source.throwOnCopy = true;
    bool threw = false;
    try { CopyMaterial(&source, mode); } catch (std::runtime_error const&) { threw = true; }
    check(threw, "failed blit refresh is not marked successful");
    source.throwOnCopy = false; source.texture = 99;
    check(CopyMaterial(&source, mode)->texture == 99, "blit retry refreshes after failure");
  }
  check(materials[&source].copies[3].ptr() != materials[&source].copies[4].ptr(), "eyes keep independent blit copies");
  for (int mode : {0, 1, 2, 5}) {
    source.shader = &sourceA;
    auto* scene = CopyMaterial(&source, mode); auto const count = scene->copies;
    check(CopyMaterial(&source, mode) == scene && scene->copies == count, "unchanged scene snapshot avoids redundant copies");
    source.shader = &sourceB;
    check(CopyMaterial(&source, mode) == scene && scene->shader == &b[mode], "changed donor invalidates scene snapshot");
    source.shader = &sourceA; ++materialEpoch; source.texture = mode + 100;
    check(CopyMaterial(&source, mode)->texture == mode + 100 && scene->shader == &a[mode], "next snapshot refreshes scene properties");
  }
  selectedEye = 1; source.texture = 197;
  check(runtime.BlitMaterialFor(&source, &target) == materials[&source].copies[4].ptr() &&
        materials[&source].copies[4]->texture == 197, "runtime dispatch uses current explicit eye");
  selectedEye = -1;
  check(source.shader == &sourceA && source.instancing && source.flags == HideFlags::None, "authored source remains untouched");
  originals[&source].sourceShader = &sourceA;
  originals[&source].shader = sourceA.name;
  separateEyeShaderNames.insert(sourceA.name); // Source has a geometry stage.
  auto native = [&] { ++nativeCheckEpoch; return CanUseNativeMaterial(&source); };
  check(native(), "supported upstream geometry uses native scene path");
  source.shader = &sourceB;
  check(!CanUseNativeMaterial(&source), "same-cycle shader change invalidates native permission");
  source.shader = nullptr;
  check(!CanUseNativeMaterial(&source), "same-cycle removed shader invalidates native permission");
  source.shader = &sourceA;
  check(CanUseNativeMaterial(&source), "restored proven shader can reuse same-cycle native permission");
  activeSceneEyeTessellationNames.insert(sourceA.name);
  check(!native(), "active paired tessellation keeps its scene-eye path");
  activeSceneEyeTessellationNames.clear();
  check(native(), "declined or disabled tessellation retains upstream native fallback");
  sourceA.supported = false;
  check(!native(), "unsupported native shader retains donor fallback"); sourceA.supported = true;
  source.shader = &sourceB;
  check(!native(), "unproven live shader assignment cannot take native shortcut"); source.shader = &sourceA;
  completeStageInfo = false;
  check(!native(), "incomplete stage metadata retains conservative fallback"); completeStageInfo = true;
  Material unknown; unknown.shader = &sourceA;
  check(!CanUseNativeMaterial(&unknown), "unknown source identity cannot take native shortcut");
  check(CanUseNativeMaterial(nullptr), "unshaded passthrough preserves native path");
  for (auto* material : Material::allocated) delete material;
  std::printf("%d material refresh checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
