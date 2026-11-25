// Meshes for shaders whose geometry stage the converter moved into the vertex
// stage (0.15.55, VivifyGeometryLowering.hpp). Each renderer drawn with such a
// shader gets a copy of its mesh in which every triangle has its own three
// vertices, each carrying all three corners of its triangle (UV channels 5, 6
// and 7) and which corner it is (UV channel 4: x the corner, w = 1).

#include "VivifyLoweredGeometry.hpp"

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "VivifyRuntimeInternal.hpp"
#include "UnityEngine/Bounds.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/MeshFilter.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/MeshTopology.hpp"
#include "UnityEngine/Rendering/IndexFormat.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/SkinnedMeshRenderer.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Vector4.hpp"

namespace Vivify::LoweredGeometry {
namespace {
std::mutex gNamesMutex;
std::set<std::string> gShaderNames;

struct Prepared {
  SafePtrUnity<UnityEngine::Mesh> source;
  SafePtrUnity<UnityEngine::Mesh> mesh;  // null: the source could not be prepared
};
std::unordered_map<UnityEngine::Mesh*, Prepared> gPrepared;
std::set<UnityEngine::Mesh*> gOurs;
std::set<std::string> gReported;
int gMeshesMade = 0;

void ReportOnce(std::string const& key, std::string const& text) {
  if (gReported.insert(key).second) PaperLogger.info("{}", text);
}

bool UsesLoweredShader(UnityEngine::Renderer* renderer) {
  auto materials = renderer->get_sharedMaterials();
  if (!materials) return false;
  std::lock_guard lock(gNamesMutex);
  if (gShaderNames.empty()) return false;
  for (int i = 0; i < materials.size(); i++) {
    auto* material = materials[i].unsafePtr();
    if (!IsManagedAlive(material)) continue;
    auto shader = material->get_shader();
    if (!IsManagedAlive(shader.unsafePtr())) continue;
    if (gShaderNames.contains(static_cast<std::string>(shader->get_name()))) return true;
  }
  return false;
}

UnityEngine::Mesh* Expand(UnityEngine::Mesh* source) {
  std::string const name = static_cast<std::string>(source->get_name());
  if (!source->get_isReadable()) {
    ReportOnce("unreadable:" + name, fmt::format("Vivify lowered geometry: mesh '{}' is not readable; it draws as "
                                                 "the plain mesh", name));
    return nullptr;
  }
  int const subMeshes = source->get_subMeshCount();
  std::vector<ArrayW<int32_t>> triangles;
  size_t total = 0;
  for (int s = 0; s < subMeshes; s++) {
    if (source->GetTopology(s) != UnityEngine::MeshTopology::Triangles) {
      ReportOnce("topology:" + name, fmt::format("Vivify lowered geometry: mesh '{}' is not all triangles; it draws "
                                                 "as the plain mesh", name));
      return nullptr;
    }
    triangles.push_back(source->GetTriangles(s));
    total += triangles.back() ? triangles.back().size() : 0;
  }
  if (total == 0 || total > 3'000'000) return nullptr;
  auto vertices = source->get_vertices();
  if (!vertices || vertices.size() == 0) return nullptr;
  auto normals = source->get_normals();
  auto uv = source->get_uv();
  auto colors = source->get_colors32();
  bool const hasNormals = normals && normals.size() == vertices.size();
  bool const hasUv = uv && uv.size() == vertices.size();
  bool const hasColors = colors && colors.size() == vertices.size();

  auto const count = static_cast<il2cpp_array_size_t>(total);
  auto outVertices = ArrayW<UnityEngine::Vector3>(count);
  auto outNormals = hasNormals ? ArrayW<UnityEngine::Vector3>(count) : ArrayW<UnityEngine::Vector3>();
  auto outUv = hasUv ? ArrayW<UnityEngine::Vector2>(count) : ArrayW<UnityEngine::Vector2>();
  auto outColors = hasColors ? ArrayW<UnityEngine::Color32>(count) : ArrayW<UnityEngine::Color32>();
  ArrayW<UnityEngine::Vector4> corner[4] = {ArrayW<UnityEngine::Vector4>(count), ArrayW<UnityEngine::Vector4>(count),
                                            ArrayW<UnityEngine::Vector4>(count), ArrayW<UnityEngine::Vector4>(count)};
  std::vector<ArrayW<int32_t>> outTriangles;
  size_t at = 0;
  int const vertexCount = static_cast<int>(vertices.size());
  for (auto const& tri : triangles) {
    size_t const n = tri ? tri.size() : 0;
    auto indices = ArrayW<int32_t>(static_cast<il2cpp_array_size_t>(n));
    for (size_t t = 0; t + 2 < n; t += 3) {
      int idx[3] = {tri[t], tri[t + 1], tri[t + 2]};
      for (int& i : idx)
        if (i < 0 || i >= vertexCount) i = 0;
      UnityEngine::Vector4 points[3];
      for (int c = 0; c < 3; c++) {
        auto const& p = vertices[idx[c]];
        points[c] = UnityEngine::Vector4(p.x, p.y, p.z, 1.0f);
      }
      for (int c = 0; c < 3; c++) {
        size_t const v = at + t + c;
        outVertices[v] = vertices[idx[c]];
        if (hasNormals) outNormals[v] = normals[idx[c]];
        if (hasUv) outUv[v] = uv[idx[c]];
        if (hasColors) outColors[v] = colors[idx[c]];
        corner[0][v] = UnityEngine::Vector4(static_cast<float>(c), 0.0f, 0.0f, 1.0f);
        corner[1][v] = points[0];
        corner[2][v] = points[1];
        corner[3][v] = points[2];
        indices[t + c] = static_cast<int32_t>(v);
      }
    }
    outTriangles.push_back(indices);
    at += n;
  }

  auto* mesh = UnityEngine::Mesh::New_ctor();
  if (!IsManagedAlive(mesh)) return nullptr;
  mesh->set_name(StringW(name + " (Vivify per-triangle)"));
  if (total > 65535) mesh->set_indexFormat(UnityEngine::Rendering::IndexFormat::UInt32);
  mesh->SetVertices(outVertices);
  if (hasNormals) mesh->set_normals(outNormals);
  if (hasUv) mesh->set_uv(outUv);
  if (hasColors) mesh->set_colors32(outColors);
  for (int c = 0; c < 4; c++) mesh->SetUVs(4 + c, corner[c]);
  mesh->set_subMeshCount(subMeshes);
  for (int s = 0; s < subMeshes; s++) mesh->SetTriangles(outTriangles[static_cast<size_t>(s)], s, false);
  mesh->set_bounds(source->get_bounds());
  gMeshesMade++;
  return mesh;
}

UnityEngine::Mesh* PreparedFor(UnityEngine::Mesh* source) {
  if (gOurs.contains(source)) return source;
  if (auto it = gPrepared.find(source); it != gPrepared.end()) {
    if (it->second.source && IsManagedAlive(it->second.source.ptr())) {
      return it->second.mesh && IsManagedAlive(it->second.mesh.ptr()) ? it->second.mesh.ptr() : nullptr;
    }
    gPrepared.erase(it);  // a mesh from an unloaded bundle, its address reused
  }
  UnityEngine::Mesh* mesh = nullptr;
  try {
    mesh = Expand(source);
  } catch (...) {
    mesh = nullptr;
  }
  Prepared entry;
  entry.source = source;
  if (mesh) {
    entry.mesh = mesh;
    gOurs.insert(mesh);
  }
  gPrepared.erase(source);
  gPrepared.emplace(source, std::move(entry));
  return mesh;
}
}  // namespace

void NoteShaders(std::vector<std::string> const& names) {
  if (names.empty()) return;
  std::lock_guard lock(gNamesMutex);
  for (auto const& name : names) gShaderNames.insert(name);
}

void Prepare(UnityEngine::GameObject* root) {
  if (!IsManagedAlive(root)) return;
  {
    std::lock_guard lock(gNamesMutex);
    if (gShaderNames.empty()) return;
  }
  int prepared = 0;
  auto renderers = root->GetComponentsInChildren<UnityEngine::Renderer*>(true);
  for (int i = 0; renderers && i < renderers.size(); i++) {
    auto* renderer = renderers[i];
    if (!IsManagedAlive(renderer) || !UsesLoweredShader(renderer)) continue;
    if (auto* skinned = il2cpp_utils::try_cast<UnityEngine::SkinnedMeshRenderer>(renderer).value_or(nullptr)) {
      ReportOnce("skinned:" + static_cast<std::string>(skinned->get_name()),
                 fmt::format("Vivify lowered geometry: '{}' is a skinned mesh; it draws as the plain mesh",
                             static_cast<std::string>(skinned->get_name())));
      continue;
    }
    auto filter = renderer->get_gameObject()->GetComponent<UnityEngine::MeshFilter*>();
    if (!IsManagedAlive(filter)) continue;
    auto source = filter->get_sharedMesh();
    if (!IsManagedAlive(source.unsafePtr()) || gOurs.contains(source.unsafePtr())) continue;
    if (auto* mesh = PreparedFor(source.unsafePtr())) {
      filter->set_sharedMesh(mesh);
      prepared++;
    }
  }
  if (prepared > 0) {
    ReportOnce("prepared:" + static_cast<std::string>(root->get_name()),
               fmt::format("Vivify lowered geometry: '{}' -- {} renderer(s) given per-triangle meshes for their "
                           "wireframe-style shaders ({} mesh(es) built so far)",
                           static_cast<std::string>(root->get_name()), prepared, gMeshesMade));
  }
}

}  // namespace Vivify::LoweredGeometry
