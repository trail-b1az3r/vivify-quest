#include "VivifyGeometrySpecialization.hpp"
#include "VivifyMaterialSpecialization.hpp"
#include <cassert>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace Vivify::GeometrySpecialization;
// A DOM fixture only: no parser or duplicate eligibility implementation.
struct JsonMember;
struct Json {
  enum Kind { Scalar, String, Object, Array } kind = Scalar;
  std::string text;
  std::vector<JsonMember> object;
  std::vector<Json> array;
  Json(); ~Json(); Json(Json const&); Json(Json&&) noexcept;
  Json& operator=(Json const&); Json& operator=(Json&&) noexcept;
  static Json Str(std::string value);
  static Json Obj(std::initializer_list<std::pair<std::string, Json>> values);
  static Json Arr(std::initializer_list<Json> values);
  bool IsString() const { return kind == String; }
  bool IsObject() const { return kind == Object; }
  bool IsArray() const { return kind == Array; }
  char const* GetString() const { return text.c_str(); }
  size_t GetStringLength() const { return text.size(); }
  auto const& GetObject() const { return object; }
  auto const& GetArray() const { return array; }
  bool HasMember(char const* key) const;
  Json const& operator[](char const* key) const;
};
struct JsonMember { Json name, value; };
Json::Json() = default; Json::~Json() = default; Json::Json(Json const&) = default;
Json::Json(Json&&) noexcept = default; Json& Json::operator=(Json const&) = default;
Json& Json::operator=(Json&&) noexcept = default;
Json Json::Str(std::string value) { Json j; j.kind=String; j.text=std::move(value); return j; }
Json Json::Obj(std::initializer_list<std::pair<std::string, Json>> values) {
  Json j; j.kind=Object; for (auto const& [key,value]:values) j.object.push_back({Str(key),value}); return j;
}
Json Json::Arr(std::initializer_list<Json> values) { Json j; j.kind=Array; j.array=values; return j; }
bool Json::HasMember(char const* key) const { for(auto const& m:object) if(m.name.text==key)return true; return false; }
Json const& Json::operator[](char const* key) const { for(auto const& m:object)if(m.name.text==key)return m.value; std::abort(); }
int main() {
  int count = 0;
  auto check = [&](bool valid, char const* name) { ++count; if (!valid) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); } };
  std::string const declaration = "uniform vec4 unity_OrthoParams;\n";
  std::string const locals = "ivec4 r0, r1; ivec4 x0[6]; ivec4 vPrimitiveID = ivec4(gl_PrimitiveID);";
  auto shader = [&](std::string body) { return declaration + "void main() {" + locals + body + "}"; };
  std::string const guard = "r0.x = (unity_OrthoParams.w != 1.0) ? -1 : 0; if (r0.x != 0) { return; }";
  check(EmptyForPerspective(shader(guard + "EmitVertex();")), "all geometry is behind perspective return");
  check(EmptyForPerspective(declaration + "void main(){ivec4 r37; r37.w=(unity_OrthoParams.w!=1.0)?-1:0;if(r37.w!=0){return;}EmitVertex();}"), "register renaming is exact");
  for (auto const& changed : std::vector<std::string>{
        "r0.x=(unity_OrthoParams.w==1.0)?-1:0;if(r0.x!=0){return;}",
        "r0.x=(unity_OrthoParams.x!=1.0)?-1:0;if(r0.x!=0){return;}",
        "r0.x=(unity_OrthoParams.w!=1.0)?-1:0;if(r1.x!=0){return;}",
        "r0.x=(unity_OrthoParams.w!=1.0)?-1:0;if(r0.x!=0){EmitVertex();return;}",
        "EmitVertex();" + guard,
        "imageStore(outputImage,ivec2(0),vec4(1));" + guard,
        "ivec4 r3 = ivec4(atomicAdd(counter,1));" + guard,
        "if (uniformValue > 0.0) { return; }" + guard,
        "r0.x=(unity_OrthoParams.w!=0.0)?-1:0;if(r0.x!=0){return;}"})
    check(!EmptyForPerspective(shader(changed)), "nearby but inequivalent program cannot be specialized");
  check(!EmptyForPerspective("void main(){" + guard + "}"), "unknown camera uniform cannot specialize");
  check(!EmptyForPerspective(declaration + "void main(){EmitVertex();}"), "ordinary geometry is retained");
  check(SideEffectFreeVertex("uniform mat4 unity_ObjectToWorld;void main(){gl_Position=vec4(1.0);}"), "ordinary vertex shader has no storage effects");
  for (auto const& changed : {"buffer Values { uint value; };", "uniform image2D target;", "atomicAdd(counter,1);", "memoryBarrier(); barrier();"})
    check(!SideEffectFreeVertex(changed), "vertex storage/synchronization blocks specialization");
  // Synthetic prefix truncation/corruption cannot crash the proof reader.
  auto good = shader(guard + "EmitVertex();");
  for (size_t length = 0; length < good.size(); ++length) (void)EmptyForPerspective(std::string_view(good).substr(0, length));
  check(NativeEmptyProgram().find("layout(num_views = 2)") != std::string::npos &&
        NativeEmptyProgram().find("#ifdef GEOMETRY") == std::string::npos, "empty program retains native multiview without geometry");
  namespace Fixed = Vivify::MaterialSpecialization;
  auto chart = [](char const* type) {
    return Json::Obj({{"version",Json::Str("3.3.0")},{"customData",Json::Obj({
      {"customEvents",Json::Arr({Json::Obj({{"t",Json::Str(type)}})})}})}});
  };
  for(auto type:{"InstantiatePrefab","DestroyObject","SetRenderingSettings"})
    check(Fixed::ChartAllows(chart(type)), "known non-material event schema can authorize the bundle proof");
  for(auto type:{"SetMaterialProperty","SetGlobalProperty","AnimateTrack","CreateCamera","FutureEvent"})
    check(!Fixed::ChartAllows(chart(type)), "mutable or unknown event blocks constants");
  check(!Fixed::ChartAllows(Json{}), "non-object chart rejected");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("3.3.0")}})), "missing custom data rejected");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("3.3.0")},{"version",Json::Str("3.3.0")}})), "duplicate JSON keys rejected");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("4.0.0")},{"customData",Json::Obj({{"customEvents",Json::Arr({})}})}})), "unknown format rejected");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("3.3.0")},{"customData",Json::Obj({
    {"customEvents",Json::Arr({})},{"materials",Json::Obj({})}})}})), "material definitions invalidate proof");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("3.3.0")},{"customData",Json::Obj({
    {"customEvents",Json::Arr({})},{"environment",Json::Obj({{"_EmissionEnable",Json{}}})}})}})), "feature references outside events rejected");
  check(!Fixed::ChartAllows(Json::Obj({{"version",Json::Str("3.3.0")},{"customData",Json::Obj({
    {"customEvents",Json::Arr({})},{"_customEvents",Json::Arr({})}})}})), "mixed event schemas rejected");
  check(Fixed::ChartAllows(Json::Obj({{"_version",Json::Str("2.6.0")},{"_customData",Json::Obj({
    {"_customEvents",Json::Arr({Json::Obj({{"_type",Json::Str("InstantiatePrefab")}})})}})}})), "known legacy event spelling is checked");
  std::map<std::string,float> values;
  for(auto const& [name,value]:Fixed::kFlags) values.emplace(name,value);
  check(Fixed::Matches(values), "exact material profile matches");
  values["_EmissionEnable"]=1;
  check(!Fixed::Matches(values), "changed material flag rejects fixed program");
  values.erase("_EmissionEnable");
  check(!Fixed::Matches(values), "absent material flag rejects fixed program");
  std::string program="uniform int _EmissionEnable;\nuniform float _MatCapMaskEnable;\nuniform int _Other;\n//uniform int _DecalEnable;\n";
  Fixed::Specialize(program);
  check(program=="const int _EmissionEnable = 0;\nconst float _MatCapMaskEnable = 1.0;\nuniform int _Other;\n//uniform int _DecalEnable;\n", "only exact declared profile scalars change; unrelated code remains intact");
  std::printf("%d geometry/material specialization checks passed\n", count);
}
