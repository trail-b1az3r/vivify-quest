#include "VivifyLogUpload.hpp"

#include "VivifyTypes.hpp"
#include "main.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "VivifyCompat.hpp"

namespace Vivify::LogUpload {
namespace {

// The relay accepts 4.5 MiB of text per request. Keep requests smaller so
// JSON escaping cannot push the HTTP body over the relay's body-size limit.
constexpr std::uintmax_t kMaxFileBytes = 1536 * 1024;
constexpr std::uintmax_t kAutoUploadBytes = 4 * 1024 * 1024;
constexpr std::uintmax_t kManualUploadBytes = 2 * kAutoUploadBytes;
// 1.5 MB per request (0.15.33, was 3 MB): a smaller request costs the relay
// less CPU per call, and the two uploads that failed with Cloudflare 503s
// were the ones that carried a large global-log part.
constexpr std::uintmax_t kRequestTextBytes = 3 * 512 * 1024;
constexpr size_t kMaxFilesPerRequest = 12;

struct UploadFile {
  std::string name;
  std::string sourceName;
  std::uintmax_t from = 0;
  std::string text;
};

struct UploadConfig {
  std::string directory;
  std::string relayUrl;
  std::string version;
  bool lite = false;
};

UploadConfig gConfig;
std::mutex gConfigMutex;
std::atomic<bool> gConfigured{false};
std::atomic<bool> gUploadRunning{false};
std::atomic<bool> gAutoPending{false};
std::atomic<bool> gManualPending{false};

std::map<std::string, std::uintmax_t> ReadState(std::filesystem::path const& path) {
  std::map<std::string, std::uintmax_t> state;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    auto const tab = line.find('\t');
    if (tab == std::string::npos) continue;
    try {
      state[line.substr(0, tab)] = std::stoull(line.substr(tab + 1));
    } catch (...) {
    }
  }
  return state;
}

void WriteState(std::filesystem::path const& path, std::map<std::string, std::uintmax_t> const& state) {
  std::ofstream out(path, std::ios::trunc);
  for (auto const& [name, offset] : state) out << name << '\t' << offset << '\n';
}

// Only Vivify-owned diagnostic files are uploaded. The session file is
// included alongside the persistent global history and per-level reports.
bool IsGlobalLogPart(std::string const& name) {
  constexpr std::string_view kStem = "vivify_global";
  if (name.rfind(kStem, 0) != 0 || name.size() < kStem.size() + 4 || !name.ends_with(".txt")) return false;
  std::string_view const middle(name.data() + kStem.size(), name.size() - kStem.size() - 4);
  if (middle.empty()) return true;
  if (middle.size() < 3 || middle.size() > 5 || middle.substr(0, 2) != "-p") return false;
  return std::all_of(middle.begin() + 2, middle.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::filesystem::path> Candidates(std::filesystem::path const& directory) {
  std::vector<std::filesystem::path> files;
  for (char const* name : {"VivifyCrash.txt", "VivifyReport.txt", "RecoveryMaps.txt", "PlayingLevel.txt",
                           "VivifySession.txt"}) {
    files.push_back(directory / name);
  }
  std::vector<std::filesystem::path> parts;
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator(directory, ec)) {
    std::string const name = entry.path().filename().string();
    // Only the global log's own parts, "vivify_global.txt" and
    // "vivify_global-p<N>.txt": a copy such as "vivify_global (1).txt" made
    // the relay refuse the whole upload (http 400, bad file name).
    if (IsGlobalLogPart(name)) parts.push_back(entry.path());
  }
  std::sort(parts.begin(), parts.end(), [](auto const& a, auto const& b) {
    std::error_code e1, e2;
    return std::filesystem::last_write_time(a, e1) > std::filesystem::last_write_time(b, e2);
  });
  files.insert(files.end(), parts.begin(), parts.end());
  return files;
}

std::string RandomId() {
  std::random_device device;
  std::mt19937_64 random(device());
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(random()));
  return buffer;
}

std::string PartName(std::string const& name, std::uintmax_t offset, bool split) {
  if (!split) return name;
  auto const dot = name.rfind(".txt");
  std::string const stem = dot == std::string::npos ? name : name.substr(0, dot);
  std::uintmax_t const part = offset / kMaxFileBytes + 1;
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "-part%03llu", static_cast<unsigned long long>(part));
  return stem + suffix + ".txt";
}

// Cloudflare Workers are serverless: they cannot be restarted on demand.
// A fresh uncached health request wakes/validates the relay before a manual
// upload, and bounded retries give transient cold-start/network failures time
// to clear without risking duplicate log commits.
// The relay's API address for `path`. The relay's root serves its dashboard
// (static files); only /api/* reaches the worker, so a POST to the bare
// address the build carries was refused with 405 and no log ever arrived.
std::string RelayEndpoint(UploadConfig const& config, char const* path) {
  std::string url = config.relayUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // An address that already names an API route is used as it is.
  if (url.find("/api/") != std::string::npos) url = url.substr(0, url.find("/api/"));
  return url + path;
}

bool PrepareRelay(UploadConfig const& config) {
  std::string const url = RelayEndpoint(config, "/api/health");
  for (int attempt = 0; attempt < 3; ++attempt) {
    WebUtils::URLOptions options(url, std::nullopt, 15);
    options.noEscape = true;
    auto response = WebUtils::Get<WebUtils::StringResponse>(options);
    if (response.IsSuccessful()) {
      // A relay without its GitHub token can still show the logs (public
      // reads) but cannot save any: say so instead of uploading into a 500.
      if (response.responseData && response.responseData->find("\"writeReady\":false") != std::string::npos) {
        PaperLogger.warn("Vivify Send Logs: the relay is up but has no GitHub token, so it cannot save logs; "
                         "its owner has to add the GITHUB_TOKEN secret (tools/log-relay/README.md)");
        return false;
      }
      PaperLogger.info("Vivify Send Logs: relay health check succeeded");
      return true;
    }
    PaperLogger.warn("Vivify Send Logs: relay health check failed (http {}, curl {}), attempt {}/3",
                     response.get_HttpCode(), response.get_CurlStatus(), attempt + 1);
    if (attempt < 2) std::this_thread::sleep_for(std::chrono::seconds(attempt + 1));
  }
  PaperLogger.warn("Vivify Send Logs: relay did not become healthy; manual upload cancelled without advancing log offsets");
  return false;
}

// ", Cloudflare error 1102: Worker exceeded resource limits" from a Cloudflare
// HTML error page, or "" for anything else.
std::string CloudflareError(std::string const& page) {
  if (page.find("<!DOCTYPE") == std::string::npos && page.find("<html") == std::string::npos) return {};
  auto between = [&](std::string const& open, std::string const& close) {
    size_t const start = page.find(open);
    if (start == std::string::npos) return std::string();
    size_t const from = start + open.size();
    size_t const end = page.find(close, from);
    if (end == std::string::npos) return std::string();
    std::string text = page.substr(from, std::min<size_t>(end - from, 160));
    for (auto& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
  };
  std::string code;
  if (size_t const at = page.find("Error code"); at != std::string::npos) {
    for (size_t i = at + 10; i < page.size() && i < at + 40; ++i) {
      if (std::isdigit(static_cast<unsigned char>(page[i]))) code += page[i];
      else if (!code.empty()) break;
    }
  }
  std::string const title = between("<title>", "</title>");
  if (code.empty() && title.empty()) return ", an HTML error page";
  return ", Cloudflare" + (code.empty() ? std::string() : " error " + code) + (title.empty() ? "" : ": " + title);
}

bool PostBatch(UploadConfig const& config, std::vector<UploadFile> const& batch) {
  if (batch.empty()) return true;
  rapidjson::Document body(rapidjson::kObjectType);
  auto& allocator = body.GetAllocator();
  body.AddMember("version", rapidjson::Value(config.version.c_str(), allocator), allocator);
  body.AddMember("lite", config.lite, allocator);
  std::string const uploadId = RandomId();
  body.AddMember("upload", rapidjson::Value(uploadId.c_str(), allocator), allocator);
  rapidjson::Value files(rapidjson::kArrayType);
  std::uintmax_t total = 0;
  for (auto const& entry : batch) {
    rapidjson::Value file(rapidjson::kObjectType);
    file.AddMember("name", rapidjson::Value(entry.name.c_str(), allocator), allocator);
    file.AddMember("from", static_cast<uint64_t>(entry.from), allocator);
    file.AddMember("text", rapidjson::Value(entry.text.c_str(), static_cast<rapidjson::SizeType>(entry.text.size()), allocator),
                   allocator);
    files.PushBack(file, allocator);
    total += entry.text.size();
  }
  body.AddMember("files", files, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  body.Accept(writer);
  std::span<uint8_t const> const payload(reinterpret_cast<uint8_t const*>(buffer.GetString()), buffer.GetSize());

  WebUtils::URLOptions options(RelayEndpoint(config, "/api/upload"), WebUtils::URLOptions::QueryMap{},
                               WebUtils::URLOptions::HeaderMap{{"Content-Type", "application/json"}}, false, "",
                               std::nullopt, 90);
  options.noEscape = true;
  // Retries (0.15.33): a busy or briefly failing relay answers 5xx (Cloudflare
  // serves its own HTML error page, e.g. when the Worker runs out of CPU on a
  // large request), and a dropped connection answers no HTTP status at all.
  // Those are tried again after a pause; a refusal (4xx) is final. The log
  // offsets only move after a success, so nothing is lost either way.
  constexpr int kAttempts = 4;
  constexpr int kPauseSeconds[kAttempts] = {0, 4, 12, 30};
  auto transient = [](long code) { return code == 0 || code == 408 || code == 429 || code >= 500; };
  auto response = WebUtils::Post<WebUtils::StringResponse>(options, payload);
  for (int attempt = 1; attempt < kAttempts && !response.IsSuccessful() && transient(response.get_HttpCode()); ++attempt) {
    PaperLogger.warn("Vivify Send Logs: upload attempt {}/{} failed (http {}, curl {}{}); retrying in {} s", attempt,
                     kAttempts, response.get_HttpCode(), response.get_CurlStatus(),
                     response.responseData ? CloudflareError(*response.responseData) : std::string(),
                     kPauseSeconds[attempt]);
    std::this_thread::sleep_for(std::chrono::seconds(kPauseSeconds[attempt]));
    response = WebUtils::Post<WebUtils::StringResponse>(options, payload);
  }
  if (!response.IsSuccessful()) {
    // The relay answers a refusal with its reason ("bad file: <name>", "too
    // much text", ...); without it a 400 in the log says nothing. A Cloudflare
    // error page is reduced to its error code and title.
    std::string reason = response.responseData ? *response.responseData : std::string();
    if (std::string const cloudflare = CloudflareError(reason); !cloudflare.empty()) reason = cloudflare.substr(2);
    if (reason.size() > 300) reason.resize(300);
    PaperLogger.warn("Vivify Send Logs: upload failed (http {}, curl {}): {} [files: {}]",
                     response.get_HttpCode(), response.get_CurlStatus(), reason.empty() ? "no reply" : reason,
                     [&] {
                       std::string names;
                       for (auto const& entry : batch) names += (names.empty() ? "" : ", ") + entry.name;
                       return names;
                     }());
    return false;
  }
  PaperLogger.info("Vivify Send Logs: uploaded {} file part(s), {} KB ({})", batch.size(), total / 1024,
                   response.responseData ? *response.responseData : std::string("accepted"));
  return true;
}

void Run(UploadConfig config, bool manual) {
  // Only the menu's explicit Send Logs action performs relay recovery checks.
  // Automatic uploads stay lightweight and retain their existing behaviour.
  if (manual && !PrepareRelay(config)) return;
  std::filesystem::path const dir(config.directory);
  std::filesystem::path const statePath = dir / "LogUpload.state";
  auto state = ReadState(statePath);
  auto next = state;
  std::vector<UploadFile> pending;
  std::uintmax_t total = 0;
  std::map<std::string, std::uintmax_t> manualCompleted;
  std::uintmax_t const totalLimit = manual ? kManualUploadBytes : kAutoUploadBytes;

  for (auto const& path : Candidates(dir)) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) continue;
    std::uintmax_t const size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) continue;
    std::string const sourceName = path.filename().string();
    std::uintmax_t from = manual ? 0 : (state.count(sourceName) ? state[sourceName] : 0);
    if (from > size) from = 0;
    if (!manual && from == size) continue;

    // Automatic uploads keep their previous newest-tail behavior per file.
    // Manual uploads start at byte zero and split large files into numbered
    // pieces so every piece fits the relay's per-file limit.
    if (!manual && size - from > kMaxFileBytes) from = size - kMaxFileBytes;
    while (from < size && total < totalLimit) {
      std::uintmax_t const length = std::min<std::uintmax_t>(
          {kMaxFileBytes, size - from, totalLimit - total});
      if (length == 0) break;
      std::ifstream in(path, std::ios::binary);
      if (!in) break;
      in.seekg(static_cast<std::streamoff>(from));
      std::string text(static_cast<size_t>(length), '\0');
      in.read(text.data(), static_cast<std::streamsize>(length));
      text.resize(static_cast<size_t>(in.gcount()));
      if (text.empty()) break;

      bool const split = size > kMaxFileBytes;
      UploadFile entry;
      entry.name = PartName(sourceName, from, split);
      entry.sourceName = sourceName;
      entry.from = from;
      entry.text = std::move(text);
      total += entry.text.size();
      from += entry.text.size();
      pending.push_back(std::move(entry));
      if (!manual) break;
    }
    if (manual && from >= size) manualCompleted[sourceName] = size;
    if (total >= totalLimit) break;
  }

  if (pending.empty()) {
    PaperLogger.info("Vivify Send Logs: no {} logs to upload", manual ? "available" : "new");
    return;
  }

  // Multiple small requests let a manual upload carry twice as much as an
  // automatic upload without ever exceeding the relay's per-request limit.
  std::vector<UploadFile> batch;
  std::uintmax_t batchBytes = 0;
  for (auto const& entry : pending) {
    if (!batch.empty() && (batch.size() >= kMaxFilesPerRequest ||
                           batchBytes + entry.text.size() > kRequestTextBytes)) {
      if (!PostBatch(config, batch)) return;
      if (!manual) {
        for (auto const& sent : batch) next[sent.sourceName] = sent.from + sent.text.size();
        WriteState(statePath, next);
      }
      batch.clear();
      batchBytes = 0;
    }
    batchBytes += entry.text.size();
    batch.push_back(entry);
  }
  if (!PostBatch(config, batch)) return;
  if (manual) {
    for (auto const& [name, size] : manualCompleted) next[name] = size;
    // Manual uploads are intentionally full-file uploads; recording offsets
    // avoids immediately sending the same data again at the next map end.
    WriteState(statePath, next);
  } else {
    for (auto const& sent : batch) next[sent.sourceName] = sent.from + sent.text.size();
    WriteState(statePath, next);
  }
  if (total >= totalLimit) {
    PaperLogger.info("Vivify Send Logs: reached the {} MB upload limit; use the menu button for the larger manual upload",
                     totalLimit / (1024 * 1024));
  }
}

void BeginUpload(bool manual) {
  if (!gConfigured.load()) {
    PaperLogger.warn("Vivify Send Logs: uploader is not configured");
    return;
  }
  if (!GetSendLogs()) {
    PaperLogger.info("Vivify Send Logs: disabled in Vivify settings");
    return;
  }
  if (!manual && !GetVivifyDebugLogging()) return;
  if (gConfig.relayUrl.empty()) {
    PaperLogger.info("Vivify Send Logs: this build has no log relay address");
    return;
  }
  if (gUploadRunning.exchange(true)) {
    (manual ? gManualPending : gAutoPending).store(true);
    PaperLogger.info("Vivify Send Logs: queued behind the current upload");
    return;
  }
  UploadConfig config;
  {
    std::lock_guard lock(gConfigMutex);
    config = gConfig;
  }
  std::thread([config = std::move(config), manual]() {
    bool nextManual = manual;
    do {
      try {
        Run(config, nextManual);
      } catch (std::exception const& error) {
        PaperLogger.warn("Vivify Send Logs: failed: {}", error.what());
      } catch (...) {
        PaperLogger.warn("Vivify Send Logs: failed");
      }
      if (gManualPending.exchange(false)) nextManual = true;
      else if (gAutoPending.exchange(false)) nextManual = false;
      else break;
    } while (true);
    gUploadRunning.store(false);
    if (gManualPending.exchange(false)) BeginUpload(true);
    else if (gAutoPending.exchange(false)) BeginUpload(false);
  }).detach();
}

}  // namespace

void StartIfEnabled(std::string const& directory, std::string const& relayUrl, std::string const& version,
                    bool lite) {
  {
    std::lock_guard lock(gConfigMutex);
    gConfig = {directory, relayUrl, version, lite};
  }
  gConfigured.store(true);
  // Retry any data left behind by a previous failed upload.
  BeginUpload(false);
}

void UploadManual() {
  BeginUpload(true);
}

void UploadAfterLevelEnd() {
  BeginUpload(false);
}

}  // namespace Vivify::LogUpload
