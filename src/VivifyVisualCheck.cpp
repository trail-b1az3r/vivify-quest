// Live checks of what a player can actually see, run a few times into each
// Vivify level (0.14.29). Reports like "notes are only their arrow" or "notes
// sometimes disappear" come without a headset to look through, and nothing in
// the log said what a note was made of at that moment. These checks write it
// down: a live note's renderers and shaders, how many notes have nothing left
// that can draw, the lights and shadows the map added, and whether the camera
// draws the notes' layer at all. Anything that looks wrong is also kept for the
// "Likely visual problems" list at the top of the level's VivifyReport block.

#include "VivifyRuntimeInternal.hpp"

#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/MaterialPropertyBlock.hpp"
#include "UnityEngine/Transform.hpp"
#include "GlobalNamespace/Mirror.hpp"
#include "GlobalNamespace/MirrorRendererSO.hpp"
#include "TMPro/TMP_Text.hpp"
#include "TMPro/TMP_TextInfo.hpp"
#include "TMPro/TMP_FontAsset.hpp"
#include "TMPro/TMP_SubMesh.hpp"
#include "TMPro/AtlasPopulationMode.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/Bounds.hpp"
#include "UnityEngine/Font.hpp"
#include "UnityEngine/ReflectionProbe.hpp"
#include "UnityEngine/RenderSettings.hpp"
#include "UnityEngine/Texture.hpp"
#include "UnityEngine/Rendering/ReflectionProbeMode.hpp"
#include "UnityEngine/Rendering/ReflectionProbeRefreshMode.hpp"
#include "UnityEngine/Rendering/DefaultReflectionMode.hpp"

namespace Vivify {

namespace {

// Song seconds at which a check runs: early (the opening has spawned notes),
// after the intro, and well into the song.
constexpr float kVisualCheckTimes[] = {5.0f, 30.0f, 90.0f};

int CutoutPropertyId() {
  static int id = UnityEngine::Shader::PropertyToID(u"_Cutout");
  return id;
}

}  // namespace

void Runtime::NoteVisualProblem(std::string problem) {
  if (std::find(_visualProblems.begin(), _visualProblems.end(), problem) != _visualProblems.end()) return;
  PaperLogger.warn("Vivify visual check: {}", problem);
  if (_visualProblems.size() < 40) _visualProblems.push_back(std::move(problem));
}

void Runtime::RunScheduledVisualChecks() {
  if (_pauseMenuActive || _isResetting || _currentBeatmapData == nullptr) return;
  if (!_textChecks.empty()) {
    try {
      RunTextChecks();
    } catch (...) {
      _textChecks.clear();
      PaperLogger.warn("Vivify text check threw");
    }
  }
  constexpr int count = static_cast<int>(sizeof(kVisualCheckTimes) / sizeof(kVisualCheckTimes[0]));
  if (_visualChecksRun >= count) return;
  float const songTime = CurrentSongTime();
  if (songTime < kVisualCheckTimes[_visualChecksRun]) return;
  _visualChecksRun++;
  try {
    RunVisualCheck(fmt::format("{:.0f}s", songTime));
  } catch (std::exception const& e) {
    PaperLogger.warn("Vivify visual check threw: {}", e.what());
  } catch (...) {
    PaperLogger.warn("Vivify visual check threw");
  }
}

// Delays after a spawn: once TextMeshPro has had a frame or two to build its
// mesh, and once a map's grow-in animation (Lifelike scales its runway text up
// from zero over four beats) has finished.
constexpr float kTextCheckDelays[] = {1.0f, 6.0f};

void Runtime::NoteSpawnedText(UnityEngine::GameObject* instance, std::string const& asset) {
  if (!IsAlive(instance) || _textChecks.size() >= 16) return;
  auto texts = instance->GetComponentsInChildren<TMPro::TMP_Text*>(true);
  if (!texts || texts.size() == 0) return;
  _textChecks.push_back(TextCheck{instance, asset, CurrentSongTime() + kTextCheckDelays[0], 0});
}

void Runtime::RunTextChecks() {
  float const now = CurrentSongTime();
  for (auto it = _textChecks.begin(); it != _textChecks.end();) {
    if (!IsAlive(it->instance)) { it = _textChecks.erase(it); continue; }
    if (now < it->due) { ++it; continue; }
    auto texts = it->instance->GetComponentsInChildren<TMPro::TMP_Text*>(true);
    for (int i = 0; texts && i < texts.size() && i < 8; i++) {
      auto* text = texts[i];
      if (!IsAlive(text)) continue;
      auto gameObject = text->get_gameObject();
      auto font = text->get_font();
      auto* info = text->get_textInfo();
      auto mesh = text->get_mesh();
      auto* material = text->get_fontSharedMaterial().unsafePtr();
      auto* shader = IsAlive(material) ? material->get_shader().unsafePtr() : nullptr;
      auto const scale = text->get_transform()->get_lossyScale();
      int vertices = -1;
      std::string uv2 = "-";
      std::string bounds = "-";
      if (IsAlive(mesh.unsafePtr())) {
        vertices = mesh->get_vertexCount();
        auto uvs = mesh->get_uv2();
        if (uvs && uvs.size() > 0) uv2 = fmt::format("({:.4g},{:.4g})", uvs[0].x, uvs[0].y);
        auto const size = mesh->get_bounds().get_size();
        bounds = fmt::format("({:.3g},{:.3g},{:.3g})", size.x, size.y, size.z);
      }
      auto* renderer = gameObject->GetComponent<UnityEngine::Renderer*>();
      auto subMeshes = text->GetComponentsInChildren<TMPro::TMP_SubMesh*>(true);
      auto const content = text->get_text();
      PaperLogger.info(
          "Vivify text check +{:.0f}s: prefab='{}' text='{}' active={} enabled={} chars={} shown={} "
          "lossyScale=({:.4g},{:.4g},{:.4g}) font='{}' dynamic={} sourceFont={} mesh vertices={} uv2[0]={} "
          "bounds={} renderer={} material='{}' shader='{}' subMeshes={}",
          kTextCheckDelays[it->round], it->asset, ToStdString(gameObject->get_name()),
          BoolText(gameObject->get_activeInHierarchy()), BoolText(text->get_enabled()),
          content ? ToStdString(content).size() : 0, info != nullptr ? info->characterCount : -1, scale.x, scale.y, scale.z,
          IsAlive(font.unsafePtr()) ? ToStdString(font->get_name()) : std::string("<none>"),
          BoolText(IsAlive(font.unsafePtr()) && font->get_atlasPopulationMode().value__ == 1),
          BoolText(IsAlive(font.unsafePtr()) && IsAlive(font->get_sourceFontFile().unsafePtr())), vertices, uv2,
          bounds, IsAlive(renderer) ? (renderer->get_enabled() ? "on" : "off") : "none",
          IsAlive(material) ? ToStdString(material->get_name()) : std::string("<none>"),
          IsAlive(shader) ? ToStdString(shader->get_name()) : std::string("<none>"), subMeshes ? subMeshes.size() : 0);
    }
    it->round++;
    if (it->round >= static_cast<int>(sizeof(kTextCheckDelays) / sizeof(kTextCheckDelays[0]))) {
      it = _textChecks.erase(it);
    } else {
      it->due += kTextCheckDelays[it->round] - kTextCheckDelays[it->round - 1];
      ++it;
    }
  }
}

void Runtime::RunVisualCheck(std::string_view when) {
  bool const firstCheck = _visualChecksRun == 1;

  // What a renderer amounts to on screen: drawn at all, and by what.
  auto describe = [this](UnityEngine::Renderer* renderer) {
    auto gameObject = renderer->get_gameObject();
    auto* material = renderer->get_sharedMaterial().unsafePtr();
    auto* shader = IsAlive(material) ? material->get_shader().unsafePtr() : nullptr;
    std::string text = fmt::format(
        "'{}' enabled={} active={} layer={} material='{}' shader='{}' supported={} queue={}",
        IsAlive(gameObject.unsafePtr()) ? ToStdString(gameObject->get_name()) : std::string("?"),
        BoolText(renderer->get_enabled()),
        BoolText(IsAlive(gameObject.unsafePtr()) && gameObject->get_activeInHierarchy()),
        IsAlive(gameObject.unsafePtr()) ? gameObject->get_layer() : -1,
        IsAlive(material) ? ToStdString(material->get_name()) : std::string("<none>"),
        IsAlive(shader) ? ToStdString(shader->get_name()) : std::string("<none>"),
        BoolText(IsAlive(shader) && shader->get_isSupported()), IsAlive(material) ? material->get_renderQueue() : -1);
    // The game dissolves a note through _Cutout in its property block (Noodle's
    // dissolve animates it), and tints it through _Color: a body at cutout 1 or
    // alpha 0 is there, enabled, and invisible. An unset block reads 0 for both.
    auto* block = UnityEngine::MaterialPropertyBlock::New_ctor();
    if (block != nullptr) {
      renderer->GetPropertyBlock(block);
      auto const color = block->GetColor(ColorPropertyId());
      text += fmt::format(" block: _Cutout={:.2f} _Color=({:.2f},{:.2f},{:.2f},{:.2f})",
                          block->GetFloat(CutoutPropertyId()), color.r, color.g, color.b, color.a);
    }
    return text;
  };
  auto drawable = [this](UnityEngine::Renderer* renderer) {
    if (!IsAlive(renderer) || !renderer->get_enabled()) return false;
    auto gameObject = renderer->get_gameObject();
    if (!IsAlive(gameObject.unsafePtr()) || !gameObject->get_activeInHierarchy()) return false;
    auto* material = renderer->get_sharedMaterial().unsafePtr();
    auto* shader = IsAlive(material) ? material->get_shader().unsafePtr() : nullptr;
    return IsAlive(shader) && shader->get_isSupported();
  };

  // Notes.
  int live = 0;
  int replaced = 0;
  int replacedNothingDrawable = 0;
  int defaultWithoutBody = 0;
  int noteLayer = -1;
  GlobalNamespace::GameNoteController* sample = nullptr;
  auto notes = UnityEngine::Object::FindObjectsOfType<GlobalNamespace::GameNoteController*>();
  for (int i = 0; notes && i < notes.size(); i++) {
    auto* note = notes[i];
    if (!IsAlive(note)) continue;
    auto noteObject = note->get_gameObject();
    if (!IsAlive(noteObject.unsafePtr()) || !noteObject->get_activeInHierarchy()) continue;
    live++;
    if (sample == nullptr) sample = note;
    auto it = _noteReplacements.find(note);
    if (it != _noteReplacements.end()) {
      replaced++;
      bool any = false;
      for (auto* renderer : it->second.replacementRenderers) any = any || drawable(renderer);
      if (it->second.hideOriginal && !any) replacedNothingDrawable++;
      continue;
    }
    // The game's own note: its body is the renderer on "NoteCube"; the arrow
    // and glows are separate renderers under it.
    bool bodyFound = false;
    bool bodyDrawable = false;
    auto renderers = note->GetComponentsInChildren<UnityEngine::Renderer*>(true);
    for (int r = 0; renderers && r < renderers.size(); r++) {
      auto* renderer = renderers[r];
      if (!IsAlive(renderer)) continue;
      auto rendererObject = renderer->get_gameObject();
      if (!IsAlive(rendererObject.unsafePtr())) continue;
      if (ToStdString(rendererObject->get_name()) != "NoteCube") continue;
      bodyFound = true;
      bodyDrawable = drawable(renderer);
      if (noteLayer < 0) noteLayer = rendererObject->get_layer();
    }
    if (bodyFound && !bodyDrawable) defaultWithoutBody++;
  }
  PaperLogger.info("Vivify visual check at {}: {} live note(s), {} with a map prefab; {} game note(s) whose body "
                   "cannot draw; {} map-prefab note(s) with nothing that can draw",
                   when, live, replaced, defaultWithoutBody, replacedNothingDrawable);
  if (defaultWithoutBody > 0) {
    NoteVisualProblem(fmt::format("{} of {} live game note(s) had a body that cannot draw at {} (only the arrow "
                                  "shows); see the note dump in the session log",
                                  defaultWithoutBody, live, when));
  }
  if (replacedNothingDrawable > 0) {
    NoteVisualProblem(fmt::format("{} of {} map-prefab note(s) had nothing that can draw at {} (the note is "
                                  "invisible)",
                                  replacedNothingDrawable, replaced, when));
  }
  // One full dump per level, of the first live note: enough to see which part
  // is missing and why.
  if (firstCheck && IsAlive(sample)) {
    auto renderers = sample->GetComponentsInChildren<UnityEngine::Renderer*>(true);
    PaperLogger.info("Vivify visual check: a live note has {} renderer(s){}:", renderers ? renderers.size() : 0,
                     _noteReplacements.contains(sample) ? " (map prefab)" : "");
    for (int r = 0; renderers && r < renderers.size() && r < 16; r++) {
      if (IsAlive(renderers[r])) PaperLogger.info("Vivify visual check:   {}", describe(renderers[r]));
    }
  }

  // Lights and shadows the scene has now: a map's directional light and
  // realtime shadows change how lit shaders draw, and Quest builds are rarely
  // tested with them.
  auto lights = UnityEngine::Object::FindObjectsOfType<UnityEngine::Light*>();
  int enabledLights = 0;
  int shadowed = 0;
  std::string lightList;
  for (int i = 0; lights && i < lights.size(); i++) {
    auto* light = lights[i];
    if (!IsAlive(light) || !light->get_enabled()) continue;
    enabledLights++;
    int const shadows = static_cast<int>(light->get_shadows().value__);
    if (shadows != 0) shadowed++;
    if (enabledLights <= 6) {
      auto lightObject = light->get_gameObject();
      lightList += fmt::format("{}'{}' type={} intensity={:.2f} shadows={}", lightList.empty() ? "" : "; ",
                               IsAlive(lightObject.unsafePtr()) ? ToStdString(lightObject->get_name()) : "?",
                               static_cast<int>(light->get_type().value__), light->get_intensity(), shadows);
    }
  }
  if (firstCheck) {
    auto sun = UnityEngine::RenderSettings::get_sun();
    PaperLogger.info("Vivify visual check: {} light(s) on, {} with shadows{}{}; ambient mode={} intensity={:.2f} "
                     "sun='{}'",
                     enabledLights, shadowed, lightList.empty() ? "" : ": ", lightList,
                     static_cast<int>(UnityEngine::RenderSettings::get_ambientMode().value__),
                     UnityEngine::RenderSettings::get_ambientIntensity(),
                     IsAlive(sun.unsafePtr()) ? ToStdString(sun->get_name()) : std::string("<none>"));
  }

  // Where reflective shaders get their unity_SpecCube0 (0.15.37): the scene's
  // reflection probes and the environment reflection. Lifelike's and
  // Luminescent's water reflect from it, and on the headset the reflection
  // tilts with the head; nothing said which cubemap that is or who redraws it.
  if (firstCheck || _visualChecksRun == 2) {
    auto probes = UnityEngine::Object::FindObjectsOfType<UnityEngine::ReflectionProbe*>();
    std::string probeList;
    int probeCount = 0;
    for (int i = 0; probes && i < probes.size(); i++) {
      auto* probe = probes[i];
      if (!IsAlive(probe) || !probe->get_enabled()) continue;
      if (++probeCount > 6) continue;
      auto probeObject = probe->get_gameObject();
      auto const position = probe->get_transform()->get_position();
      auto texture = probe->get_texture();
      probeList += fmt::format("{}'{}' mode={} refresh={} resolution={} importance={} box={} pos=({:.1f},{:.1f},{:.1f}) "
                               "texture='{}'",
                               probeList.empty() ? "" : "; ",
                               IsAlive(probeObject.unsafePtr()) ? ToStdString(probeObject->get_name()) : "?",
                               static_cast<int>(probe->get_mode().value__),
                               static_cast<int>(probe->get_refreshMode().value__), probe->get_resolution(),
                               probe->get_importance(), BoolText(probe->get_boxProjection()), position.x, position.y,
                               position.z,
                               IsAlive(texture.unsafePtr()) ? ToStdString(texture->get_name()) : std::string("<none>"));
    }
    auto custom = UnityEngine::RenderSettings::get_customReflectionTexture();
    PaperLogger.info("Vivify visual check at {}: {} reflection probe(s) on{}{}; environment reflection mode={} "
                     "custom='{}' intensity={:.2f}",
                     when, probeCount, probeList.empty() ? "" : ": ", probeList,
                     static_cast<int>(UnityEngine::RenderSettings::get_defaultReflectionMode().value__),
                     IsAlive(custom.unsafePtr()) ? ToStdString(custom->get_name()) : std::string("<none>"),
                     UnityEngine::RenderSettings::get_reflectionIntensity());
  }

  // The game's own mirrors (0.15.1), for reflections that turn with the
  // head instead of staying in place (Lifelike's floor mirror, with
  // GraphicsTweaks' mirrors on). Every enabled camera that sees a mirror
  // renders a reflection for itself, so the cameras are listed too.
  auto mirrors = UnityEngine::Object::FindObjectsOfType<GlobalNamespace::Mirror*>();
  int mirrorsShown = 0;
  for (int i = 0; mirrors && i < mirrors.size() && mirrorsShown < 4; i++) {
    auto* mirror = mirrors[i];
    if (!IsAlive(mirror)) continue;
    auto gameObject = mirror->get_gameObject();
    if (!IsAlive(gameObject.unsafePtr()) || !gameObject->get_activeInHierarchy()) continue;
    mirrorsShown++;
    auto const scale = gameObject->get_transform()->get_lossyScale();
    auto* renderer = mirror->____renderer.unsafePtr();
    auto* mirrorRenderer = mirror->____mirrorRenderer.unsafePtr();
    PaperLogger.info("Vivify visual check: mirror '{}' isEnabled={} scale=({:.2f},{:.2f},{:.2f}) renderer: {} "
                     "mirrorRenderer: {}",
                     ToStdString(gameObject->get_name()), BoolText(mirror->get_isEnabled()), scale.x, scale.y,
                     scale.z, IsAlive(renderer) ? describe(renderer) : std::string("<none>"),
                     IsAlive(mirrorRenderer)
                         ? fmt::format("stereo={}x{} mono={}x{} aa={} disableDepth={}",
                                       mirrorRenderer->____stereoTextureWidth, mirrorRenderer->____stereoTextureHeight,
                                       mirrorRenderer->____monoTextureWidth, mirrorRenderer->____monoTextureHeight,
                                       mirrorRenderer->____maxAntiAliasing,
#ifdef VIVIFY_GAME_1_45_3
                                       std::string("n/a"))
#else
                                       BoolText(mirrorRenderer->____disableDepthTexture))
#endif
                         : std::string("<none>"));
  }
  if (mirrorsShown > 0) {
    auto cameras = UnityEngine::Object::FindObjectsOfType<UnityEngine::Camera*>();
    std::string cameraList;
    for (int i = 0; cameras && i < cameras.size(); i++) {
      auto* camera = cameras[i];
      if (!IsAlive(camera) || !camera->get_enabled()) continue;
      auto cameraObject = camera->get_gameObject();
      if (!IsAlive(cameraObject.unsafePtr()) || !cameraObject->get_activeInHierarchy()) continue;
      cameraList += fmt::format("{}'{}' stereo={} eye={} target={} mask=0x{:x}", cameraList.empty() ? "" : "; ",
                                ToStdString(cameraObject->get_name()), BoolText(camera->get_stereoEnabled()),
                                static_cast<int>(camera->get_stereoTargetEye().value__),
                                IsAlive(camera->get_targetTexture().unsafePtr()) ? "texture" : "screen",
                                static_cast<uint32_t>(camera->get_cullingMask()));
    }
    PaperLogger.info("Vivify visual check: cameras that may render the mirror: {}", cameraList);
  }

  // What the scene is drawing with (0.15.0), for "the map is just black":
  // whether the map's objects are there at all, how many wear a shader this
  // GPU cannot run, and which shaders draw the most of the scene.
  if (firstCheck || _visualChecksRun == 2) {
    int active = 0;
    int unsupported = 0;
    std::unordered_map<std::string, int> byShader;
    auto renderers = UnityEngine::Object::FindObjectsOfType<UnityEngine::Renderer*>();
    for (int i = 0; renderers && i < renderers.size(); i++) {
      auto* renderer = renderers[i];
      if (!IsAlive(renderer) || !renderer->get_enabled()) continue;
      auto rendererObject = renderer->get_gameObject();
      if (!IsAlive(rendererObject.unsafePtr()) || !rendererObject->get_activeInHierarchy()) continue;
      active++;
      auto* material = renderer->get_sharedMaterial().unsafePtr();
      auto* shader = IsAlive(material) ? material->get_shader().unsafePtr() : nullptr;
      if (!IsAlive(shader) || !shader->get_isSupported()) {
        unsupported++;
        continue;
      }
      byShader[ToStdString(shader->get_name())]++;
    }
    std::vector<std::pair<int, std::string>> top;
    for (auto const& [name, count] : byShader) top.emplace_back(count, name);
    std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::string list;
    for (size_t i = 0; i < top.size() && i < 8; i++) {
      list += fmt::format("{}{} x{}", list.empty() ? "" : ", ", top[i].second, top[i].first);
    }
    PaperLogger.info("Vivify visual check at {}: {} active renderer(s), {} with no runnable shader; most drawn: {}; "
                     "screen effects: {} before, {} after the main effect",
                     when, active, unsupported, list.empty() ? std::string("<none>") : list, _preEffects.size(),
                     _postEffects.size());
    if (active > 0 && unsupported * 2 > active) {
      NoteVisualProblem(fmt::format("{} of {} active renderer(s) had no shader this GPU can run at {}", unsupported,
                                    active, when));
    }
  }

  // What Unity's GrabPass actually left bound for each grab texture the map's
  // shaders read (0.15.29). A converted multiview shader samples it as a
  // two-eye array; water whose reflections or refraction come from a grab
  // (Luminescent's Silent/Clear Water) shows nothing if Unity binds a plain
  // 2D texture, or none, there.
  {
    std::set<std::string> grabs;
    {
      std::lock_guard lock(_depthScanMutex);
      grabs = _grabTextureNames;
    }
    for (auto const& name : grabs) {
      auto* texture = UnityEngine::Shader::GetGlobalTexture(StringW(name)).unsafePtr();
      if (!IsAlive(texture)) {
        PaperLogger.info("Vivify grab texture '{}' at {}: nothing bound", name, when);
        continue;
      }
      auto* rt = il2cpp_utils::try_cast<UnityEngine::RenderTexture>(texture).value_or(nullptr);
      if (IsAlive(rt)) {
        auto d = rt->get_descriptor();
        PaperLogger.info("Vivify grab texture '{}' at {}: render texture dimension={} layers={} {}x{} msaa={}",
                         name, when, static_cast<int>(d.get_dimension().value__), d.get_volumeDepth(),
                         d.get_width(), d.get_height(), d.get_msaaSamples());
      } else {
        PaperLogger.info("Vivify grab texture '{}' at {}: '{}' (not a render texture) {}x{}", name, when,
                         ToStdString(texture->get_name()), texture->get_width(), texture->get_height());
      }
    }
  }

  // Whether the main camera draws the notes' layer at all.
  UnityW<UnityEngine::Camera> camera(VivifyMainCamera());
  if (IsAlive(camera.unsafePtr())) {
    int const mask = camera->get_cullingMask();
    if (firstCheck) {
      PaperLogger.info("Vivify visual check: main camera cullingMask=0x{:08x} depthTextureMode={} clearFlags={}",
                       static_cast<uint32_t>(mask), static_cast<int>(camera->get_depthTextureMode().value__),
                       static_cast<int>(camera->get_clearFlags().value__));
    }
    if (noteLayer >= 0 && (mask & (1 << noteLayer)) == 0) {
      NoteVisualProblem(fmt::format("the main camera does not draw layer {}, the notes' bodies, at {}", noteLayer,
                                    when));
    }
  }
}

}  // namespace Vivify
