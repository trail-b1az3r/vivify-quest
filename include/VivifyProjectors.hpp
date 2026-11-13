#pragma once
#include "UnityEngine/Behaviour.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Object.hpp"
#include "main.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// The game strips the managed Projector class but retains the native feature.
// Unity returns these components as Behaviour wrappers. Validate them through
// its native class string AND its own component lookup before native access.
// No native offsets, fabricated wrappers or arbitrary Behaviour casts are used.
namespace Vivify::Projectors {
struct Bindings {
  using Get = UnityEngine::Material* (*)(UnityEngine::Behaviour*);
  using Set = void (*)(UnityEngine::Behaviour*, UnityEngine::Material*);
  Il2CppClass* type = nullptr;
  Get get = nullptr;
  Set set = nullptr;
  Bindings() {
    size_t count = 0;
    auto assemblies = il2cpp_functions::domain_get_assemblies(il2cpp_functions::domain_get(), &count);
    for (size_t i = 0; i < count && !type; ++i)
      type = il2cpp_functions::class_from_name(il2cpp_functions::assembly_get_image(assemblies[i]), "UnityEngine", "Projector");
    get = reinterpret_cast<Get>(il2cpp_functions::resolve_icall("UnityEngine.Projector::get_material"));
    set = reinterpret_cast<Set>(il2cpp_functions::resolve_icall("UnityEngine.Projector::set_material"));
    if (type && !il2cpp_functions::class_is_assignable_from(classof(UnityEngine::Behaviour*), type)) type = nullptr;
    PaperLogger.info("Vivify Projector interface: managedType={} nativeGetMaterial={} nativeSetMaterial={}", type != nullptr, get != nullptr, set != nullptr);
  }
};
inline Bindings& GetBindings() { static Bindings bindings; return bindings; }
inline bool Available() { auto& b = GetBindings(); return b.get && b.set; }
inline bool IsProjector(UnityEngine::Behaviour* object) {
  auto& b = GetBindings();
  if (!Available() || !object || !UnityEngine::Object::op_Implicit_bool(object)) return false;
  auto* cls = il2cpp_functions::object_get_class(reinterpret_cast<Il2CppObject*>(object));
  if (b.type) return il2cpp_functions::class_is_assignable_from(b.type, cls);
  // A managed script can override ToString; it cannot qualify for this path.
  // The exact base Behaviour wrapper is what this game's stripped native
  // Projectors were observed to use. Native object identity is then checked.
  if (cls != classof(UnityEngine::Behaviour*)) return false;
  static std::unordered_map<UnityEngine::Behaviour*, int> known;
  int const id = object->GetInstanceID();
  if (auto found = known.find(object); found != known.end() && found->second == id) return true;
  if (!std::string(object->ToString()).ends_with(" (UnityEngine.Projector)")) return false;
  auto* owner = object->get_gameObject().unsafePtr();
  if (!owner || owner->GetComponent(u"Projector").unsafePtr() != object) return false;
  if (known.size() > 1024) known.clear();
  known[object] = id;
  if (GetVivifyDebugLogging()) PaperLogger.info("Vivify Projector bridge: validated native component '{}' through Unity's Behaviour wrapper", std::string(owner->get_name()));
  return true;
}
inline std::vector<UnityEngine::Behaviour*> InChildren(UnityEngine::GameObject* root) {
  std::vector<UnityEngine::Behaviour*> result;
  if (!root || !Available()) return result;
  for (auto* component : root->GetComponentsInChildren<UnityEngine::Behaviour*>(true))
    if (IsProjector(component)) result.push_back(component);
  return result;
}
inline UnityEngine::Material* GetMaterial(UnityEngine::Behaviour* object) {
  return IsProjector(object) ? GetBindings().get(object) : nullptr;
}
inline void SetMaterial(UnityEngine::Behaviour* object, UnityEngine::Material* material) {
  if (!IsProjector(object)) throw std::runtime_error("Projector material binding unavailable");
  GetBindings().set(object, material);
}
}
