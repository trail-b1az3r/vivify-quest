// Vivify Quest Converter: the converter as a desktop window.
//
// The same library as vivify-convert (../include/vivify_convert.h), with a
// window around it: drop PC bundles or map folders on it (or browse for
// them), pick the options, and convert them all, one after another, on a
// background thread so the window stays responsive. Each bundle's result,
// and what its shaders were compiled for, is shown beside the queue.
//
// Dear ImGui on GLFW and OpenGL 3; file dialogs from portable-file-dialogs
// (native on Windows and macOS, zenity or kdialog on Linux; without either,
// drag and drop and typed paths still work).
#include "vivify_convert.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <portable-file-dialogs.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char const* kQuestBundleName = "bundleAndroid2021.vivify";
constexpr char const* kPcBundleNames[] = {"bundleWindows2021.vivify", "bundleWindows2019.vivify"};

// UTF-8 path text <-> std::filesystem::path on every platform.
fs::path PathOf(std::string const& utf8) {
  return fs::path(std::u8string(reinterpret_cast<char8_t const*>(utf8.data()), utf8.size()));
}
std::string TextOf(fs::path const& path) {
  auto const text = path.u8string();
  return std::string(reinterpret_cast<char const*>(text.data()), text.size());
}

enum class JobState { Waiting, Running, Done, Failed };

struct Job {
  std::string source, dest;
  JobState state = JobState::Waiting;
  int32_t status = -1;
  VivifyConvertResult result{};
  std::string message;   // the converter's summary or failure reason
  std::string describe;  // what the source bundle's shaders were compiled for
};

struct App {
  std::vector<std::shared_ptr<Job>> jobs;
  std::mutex jobsMutex;  // guards each job's state, status, result, message
  VivifyConvertOptions options{};
  std::thread worker;
  std::atomic<bool> running{false};
  std::atomic<bool> stopRequested{false};
  int selected = -1;
  std::string notice;  // a one-line note for the user (rejected drop, ...)
  char typedPath[1024] = {};
};

// A dropped or browsed path: a PC bundle itself, or a map folder holding one.
// Returns the bundle path, or "" with a reason in `why`.
std::string ResolveBundle(std::string const& input, std::string& why) {
  std::error_code ec;
  fs::path const path = PathOf(input);
  if (fs::is_directory(path, ec)) {
    for (auto const* name : kPcBundleNames) {
      fs::path const candidate = path / name;
      if (fs::is_regular_file(candidate, ec)) return TextOf(candidate);
    }
    why = "no bundleWindows2021.vivify or bundleWindows2019.vivify in " + TextOf(path.filename());
    return {};
  }
  if (!fs::is_regular_file(path, ec)) {
    why = "not found: " + input;
    return {};
  }
  if (!vivify_convert_is_bundle(input.c_str())) {
    why = TextOf(path.filename()) + " is not a Unity asset bundle";
    return {};
  }
  return input;
}

void AddSource(App& app, std::string const& input) {
  std::string why;
  std::string const source = ResolveBundle(input, why);
  if (source.empty()) {
    app.notice = why;
    return;
  }
  std::lock_guard lock(app.jobsMutex);
  for (auto const& job : app.jobs) {
    if (job->source == source) {
      app.notice = "already in the list: " + source;
      return;
    }
  }
  auto job = std::make_shared<Job>();
  job->source = source;
  job->dest = TextOf(PathOf(source).parent_path() / kQuestBundleName);
  // What the bundle was built for, read up front: it is quick, and it tells
  // a mapper at once whether the file is the right one.
  size_t const size = vivify_convert_describe_bundle(source.c_str(), nullptr, 0);
  job->describe.assign(size + 1, '\0');
  vivify_convert_describe_bundle(source.c_str(), job->describe.data(), job->describe.size());
  job->describe.resize(size);
  app.jobs.push_back(std::move(job));
  app.selected = static_cast<int>(app.jobs.size()) - 1;
  app.notice.clear();
}

App* g_app = nullptr;
void OnDrop(GLFWwindow*, int count, char const** paths) {
  for (int i = 0; i < count; i++) AddSource(*g_app, paths[i]);
}

std::string Summary(Job const& job) {
  auto const& r = job.result;
  char text[256];
  std::snprintf(text, sizeof text, "%d of %d shaders translated, %d refused; %.1f MB",
                r.shaders_translated, r.shaders_seen, r.shaders_refused,
                static_cast<double>(r.output_bytes) / (1024.0 * 1024.0));
  return text;
}

void ConvertAll(App& app) {
  if (app.running) return;
  if (app.worker.joinable()) app.worker.join();
  std::vector<std::shared_ptr<Job>> queue;
  {
    std::lock_guard lock(app.jobsMutex);
    for (auto const& job : app.jobs) {
      if (job->state == JobState::Running) continue;
      job->state = JobState::Waiting;
      queue.push_back(job);
    }
  }
  if (queue.empty()) return;
  VivifyConvertOptions const options = app.options;
  app.running = true;
  app.stopRequested = false;
  app.worker = std::thread([&app, queue, options] {
    for (auto const& job : queue) {
      if (app.stopRequested) break;
      std::string source, dest;
      {
        std::lock_guard lock(app.jobsMutex);
        job->state = JobState::Running;
        source = job->source;
        dest = job->dest;
      }
      VivifyConvertResult result{};
      result.struct_size = sizeof result;
      std::vector<char> message(1 << 16);
      int32_t const status =
          vivify_convert_bundle(source.c_str(), dest.c_str(), &options, &result, message.data(), message.size());
      std::lock_guard lock(app.jobsMutex);
      job->status = status;
      job->result = result;
      job->message = message.data();
      job->state = status == VIVIFY_CONVERT_SUCCESS || status == VIVIFY_CONVERT_ALREADY_ANDROID ? JobState::Done
                                                                                               : JobState::Failed;
    }
    app.running = false;
  });
}

void HelpMarker(char const* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

bool Flag(char const* label, int32_t& value, char const* help, bool inverted = false) {
  bool on = inverted ? value == 0 : value != 0;
  bool const changed = ImGui::Checkbox(label, &on);
  if (changed) value = (on != inverted) ? 1 : 0;
  HelpMarker(help);
  return changed;
}

void OptionsPanel(App& app) {
  auto& o = app.options;
  ImGui::BeginDisabled(app.running);
  Flag("Translate shaders", o.translate_shaders,
       "Translate every shader's Direct3D bytecode to GLSL ES 3 with multiview. Off: only retarget the bundle to "
       "Android and leave the shaders as they are.");
  ImGui::BeginDisabled(o.translate_shaders == 0);
  Flag("Copy geometry into the right eye", o.geometry_right_eye_copy,
       "The Quest's driver runs geometry shaders for the left eye only; this draws a second copy of their output "
       "into the right eye.");
  Flag("Draw untranslatable geometry flat", o.drop_untranslatable_geometry,
       "Geometry-shader variants whose geometry stage cannot be translated are drawn without that stage, instead "
       "of being left on Direct3D.");
  Flag("Draw all geometry flat", o.drop_geometry_stages,
       "Every geometry-shader variant is drawn without its geometry stage.");
  Flag("Keep variants a Quest never uses", o.skip_quest_unused_variants,
       "Also translate GPU-instancing and unsplit single-pass-stereo variants. Only needed for a bundle whose "
       "materials keep GPU instancing on; makes the output larger.",
       true);
  Flag("Single-view programs only", o.single_view_only,
       "For cameras that draw into plain textures (screen textures, replay rendering), not for a map's bundle.");
  Flag("Split single-view / multiview", o.separate_stereo_variants,
       "Selected by the STEREO_MULTIVIEW_ON keyword. Only for a game whose own shaders use that keyword; Beat "
       "Saber 1.40 does not.");
  Flag("...for Unity 2019 bundles too", o.split_unity_2019, "As above, for bundles built with Unity 2019.");
  ImGui::EndDisabled();
  Flag("Rename archive files", o.rename_archive_files,
       "Give the archive's files new names, so the result can be loaded beside another conversion of the same "
       "bundle.");
  if (ImGui::Button("Mod defaults")) vivify_convert_default_options(&o);
  HelpMarker("The options the Vivify Quest mod converts a map's PC bundle with on the headset.");
  ImGui::EndDisabled();
}

char const* StateText(Job const& job) {
  switch (job.state) {
    case JobState::Waiting: return "Waiting";
    case JobState::Running: return "Converting...";
    case JobState::Done: return job.status == VIVIFY_CONVERT_ALREADY_ANDROID ? "Already Android" : "Done";
    case JobState::Failed: return vivify_convert_status_text(job.status);
  }
  return "";
}

ImVec4 StateColour(JobState state) {
  switch (state) {
    case JobState::Done: return {0.45f, 0.85f, 0.45f, 1.0f};
    case JobState::Failed: return {0.95f, 0.45f, 0.40f, 1.0f};
    case JobState::Running: return {0.95f, 0.80f, 0.35f, 1.0f};
    default: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
  }
}

void QueuePanel(App& app) {
  bool const dialogs = pfd::settings::available();
  ImGui::BeginDisabled(app.running);
  ImGui::BeginDisabled(!dialogs);
  if (ImGui::Button("Add bundles...")) {
    auto const files = pfd::open_file("PC Vivify bundles", "", {"Vivify bundles", "*.vivify", "All files", "*"},
                                      pfd::opt::multiselect)
                           .result();
    for (auto const& file : files) AddSource(app, file);
  }
  ImGui::SameLine();
  if (ImGui::Button("Add map folder...")) {
    auto const folder = pfd::select_folder("A map folder with bundleWindows2021.vivify").result();
    if (!folder.empty()) AddSource(app, folder);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 4.0f);
  bool const entered = ImGui::InputTextWithHint("##path", "or type a bundle or map folder path", app.typedPath,
                                                sizeof app.typedPath, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if ((ImGui::Button("Add") || entered) && app.typedPath[0]) {
    AddSource(app, app.typedPath);
    if (app.notice.empty()) app.typedPath[0] = '\0';
  }
  ImGui::EndDisabled();
  if (!dialogs) ImGui::TextDisabled("No file dialog here (install zenity or kdialog); drop files or type a path.");

  std::lock_guard lock(app.jobsMutex);
  if (app.jobs.empty()) {
    ImVec2 const size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 4.0f);
    ImGui::BeginChild("drop", size, ImGuiChildFlags_Borders);
    ImGui::SetCursorPosY(size.y * 0.5f - ImGui::GetTextLineHeight());
    char const* hint = "Drop PC bundles or map folders anywhere on this window";
    ImGui::SetCursorPosX((size.x - ImGui::CalcTextSize(hint).x) * 0.5f);
    ImGui::TextDisabled("%s", hint);
    ImGui::EndChild();
    return;
  }
  int remove = -1;
  ImGuiTableFlags const flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  float const height = std::min(ImGui::GetFrameHeightWithSpacing() * (app.jobs.size() + 1.5f),
                                ImGui::GetContentRegionAvail().y * 0.45f);
  if (ImGui::BeginTable("queue", 4, flags, ImVec2(0, height))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Map / bundle", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Writes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableHeadersRow();
    for (int i = 0; i < static_cast<int>(app.jobs.size()); i++) {
      auto& job = *app.jobs[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      fs::path const source = PathOf(job.source);
      std::string const label = TextOf(source.parent_path().filename()) + " / " + TextOf(source.filename());
      if (ImGui::Selectable(label.c_str(), app.selected == i, ImGuiSelectableFlags_SpanAllColumns |
                                                                   ImGuiSelectableFlags_AllowOverlap))
        app.selected = i;
      ImGui::SetItemTooltip("%s", job.source.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(TextOf(PathOf(job.dest).filename()).c_str());
      ImGui::SetItemTooltip("%s", job.dest.c_str());
      ImGui::TableNextColumn();
      ImGui::TextColored(StateColour(job.state), "%s", StateText(job));
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(job.state == JobState::Running || app.running);
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::SetItemTooltip("Remove from the list");
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (remove >= 0) {
    app.jobs.erase(app.jobs.begin() + remove);
    if (app.selected >= static_cast<int>(app.jobs.size())) app.selected = static_cast<int>(app.jobs.size()) - 1;
  }
}

void DetailsPanel(App& app) {
  std::lock_guard lock(app.jobsMutex);
  if (app.selected < 0 || app.selected >= static_cast<int>(app.jobs.size())) {
    ImGui::TextDisabled("Select a bundle to see its details.");
    return;
  }
  auto& job = *app.jobs[app.selected];
  ImGui::TextUnformatted("Bundle:");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", job.source.c_str());
  ImGui::TextUnformatted("Writes:");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", job.dest.c_str());
  ImGui::BeginDisabled(app.running || !pfd::settings::available());
  if (ImGui::Button("Save as...")) {
    auto const chosen =
        pfd::save_file("Write the Quest bundle to", job.dest, {"Vivify bundles", "*.vivify"}, pfd::opt::none)
            .result();
    if (!chosen.empty()) {
      job.dest = chosen;
      job.state = JobState::Waiting;
    }
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Choose another file to write. The mod loads %s from the map's folder.", kQuestBundleName);
  ImGui::SameLine();
  if (ImGui::Button("Copy details")) {
    std::string text = job.source + "\n-> " + job.dest + "\n\n";
    if (!job.message.empty()) text += std::string(StateText(job)) + ": " + job.message + "\n\n";
    text += job.describe;
    ImGui::SetClipboardText(text.c_str());
  }
  ImGui::Separator();
  if (job.state == JobState::Done || job.state == JobState::Failed) {
    ImGui::TextColored(StateColour(job.state), "%s", StateText(job));
    if (job.state == JobState::Done && job.status == VIVIFY_CONVERT_SUCCESS) {
      ImGui::SameLine();
      ImGui::TextUnformatted(Summary(job).c_str());
    }
  }
  ImGui::BeginChild("text", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (!job.message.empty()) {
    ImGui::TextWrapped("%s", job.message.c_str());
    ImGui::Separator();
  }
  ImGui::TextDisabled("What this bundle was built for:");
  ImGui::TextWrapped("%s", job.describe.c_str());
  ImGui::EndChild();
}

void Frame(App& app) {
  ImGuiViewport const* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("Vivify Quest Converter", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted("Turns a PC Vivify bundle into bundleAndroid2021.vivify, which Vivify Quest loads first.");
  ImGui::TextDisabled("Converter revision %d: the same converter the mod runs on the headset.",
                      vivify_convert_revision());
  ImGui::Spacing();
  QueuePanel(app);
  if (!app.notice.empty()) ImGui::TextColored(StateColour(JobState::Failed), "%s", app.notice.c_str());

  bool hasJobs;
  int finished = 0;
  {
    std::lock_guard lock(app.jobsMutex);
    hasJobs = !app.jobs.empty();
    for (auto const& job : app.jobs) finished += job->state == JobState::Done || job->state == JobState::Failed;
  }
  ImGui::Spacing();
  if (app.running) {
    if (ImGui::Button("Stop after this bundle")) app.stopRequested = true;
    ImGui::SameLine();
    static char const spinner[] = "|/-\\";
    ImGui::Text("Converting %c", spinner[static_cast<int>(ImGui::GetTime() * 8.0) % 4]);
  } else {
    ImGui::BeginDisabled(!hasJobs);
    if (ImGui::Button(finished ? "Convert all again" : "Convert all")) ConvertAll(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!finished);
    if (ImGui::Button("Clear finished")) {
      std::lock_guard lock(app.jobsMutex);
      std::erase_if(app.jobs, [](auto const& job) { return job->state == JobState::Done; });
      app.selected = std::min(app.selected, static_cast<int>(app.jobs.size()) - 1);
    }
    ImGui::EndDisabled();
  }
  ImGui::Spacing();

  float const optionsWidth = std::max(ImGui::GetFontSize() * 22.0f, ImGui::GetContentRegionAvail().x * 0.34f);
  ImGui::BeginChild("options", ImVec2(optionsWidth, 0), ImGuiChildFlags_Borders);
  ImGui::SeparatorText("Options");
  OptionsPanel(app);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::SeparatorText("Details");
  DetailsPanel(app);
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
  glfwSetErrorCallback([](int code, char const* text) { std::fprintf(stderr, "GLFW error %d: %s\n", code, text); });
  if (!glfwInit()) return 1;
#if defined(__APPLE__)
  char const* glsl = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  char const* glsl = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  GLFWwindow* window = glfwCreateWindow(1100, 720, "Vivify Quest Converter", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  float scale = 1.0f;
  glfwGetWindowContentScale(window, &scale, nullptr);
  scale = std::max(1.0f, scale);
  ImFontConfig font;
  font.SizePixels = std::round(13.0f * scale * 1.15f);
  io.Fonts->AddFontDefault(&font);
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FrameRounding = 3.0f * scale;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl);

  App app;
  vivify_convert_default_options(&app.options);
  g_app = &app;
  glfwSetDropCallback(window, OnDrop);
  for (int i = 1; i < argc; i++) AddSource(app, argv[i]);

  while (!glfwWindowShouldClose(window)) {
    // Idle without spinning; wake now and then while converting, for the
    // spinner and the status column.
    if (app.running) glfwWaitEventsTimeout(0.1);
    else glfwWaitEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    Frame(app);
    ImGui::Render();
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }
  // A conversion cannot be interrupted midway; let the current one finish.
  app.stopRequested = true;
  if (app.worker.joinable()) app.worker.join();

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
