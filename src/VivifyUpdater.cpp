// Settings > Update Vivify, Install Nightly / Dev Build / Version (0.15.12).
//
// A QMOD is a zip holding the mod's library and its mod.json. Installing one
// here means: download it, check that every mod it depends on is installed at
// a version it accepts, and put its library in place of the one this game is
// running, so the next start loads it. The library is written beside the old
// one and renamed over it: the running game keeps its mapping of the old file,
// which is never written into. Mod managers keep their own record of what is
// installed and go on showing the old version number until they reinstall.
#include "VivifyUpdater.hpp"

#include "VivifyTypes.hpp"
#include "VivifyZip.hpp"
#include "main.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <thread>

namespace Vivify::Updater {
namespace {

constexpr char const* kRepo = "trail-b1az3r/vivify-quest";
constexpr char const* kModId = "vivify";
constexpr char const* kStagingDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/Update";

std::atomic<bool> gBusy = false;

struct Semver {
  int major = 0, minor = 0, patch = 0;
  bool ok = false;
};

Semver ParseSemver(std::string_view text) {
  Semver v;
  while (!text.empty() && !std::isdigit(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  int parts[3] = {0, 0, 0};
  int index = 0;
  bool any = false;
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      parts[index] = parts[index] * 10 + (c - '0');
      any = true;
    } else if (c == '.' && index < 2) {
      index++;
    } else {
      break;
    }
  }
  v.major = parts[0];
  v.minor = parts[1];
  v.patch = parts[2];
  v.ok = any;
  return v;
}

int Compare(Semver const& a, Semver const& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// The range forms QMODs use: ^x.y.z, ~x.y.z, >=x.y.z, x.y.z, *.
bool Satisfies(std::string const& version, std::string range) {
  range.erase(std::remove(range.begin(), range.end(), ' '), range.end());
  if (range.empty() || range == "*") return true;
  Semver const have = ParseSemver(version);
  Semver const want = ParseSemver(range);
  if (!have.ok || !want.ok) return true;  // unreadable: not a reason to refuse
  if (range.rfind(">=", 0) == 0) return Compare(have, want) >= 0;
  if (range.front() == '^') {
    if (Compare(have, want) < 0) return false;
    if (want.major > 0) return have.major == want.major;
    if (want.minor > 0) return have.major == 0 && have.minor == want.minor;
    return have.major == 0 && have.minor == 0 && have.patch == want.patch;
  }
  if (range.front() == '~') {
    return Compare(have, want) >= 0 && have.major == want.major && have.minor == want.minor;
  }
  return Compare(have, want) == 0;
}

void Report(std::function<void(std::string const&)> const& status, std::string const& text) {
  PaperLogger.info("Vivify updater: {}", text);
  if (!status) return;
  BSML::MainThreadScheduler::Schedule([status, text]() { status(text); });
}

// The URL exactly as written (0.15.16). web-utils percent-escapes a URL by
// default, '?' and '=' included, so "releases?per_page=100" reached GitHub as
// "releases%3Fper_page%3D100", which it refuses: the version list stayed at
// "Latest" and every install said the release could not be found.
WebUtils::URLOptions Request(std::string const& url, int timeoutSeconds) {
  WebUtils::URLOptions options(url, std::nullopt, timeoutSeconds);
  options.noEscape = true;
  return options;
}

// Which site, for messages: the full URL is long enough to push the settings
// list sideways, and says nothing a player can act on.
std::string Host(std::string const& url) {
  auto const start = url.find("://");
  auto const from = start == std::string::npos ? 0 : start + 3;
  return url.substr(from, url.find('/', from) - from);
}

std::optional<std::string> GetText(std::string const& url, std::string& error) {
  auto response = WebUtils::Get<WebUtils::StringResponse>(Request(url, 30));
  if (!response.IsSuccessful() || !response.responseData.has_value()) {
    PaperLogger.warn("Vivify updater: {} failed: http {}, curl {}", url, response.get_HttpCode(),
                     response.get_CurlStatus());
    error = Host(url) + " did not answer (http " + std::to_string(response.get_HttpCode()) + ")";
    return std::nullopt;
  }
  return *response.responseData;
}

std::optional<std::vector<uint8_t>> GetData(std::string const& url, std::string& error) {
  auto response = WebUtils::Get<WebUtils::DataResponse>(Request(url, 300));
  if (!response.IsSuccessful() || !response.responseData.has_value()) {
    error = "download failed (http " + std::to_string(response.get_HttpCode()) + ", curl " +
            std::to_string(response.get_CurlStatus()) + ")";
    return std::nullopt;
  }
  return std::vector<uint8_t>(response.responseData->begin(), response.responseData->end());
}

// The asset of a release that matches this build: the Lite QMOD for the Lite
// build, the full one otherwise.
std::string PickAsset(rapidjson::Value const& release) {
  if (!release.HasMember("assets") || !release["assets"].IsArray()) return {};
  std::string fallback;
  for (auto const& asset : release["assets"].GetArray()) {
    auto name = ReadStringView(asset, "name");
    auto url = ReadStringView(asset, "browser_download_url");
    if (!name || !url || name->size() < 5 || name->substr(name->size() - 5) != ".qmod") continue;
    bool const lite = name->find("Lite") != std::string_view::npos;
    if (lite == kLiteBuild) return std::string(*url);
    if (fallback.empty() && !lite) fallback = std::string(*url);
  }
  return kLiteBuild ? std::string() : fallback;
}

std::optional<rapidjson::Document> GetReleases(std::string& error) {
  auto text = GetText(std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=100", error);
  if (!text) return std::nullopt;
  rapidjson::Document doc;
  doc.Parse(text->c_str());
  if (doc.HasParseError() || !doc.IsArray()) {
    error = "GitHub sent an unreadable release list";
    return std::nullopt;
  }
  return doc;
}

// The QMOD to download for a channel, and the name to report.
bool Resolve(Channel channel, std::string const& tag, std::string& url, std::string& label, std::string& error) {
  if (channel == Channel::Dev) {
    // nightly.link serves the newest successful build.yml artifact of main
    // without a GitHub login; its page links the artifact by name.
    auto page = GetText(std::string("https://nightly.link/") + kRepo + "/workflows/build/main", error);
    if (!page) return false;
    std::smatch match;
    std::regex const link("https://nightly\\.link/[^\"' <>]+?\\.zip");
    if (!std::regex_search(*page, match, link)) {
      error = "no build of main is available";
      return false;
    }
    url = match.str();
    label = "the latest build of main";
    return true;
  }
  auto releases = GetReleases(error);
  if (!releases) return false;
  for (auto const& release : releases->GetArray()) {
    auto releaseTag = ReadStringView(release, "tag_name");
    if (!releaseTag) continue;
    bool const prerelease = release.HasMember("prerelease") && release["prerelease"].IsBool() &&
                            release["prerelease"].GetBool();
    bool wanted = false;
    if (channel == Channel::Nightly) wanted = *releaseTag == "nightly";
    if (channel == Channel::Stable) wanted = !prerelease && *releaseTag != "nightly";
    if (channel == Channel::Version) wanted = *releaseTag == tag;
    if (!wanted) continue;
    url = PickAsset(release);
    auto name = ReadStringView(release, "name");
    label = name ? std::string(*name) : std::string(*releaseTag);
    if (url.empty()) {
      error = "'" + label + "' has no " + std::string(kLiteBuild ? "Lite " : "") + "QMOD to install";
      return false;
    }
    return true;
  }
  error = channel == Channel::Nightly ? "there is no nightly release" : "that release was not found";
  return false;
}

bool InstallQmod(std::vector<uint8_t> const& qmod, std::string& version, std::string& error) {
  std::error_code ec;
  std::filesystem::path const staging(kStagingDir);
  std::filesystem::remove_all(staging, ec);
  if (!ExtractZip(std::span<uint8_t const>(qmod.data(), qmod.size()), staging, error)) {
    error = "the QMOD could not be unpacked: " + error;
    return false;
  }
  std::ifstream in(staging / "mod.json");
  std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  rapidjson::Document mod;
  mod.Parse(text.c_str());
  if (mod.HasParseError() || !mod.IsObject()) {
    error = "the QMOD has no readable mod.json";
    return false;
  }
  if (ReadStringView(mod, "id").value_or("") != kModId) {
    error = "the download is not a Vivify QMOD";
    return false;
  }
  version = std::string(ReadStringView(mod, "version").value_or("?"));

  // Every dependency that is installed as a mod must be at a version the new
  // build accepts; a library the loader does not list as a mod is not checked.
  if (mod.HasMember("dependencies") && mod["dependencies"].IsArray()) {
    for (auto const& dependency : mod["dependencies"].GetArray()) {
      auto id = ReadStringView(dependency, "id");
      auto range = ReadStringView(dependency, "version");
      if (!id || !range) continue;
      std::string const idText(*id);
      CModInfo info{idText.c_str(), nullptr, 0};
      auto const found = modloader_get_mod(&info, MatchType_IdOnly);
      if (found.handle == nullptr || found.info.version == nullptr) continue;
      if (!Satisfies(found.info.version, std::string(*range))) {
        error = "it needs " + idText + " " + std::string(*range) + ", and " + found.info.version +
                " is installed; update that mod first (or install this version with a mod manager)";
        return false;
      }
    }
  }

  // Where this library was loaded from: the new one takes its place.
  CModInfo self{kModId, nullptr, 0};
  auto const selfResult = modloader_get_mod(&self, MatchType_IdOnly);
  if (selfResult.path == nullptr) {
    error = "the game did not say where Vivify's library is";
    return false;
  }
  std::filesystem::path const target(selfResult.path);
  std::filesystem::path source;
  for (char const* list : {"lateModFiles", "modFiles"}) {
    if (!mod.HasMember(list) || !mod[list].IsArray()) continue;
    for (auto const& file : mod[list].GetArray()) {
      if (file.IsString() && std::filesystem::exists(staging / file.GetString(), ec)) {
        source = staging / file.GetString();
        break;
      }
    }
    if (!source.empty()) break;
  }
  if (source.empty()) {
    error = "the QMOD holds no library";
    return false;
  }
  std::filesystem::path const temporary = target.string() + ".new";
  std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    error = "could not write " + temporary.string() + ": " + ec.message();
    return false;
  }
  if (std::rename(temporary.c_str(), target.c_str()) != 0) {
    std::filesystem::remove(temporary, ec);
    error = "could not replace " + target.string();
    return false;
  }
  std::filesystem::remove_all(staging, ec);
  return true;
}

// A build.yml artifact zip holds the QMODs themselves.
std::optional<std::vector<uint8_t>> QmodFromArtifact(std::vector<uint8_t> const& zip, std::string& error) {
  std::error_code ec;
  std::filesystem::path const unpack = std::string(kStagingDir) + "-artifact";
  std::filesystem::remove_all(unpack, ec);
  if (!ExtractZip(std::span<uint8_t const>(zip.data(), zip.size()), unpack, error)) return std::nullopt;
  std::filesystem::path chosen;
  for (auto const& entry : std::filesystem::directory_iterator(unpack, ec)) {
    std::string const name = entry.path().filename().string();
    if (entry.path().extension() != ".qmod") continue;
    if ((name.find("Lite") != std::string::npos) == kLiteBuild) chosen = entry.path();
  }
  if (chosen.empty()) {
    error = "the build has no matching QMOD";
    return std::nullopt;
  }
  std::ifstream in(chosen, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::filesystem::remove_all(unpack, ec);
  return data;
}

}  // namespace

bool Busy() {
  return gBusy.load();
}

void Install(Channel channel, std::string const& tag, std::function<void(std::string const&)> status) {
  if (gBusy.exchange(true)) {
    Report(status, "An update is already being installed");
    return;
  }
  std::thread([channel, tag, status = std::move(status)]() {
    std::string error;
    std::string url;
    std::string label;
    std::string version;
    bool ok = false;
    Report(status, "Looking for the build...");
    if (Resolve(channel, tag, url, label, error)) {
      Report(status, "Downloading " + label + "...");
      auto data = GetData(url, error);
      if (data && channel == Channel::Dev) data = QmodFromArtifact(*data, error);
      if (data) {
        Report(status, "Installing " + label + "...");
        ok = InstallQmod(*data, version, error);
      }
    }
    if (ok) {
      Report(status, "Installed Vivify " + version + " (" + label + "). Restart Beat Saber to use it.");
    } else {
      Report(status, "Not installed: " + error);
    }
    gBusy = false;
  }).detach();
}

void ListVersions(std::function<void(std::vector<std::string> const&, std::string const&)> done) {
  std::thread([done = std::move(done)]() {
    std::string error;
    std::vector<std::string> tags;
    if (auto releases = GetReleases(error)) {
      Semver const minimum = ParseSemver("0.13.5");
      for (auto const& release : releases->GetArray()) {
        auto tag = ReadStringView(release, "tag_name");
        if (!tag || *tag == "nightly") continue;
        Semver const v = ParseSemver(*tag);
        // 9.12 was a mistyped tag of the 0.9 series.
        if (!v.ok || v.major > 0 || Compare(v, minimum) < 0) continue;
        tags.emplace_back(*tag);
      }
      std::sort(tags.begin(), tags.end(),
                [](std::string const& a, std::string const& b) { return Compare(ParseSemver(a), ParseSemver(b)) > 0; });
    }
    BSML::MainThreadScheduler::Schedule([done, tags, error]() { done(tags, error); });
  }).detach();
}

}  // namespace Vivify::Updater
