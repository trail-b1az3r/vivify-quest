#include "VivifyDepthCopy.hpp"
#include "VivifyTypes.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"
#include "UnityEngine/HideFlags.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <string>

#ifdef VIVIFY_DEPTH_BUNDLE
asm(".section .rodata\n"
    ".global vivify_depth_bundle_start\n.balign 16\nvivify_depth_bundle_start:\n"
    ".incbin \"" VIVIFY_DEPTH_BUNDLE "\"\n"
    ".global vivify_depth_bundle_end\nvivify_depth_bundle_end:\n.previous\n");
extern "C" unsigned char const vivify_depth_bundle_start[], vivify_depth_bundle_end[];
#endif

namespace Vivify::DepthCopy {
namespace {
constexpr char kPath[] = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/DepthBlit.bundle";
SafePtr<UnityEngine::Material> depth, array;
bool attempted = false;
bool loaded = false;
using SliceBlit = void (*)(UnityEngine::Texture*, UnityEngine::RenderTexture*, UnityEngine::Material*, int, bool, int);
SliceBlit sliceBlit = nullptr;
template <class T> bool Alive(T* object) { return IsManagedAlive(object); }
bool WriteBundle() {
#ifndef VIVIFY_DEPTH_BUNDLE
  return false;
#else
  size_t const size = reinterpret_cast<uintptr_t>(vivify_depth_bundle_end) - reinterpret_cast<uintptr_t>(vivify_depth_bundle_start);
  if (!size) return false;
  std::error_code ec;
  if (std::filesystem::file_size(kPath, ec) == size && !ec) {
    std::ifstream input(kPath, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(input)), {});
    if (data.size() == size && std::equal(data.begin(), data.end(), vivify_depth_bundle_start,
        [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; })) return true;
  }
  std::filesystem::create_directories(std::filesystem::path(kPath).parent_path(), ec);
  if (ec) return false;
  std::string const staged = std::string(kPath) + ".tmp";
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char const*>(vivify_depth_bundle_start), static_cast<std::streamsize>(size));
    out.close();
    if (!out) { std::filesystem::remove(staged, ec); return false; }
  }
  std::filesystem::rename(staged, kPath, ec);
  return !ec;
#endif
}
}
bool Prepare() {
  if (depth && array && Alive(depth.ptr()) && Alive(array.ptr())) return true;
  if (attempted && !loaded) return false;
  attempted = true;
  loaded = false;
  try {
    sliceBlit = reinterpret_cast<SliceBlit>(il2cpp_functions::resolve_icall("UnityEngine.Graphics::Internal_BlitMaterial6"));
    if (!WriteBundle()) { PaperLogger.error("Vivify depth copy: embedded PC depth bundle could not be written"); return false; }
    auto bundle = UnityEngine::AssetBundle::LoadFromFile(StringW(kPath));
    if (!Alive(bundle.unsafePtr())) { PaperLogger.error("Vivify depth copy: translated PC depth bundle could not be loaded"); return false; }
    auto load = [&](char const* path) {
      auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(bundle->LoadAsset(StringW(path)).unsafePtr()).value_or(nullptr);
      if (!Alive(material) || !Alive(material->get_shader().unsafePtr()) || !material->get_shader()->get_isSupported()) return static_cast<UnityEngine::Material*>(nullptr);
      material->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
      return material;
    };
    depth = load("assets/depthblit.mat"); array = load("assets/depthblitarrayslice.mat");
    bundle->Unload(false);
    PaperLogger.info("Vivify depth copy: PC 2D material={} PC array-slice material={} native slice binding={}", bool(depth), bool(array), sliceBlit != nullptr);
    loaded = depth && array;
    return loaded;
  } catch (std::exception const& ex) { PaperLogger.error("Vivify depth copy setup failed: {}", ex.what()); }
    catch (...) { PaperLogger.error("Vivify depth copy setup failed"); }
  return false;
}
bool Copy(UnityEngine::RenderTexture* destination) {
  if (!Alive(destination) || !Prepare()) return false;
  auto desc = destination->get_descriptor();
  if (desc.get_msaaSamples() != 1) return false;
  if (desc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray) {
    if (!sliceBlit || desc.get_volumeDepth() != 2) return false;
    for (int eye = 0; eye != 2; ++eye) {
      array->SetFloat(u"_ArraySliceIndex", static_cast<float>(eye));
      sliceBlit(nullptr, destination, array.ptr(), -1, true, eye);
    }
  } else if (desc.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2D) {
    UnityEngine::Graphics::Blit(nullptr, destination, depth.ptr());
  } else return false;
  return true;
}
}
