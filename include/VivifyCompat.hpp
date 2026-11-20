#pragma once

// beatsaber-hook compatibility. The 1.40.8 build uses beatsaber-hook 6
// (SafePtr, il2cpp_utils, il2cpp_functions, Configuration); the 1.45.3 build
// (VIVIFY_GAME_1_45_3) uses beatsaber-hook 9, which renamed all of them
// (safe_ptr, i2c::, i2c::functions) and dropped Configuration. Vivify's code
// keeps the old names; under 1.45.3 this header maps them onto the new API
// with the same behaviour (SafePtr::ptr() still throws on an empty handle).

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#ifndef VIVIFY_GAME_1_45_3

#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"

// A C# `in` (readonly ref) parameter in a hooked signature.
#define VIVIFY_IN(T) ByRef<T>

namespace Vivify {
// A C# exception thrown into C++ by a game call, and its text.
using CSharpException = Il2CppExceptionWrapper;
inline std::string ExceptionText(CSharpException const& ex) { return il2cpp_utils::ExceptionToString(ex.ex); }
}  // namespace Vivify

#else

#include "beatsaber-hook/shared/rapidjson.hpp"
#include "beatsaber-hook/shared/api.hpp"
#include "beatsaber-hook/shared/byref.hpp"
#include "beatsaber-hook/shared/exceptions.hpp"
#include "beatsaber-hook/shared/hooking.hpp"
#include "beatsaber-hook/shared/safeptr.hpp"
#include "beatsaber-hook/shared/stringw.hpp"
#include "beatsaber-hook/shared/types.hpp"
#include "beatsaber-hook/shared/utils.hpp"
#include "scotland2/shared/modloader.h"

// beatsaber-hook 6 names. SafePtr never checked Unity liveness in operator
// bool; SafePtrUnity did.
template <typename T>
using SafePtr = safe_ptr<T*, false>;
template <typename T>
using SafePtrUnity = safe_ptr<T*, true>;

// bs-cordl 4503 renamed UnityW::unsafePtr() to unsafe_ptr(). No 1.45.3
// library uses the old spelling, so a plain rename is safe.
#define unsafePtr unsafe_ptr

// bs-cordl 4503's by_ref<T> is beatsaber-hook 6's ByRef<T>; C# `in`
// parameters are by_ref<T const>.
template <typename T>
using ByRef = by_ref<T>;
template <typename T>
inline by_ref<T> byref(T& value) { return by_ref<T>(value); }
#define VIVIFY_IN(T) by_ref<T const>

// beatsaber-hook 6's csTypeOf(T*): the System.Type of T.
#define csTypeOf(...) reinterpret_cast<::System::Type*>(::i2c::cs_type_of<__VA_ARGS__>().convert())

// beatsaber-hook 6's classof(T*).
#define classof(...) (::i2c::class_of<__VA_ARGS__>())

namespace il2cpp_functions {
using namespace ::i2c::functions;
inline void Init() { ::i2c::functions::initialize(); }
}  // namespace il2cpp_functions

namespace il2cpp_utils {
// beatsaber-hook 6: try_cast<T>(inst) -> std::optional<T*>, empty for a null
// instance or a failed cast.
template <typename T, typename U>
inline std::optional<T*> try_cast(U* inst) noexcept {
  if (!inst) return std::nullopt;
  if (T* out = ::i2c::try_cast<T*>(inst)) return out;
  return std::nullopt;
}
template <typename T, typename U>
requires(requires(U const& u) { u.convert(); })
inline std::optional<T*> try_cast(U const& inst) noexcept {
  return try_cast<T>(reinterpret_cast<Il2CppObject*>(inst.convert()));
}
namespace detail {
inline std::string to_string(Il2CppString* str) { return str ? ::i2c::strs::to_string(str) : std::string(); }
template <typename S>
requires(requires(S const& s) { s.convert(); })
inline std::string to_string(S const& str) { return to_string(reinterpret_cast<Il2CppString*>(str.convert())); }
}  // namespace detail
}  // namespace il2cpp_utils

namespace Vivify {
// C# exceptions surface as i2c::trace_exception in beatsaber-hook 9.
using CSharpException = ::i2c::trace_exception;
inline std::string ExceptionText(CSharpException const& ex) { return ex.std::runtime_error::what(); }
}  // namespace Vivify

// beatsaber-hook 6's Configuration: a JSON document at the mod's config path,
// created empty when missing or unreadable.
class Configuration {
 public:
  rapidjson::Document config;

  explicit Configuration(modloader::ModInfo const& info) : _path(get_config_path(info)) { config.SetObject(); }

  void Load() {
    std::ifstream in(_path);
    if (in) {
      std::stringstream buffer;
      buffer << in.rdbuf();
      std::string const text = buffer.str();
      config.Parse(text.c_str());
      if (!config.HasParseError() && config.IsObject()) return;
    }
    config.SetObject();
    Write();
  }

  void Write() {
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    config.Accept(writer);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(_path).parent_path(), ec);
    std::ofstream out(_path, std::ios::trunc);
    out << buffer.GetString();
  }

 private:
  std::string _path;
};

#endif
