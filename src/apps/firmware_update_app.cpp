#include "firmware_update_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <new>
#include <vector>

#include "../core/board_pins.h"
#include "../core/shared_spi_bus.h"
#include "../core/tls_ca_bundle.h"
#include "../ui/ui_runtime.h"
#include "firmware_build_info.h"

namespace {

constexpr const char *kFirmwareRepoSlug = ZX_FW_REPO;
constexpr const char *kUserAgent = "ZX-OS-FirmwareUpdate";
constexpr const char *kFirmwareDir = "/firmware";
constexpr const char *kLatestFirmwarePath = "/firmware/latest.bin";
constexpr const char *kLatestShaPath = "/firmware/latest.bin.sha256";
constexpr const char *kLatestTagPath = "/firmware/latest.tag";
constexpr size_t kTransferChunkBytes = 4096;
constexpr unsigned long kHttpTimeoutMs = 15000UL;
constexpr unsigned long kDownloadIdleTimeoutMs = 15000UL;
constexpr size_t kLatestReleaseDocBytes = 16384;
constexpr size_t kReleaseListDocBytes = 32768;
constexpr int kReleaseListPageSize = 4;

struct ReleaseInfo {
  String tag;
  String assetName;
  String downloadUrl;
  String sha256Url;
  uint32_t size = 0;
  bool prerelease = false;
};

using ProgressFn = std::function<void(uint32_t written, uint32_t total)>;
using BeginFn = std::function<bool(uint32_t contentLength, String *error)>;
using ChunkFn = std::function<bool(const uint8_t *data, size_t len, String *error)>;

bool gSdMountedForFirmware = false;

class OverlayScope {
 public:
  OverlayScope(UiRuntime *ui, const String &title, const String &message, int percent = -1)
      : ui_(ui) {
    if (ui_) {
      ui_->showProgressOverlay(title, message, percent);
      lastUpdateMs_ = millis();
    }
  }

  ~OverlayScope() {
    if (ui_) {
      ui_->hideProgressOverlay();
    }
  }

  void update(const String &title,
              const String &message,
              int percent = -1,
              bool force = false) {
    if (!ui_) {
      return;
    }
    const unsigned long now = millis();
    if (!force && now - lastUpdateMs_ < 120UL) {
      return;
    }
    lastUpdateMs_ = now;
    ui_->showProgressOverlay(title, message, percent);
  }

 private:
  UiRuntime *ui_ = nullptr;
  unsigned long lastUpdateMs_ = 0;
};

class Sha256 {
 public:
  Sha256() {
    mbedtls_sha256_init(&ctx_);
    mbedtls_sha256_starts(&ctx_, 0);
  }
  ~Sha256() { mbedtls_sha256_free(&ctx_); }
  Sha256(const Sha256 &) = delete;
  Sha256 &operator=(const Sha256 &) = delete;

  void update(const uint8_t *data, size_t len) { mbedtls_sha256_update(&ctx_, data, len); }

  String finishHex() {
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx_, digest);
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); ++i) {
      snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return String(hex);
  }

 private:
  mbedtls_sha256_context ctx_;
};

String formatBytes(uint64_t bytes) {
  static const char *kUnits[] = {"B", "KB", "MB", "GB"};

  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }

  char buf[32];
  if (unit == 0) {
    snprintf(buf,
             sizeof(buf),
             "%llu %s",
             static_cast<unsigned long long>(bytes),
             kUnits[unit]);
  } else {
    snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  }
  return String(buf);
}

String trimMiddle(const String &value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 6) {
    return value;
  }

  const size_t left = (maxLength - 3) / 2;
  const size_t right = maxLength - 3 - left;
  return value.substring(0, left) + "..." +
         value.substring(value.length() - right);
}

int percentOf(uint32_t written, uint32_t total) {
  if (total == 0) {
    return -1;
  }
  return static_cast<int>((static_cast<uint64_t>(written) * 100ULL) / total);
}

String progressText(const char *verb, uint32_t written, uint32_t total) {
  String text = String(verb) + " " + formatBytes(written);
  if (total > 0) {
    text += " / " + formatBytes(total);
  }
  return text;
}

// Accepts "<64 hex>" or sha256sum output ("<64 hex>  name"); returns lowercase hex or "".
String normalizeSha256(const String &raw) {
  String value = raw;
  value.trim();
  const int space = value.indexOf(' ');
  if (space > 0) {
    value = value.substring(0, space);
  }
  value.toLowerCase();
  if (value.length() != 64) {
    return "";
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) {
      return "";
    }
  }
  return value;
}

// ---------------------------------------------------------------------------
// Version handling
// ---------------------------------------------------------------------------

struct ParsedVersion {
  long part[3] = {0, 0, 0};
  bool hasSuffix = false;  // "-rc1", "-dirty", ... sorts before the plain release
};

bool parseVersion(const String &raw, ParsedVersion &out) {
  String value = raw;
  value.trim();
  if (value.startsWith("v") || value.startsWith("V")) {
    value = value.substring(1);
  }
  if (value.isEmpty() || !isdigit(static_cast<unsigned char>(value[0]))) {
    return false;
  }

  out = ParsedVersion();
  size_t pos = 0;
  for (int i = 0; i < 3 && pos < value.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(value[pos]))) {
      return false;
    }
    long number = 0;
    while (pos < value.length() && isdigit(static_cast<unsigned char>(value[pos]))) {
      number = number * 10 + (value[pos] - '0');
      ++pos;
    }
    out.part[i] = number;
    if (pos < value.length() && value[pos] == '.' && i < 2) {
      ++pos;
      continue;
    }
    break;
  }
  if (pos < value.length()) {
    if (value[pos] != '-' && value[pos] != '+') {
      return false;
    }
    out.hasSuffix = value[pos] == '-';
  }
  return true;
}

String currentVersion() { return String(ZX_FW_VERSION); }

String buildEnvName() { return String(ZX_BUILD_ENV); }

// True when `latestTag` should replace the running firmware. Unparseable versions
// (e.g. dev builds tagged with a commit hash) fall back to "different means newer".
bool isNewerThanCurrent(const String &latestTag) {
  const String current = currentVersion();
  ParsedVersion a;
  ParsedVersion b;
  if (!parseVersion(latestTag, a) || !parseVersion(current, b)) {
    return latestTag != current;
  }
  for (int i = 0; i < 3; ++i) {
    if (a.part[i] != b.part[i]) {
      return a.part[i] > b.part[i];
    }
  }
  return !a.hasSuffix && b.hasSuffix;
}

// ---------------------------------------------------------------------------
// SD helpers
// ---------------------------------------------------------------------------

bool ensureSdMounted(bool forceMount, String *error) {
  if (gSdMountedForFirmware && !forceMount) {
    return true;
  }

#if HAL_HAS_DISPLAY
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_CC1101
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif
#if HAL_HAS_SD_CARD
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = sharedspi::bus();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);
  gSdMountedForFirmware = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
#else
  gSdMountedForFirmware = false;
  if (error) {
    *error = "SD card not available";
  }
  return false;
#endif
}

bool ensureFirmwareDirectory(String *error) {
  File node = SD.open(kFirmwareDir, FILE_READ);
  if (node) {
    const bool isDir = node.isDirectory();
    node.close();
    if (isDir) {
      return true;
    }
    if (error) {
      *error = "Path conflict: /firmware is file";
    }
    return false;
  }

  if (!SD.mkdir(kFirmwareDir)) {
    if (error) {
      *error = "Failed to create /firmware";
    }
    return false;
  }
  return true;
}

bool statSdFile(const String &path, uint32_t &sizeOut) {
  sizeOut = 0;

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  sizeOut = static_cast<uint32_t>(file.size());
  file.close();
  return true;
}

String readSmallSdFile(const char *path, size_t maxBytes = 128) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return "";
  }
  String value;
  while (file.available() && value.length() < maxBytes) {
    value += static_cast<char>(file.read());
  }
  file.close();
  value.trim();
  return value;
}

bool writeSmallSdFile(const char *path, const String &value) {
  if (SD.exists(path)) {
    SD.remove(path);
  }
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }
  const size_t written = file.print(value);
  file.close();
  return written == value.length();
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

bool beginHttps(HTTPClient &http,
                WiFiClientSecure &client,
                const String &url,
                String *error) {
  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  tlsca::attach(client);
  if (!http.begin(client, url)) {
    if (error) {
      *error = "HTTP begin failed";
    }
    return false;
  }

  // HTTP/1.0 avoids chunked transfer encoding so the body can be streamed as-is.
  http.useHTTP10(true);
  http.setTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  return true;
}

String describeHttpFailure(HTTPClient &http, int code) {
  if (code <= 0) {
    return String("HTTP failed: ") + HTTPClient::errorToString(code);
  }

  String msg = "HTTP " + String(code);
  StaticJsonDocument<64> filter;
  filter["message"] = true;
  DynamicJsonDocument doc(512);
  if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
    const String detail = String(static_cast<const char *>(doc["message"] | ""));
    if (!detail.isEmpty()) {
      msg += ": " + trimMiddle(detail, 40);
    }
  }
  if (code == 403 || code == 429) {
    msg += " (rate limited?)";
  }
  return msg;
}

bool httpGetJson(const String &url,
                 const JsonDocument &filter,
                 DynamicJsonDocument &docOut,
                 int &httpCodeOut,
                 String *error) {
  httpCodeOut = -1;

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginHttps(http, client, url, error)) {
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");

  const int code = http.GET();
  httpCodeOut = code;
  if (code != HTTP_CODE_OK) {
    if (error) {
      *error = describeHttpFailure(http, code);
    }
    http.end();
    return false;
  }

  const DeserializationError parseErr =
      deserializeJson(docOut, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (parseErr) {
    if (error) {
      *error = String("Release JSON parse failed: ") + parseErr.c_str();
    }
    return false;
  }
  return true;
}

bool httpGetSmallText(const String &url, String &out, String *error) {
  out = "";

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginHttps(http, client, url, error)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (error) {
      *error = code <= 0 ? String("HTTP failed: ") + HTTPClient::errorToString(code)
                         : "HTTP " + String(code);
    }
    http.end();
    return false;
  }

  WiFiClient &stream = http.getStream();
  const unsigned long startMs = millis();
  while (out.length() < 256 && millis() - startMs < kHttpTimeoutMs) {
    const int c = stream.read();
    if (c < 0) {
      if (!http.connected()) {
        break;
      }
      delay(2);
      continue;
    }
    out += static_cast<char>(c);
  }
  http.end();
  return true;
}

// Streams an HTTP body into `onChunk`. Fails on HTTP errors, stalls, and
// truncated bodies (fewer bytes than Content-Length or `expectedSize`).
bool streamHttpBody(const String &url,
                    uint32_t expectedSize,
                    const BeginFn &onBegin,
                    const ChunkFn &onChunk,
                    const std::function<void()> &backgroundTick,
                    const ProgressFn &progressTick,
                    uint32_t *bytesOut,
                    String *error) {
  if (bytesOut) {
    *bytesOut = 0;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginHttps(http, client, url, error)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (error) {
      *error = code <= 0 ? String("Download failed: ") + HTTPClient::errorToString(code)
                         : "Download HTTP " + String(code);
    }
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength > 0 && expectedSize > 0 &&
      static_cast<uint32_t>(contentLength) != expectedSize) {
    http.end();
    if (error) {
      *error = "Size mismatch: server " + formatBytes(contentLength) +
               ", release " + formatBytes(expectedSize);
    }
    return false;
  }
  const uint32_t total = contentLength > 0 ? static_cast<uint32_t>(contentLength) : expectedSize;

  if (onBegin && !onBegin(total, error)) {
    http.end();
    return false;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kTransferChunkBytes]);
  if (!buffer) {
    http.end();
    if (error) {
      *error = "Out of memory";
    }
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint32_t writtenTotal = 0;
  unsigned long lastDataMs = millis();
  if (progressTick) {
    progressTick(0, total);
  }

  while (total == 0 || writtenTotal < total) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected()) {
        break;
      }
      if (millis() - lastDataMs > kDownloadIdleTimeoutMs) {
        http.end();
        if (error) {
          *error = "Download stalled at " + formatBytes(writtenTotal);
        }
        return false;
      }
      delay(2);
      if (backgroundTick) {
        backgroundTick();
      }
      continue;
    }

    size_t toRead = std::min(available, kTransferChunkBytes);
    if (total > 0) {
      toRead = std::min(toRead, static_cast<size_t>(total - writtenTotal));
    }
    const int readLen = stream->readBytes(buffer.get(), toRead);
    if (readLen <= 0) {
      continue;
    }

    if (!onChunk(buffer.get(), static_cast<size_t>(readLen), error)) {
      http.end();
      return false;
    }

    writtenTotal += static_cast<uint32_t>(readLen);
    lastDataMs = millis();
    if (progressTick) {
      progressTick(writtenTotal, total);
    }
    if (backgroundTick) {
      backgroundTick();
    }
  }
  http.end();

  if (writtenTotal == 0) {
    if (error) {
      *error = "Downloaded file is empty";
    }
    return false;
  }
  if (total > 0 && writtenTotal != total) {
    if (error) {
      *error = "Download incomplete: " + formatBytes(writtenTotal) + " / " + formatBytes(total);
    }
    return false;
  }

  if (bytesOut) {
    *bytesOut = writtenTotal;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Release lookup
// ---------------------------------------------------------------------------

void buildReleaseFilter(JsonObject release) {
  release["tag_name"] = true;
  release["name"] = true;
  release["draft"] = true;
  release["prerelease"] = true;
  JsonObject asset = release["assets"].createNestedArray().createNestedObject();
  asset["name"] = true;
  asset["browser_download_url"] = true;
  asset["size"] = true;
}

// Picks this board's firmware ("openclaw-<env>-<tag>.bin", else the
// "-latest.bin" alias) and its ".sha256" sidecar from a release.
bool selectBoardAsset(JsonObjectConst release, ReleaseInfo &infoOut, String *error) {
  if (release["draft"] | false) {
    return false;
  }

  infoOut = ReleaseInfo();
  infoOut.tag = String(static_cast<const char *>(release["tag_name"] | ""));
  if (infoOut.tag.isEmpty()) {
    infoOut.tag = String(static_cast<const char *>(release["name"] | ""));
  }
  infoOut.prerelease = release["prerelease"] | false;

  const String base = ZX_FW_ASSET_PREFIX;
  const String versionedName = base + infoOut.tag + ".bin";
  const String aliasName = base + "latest.bin";

  JsonObjectConst versioned;
  JsonObjectConst alias;
  String versionedSha;
  String aliasSha;
  for (JsonObjectConst asset : release["assets"].as<JsonArrayConst>()) {
    const String name = String(static_cast<const char *>(asset["name"] | ""));
    const String url = String(static_cast<const char *>(asset["browser_download_url"] | ""));
    if (name == versionedName) {
      versioned = asset;
    } else if (name == aliasName) {
      alias = asset;
    } else if (name == versionedName + ".sha256") {
      versionedSha = url;
    } else if (name == aliasName + ".sha256") {
      aliasSha = url;
    }
  }

  const bool useVersioned = !versioned.isNull();
  const JsonObjectConst selected = useVersioned ? versioned : alias;
  if (selected.isNull()) {
    if (error) {
      *error = "No " + buildEnvName() + " firmware in " +
               (infoOut.tag.isEmpty() ? String("release") : infoOut.tag);
    }
    return false;
  }

  infoOut.assetName = String(static_cast<const char *>(selected["name"] | ""));
  infoOut.downloadUrl = String(static_cast<const char *>(selected["browser_download_url"] | ""));
  infoOut.size = selected["size"] | 0;
  infoOut.sha256Url = useVersioned ? versionedSha : aliasSha;
  if (infoOut.downloadUrl.isEmpty()) {
    if (error) {
      *error = "Release asset URL missing";
    }
    return false;
  }
  return true;
}

bool fetchLatestReleaseInfo(ReleaseInfo &infoOut, String *error) {
  const String apiBase = "https://api.github.com/repos/" + String(kFirmwareRepoSlug);

  {
    StaticJsonDocument<384> filter;
    buildReleaseFilter(filter.to<JsonObject>());
    DynamicJsonDocument doc(kLatestReleaseDocBytes);
    int code = -1;
    String latestErr;
    if (httpGetJson(apiBase + "/releases/latest", filter, doc, code, &latestErr)) {
      return selectBoardAsset(doc.as<JsonObjectConst>(), infoOut, error);
    }
    // 404 just means there is no stable release yet; try pre-releases below.
    if (code != HTTP_CODE_NOT_FOUND) {
      if (error) {
        *error = latestErr;
      }
      return false;
    }
  }

  StaticJsonDocument<448> filter;
  buildReleaseFilter(filter.to<JsonArray>().createNestedObject());
  DynamicJsonDocument doc(kReleaseListDocBytes);
  int code = -1;
  if (!httpGetJson(apiBase + "/releases?per_page=" + String(kReleaseListPageSize),
                   filter,
                   doc,
                   code,
                   error)) {
    return false;
  }

  String firstErr;
  for (JsonObjectConst release : doc.as<JsonArrayConst>()) {
    String err;
    if (selectBoardAsset(release, infoOut, &err)) {
      return true;
    }
    if (firstErr.isEmpty()) {
      firstErr = err;
    }
  }
  if (error) {
    *error = firstErr.isEmpty() ? String("No published firmware release found") : firstErr;
  }
  return false;
}

// Empty `shaOut` with a true result means the release publishes no checksum.
bool fetchExpectedSha256(const ReleaseInfo &info, String &shaOut, String *error) {
  shaOut = "";
  if (info.sha256Url.isEmpty()) {
    return true;
  }

  String body;
  String err;
  if (!httpGetSmallText(info.sha256Url, body, &err)) {
    if (error) {
      *error = "Checksum download failed: " + err;
    }
    return false;
  }
  shaOut = normalizeSha256(body);
  if (shaOut.isEmpty()) {
    if (error) {
      *error = "Checksum file is malformed";
    }
    return false;
  }
  return true;
}

bool checkSha256(const String &expected, const String &actual, String *error) {
  if (expected.isEmpty() || expected == actual) {
    return true;
  }
  if (error) {
    *error = "SHA-256 mismatch (got " + actual.substring(0, 8) + ", want " +
             expected.substring(0, 8) + ")";
  }
  return false;
}

// ---------------------------------------------------------------------------
// Flashing
// ---------------------------------------------------------------------------

bool beginFlash(uint32_t size, String *error) {
  if (Update.isRunning()) {
    Update.abort();
  }
  if (size > 0 && size > ESP.getFreeSketchSpace()) {
    if (error) {
      *error = "Image " + formatBytes(size) + " exceeds OTA slot " +
               formatBytes(ESP.getFreeSketchSpace());
    }
    return false;
  }
  if (!Update.begin(size > 0 ? size : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    if (error) {
      *error = String("Update begin failed: ") + Update.errorString();
    }
    return false;
  }
  return true;
}

bool writeFlash(const uint8_t *data, size_t len, String *error) {
  if (Update.write(const_cast<uint8_t *>(data), len) != len) {
    if (error) {
      *error = String("Flash write failed: ") + Update.errorString();
    }
    return false;
  }
  return true;
}

// Only commits the new image when its hash matches; otherwise the current
// firmware stays bootable.
bool finishFlash(const String &expectedSha, Sha256 &hasher, String *error) {
  if (!checkSha256(expectedSha, hasher.finishHex(), error)) {
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    if (error) {
      *error = String("Update end failed: ") + Update.errorString();
    }
    return false;
  }
  if (!Update.isFinished()) {
    if (error) {
      *error = "Update not finished";
    }
    return false;
  }
  return true;
}

bool flashFromUrl(const ReleaseInfo &info,
                  const String &expectedSha,
                  const std::function<void()> &backgroundTick,
                  const ProgressFn &progressTick,
                  String *error) {
  Sha256 hasher;
  const bool streamed = streamHttpBody(
      info.downloadUrl,
      info.size,
      [&](uint32_t total, String *err) { return beginFlash(total, err); },
      [&](const uint8_t *data, size_t len, String *err) {
        hasher.update(data, len);
        return writeFlash(data, len, err);
      },
      backgroundTick,
      progressTick,
      nullptr,
      error);
  if (!streamed) {
    if (Update.isRunning()) {
      Update.abort();
    }
    return false;
  }
  return finishFlash(expectedSha, hasher, error);
}

bool downloadToSd(const ReleaseInfo &info,
                  const String &expectedSha,
                  const std::function<void()> &backgroundTick,
                  const ProgressFn &progressTick,
                  uint32_t *bytesOut,
                  String *error) {
  String sdErr;
  if (!ensureSdMounted(false, &sdErr) || !ensureFirmwareDirectory(&sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  const String tempPath = String(kLatestFirmwarePath) + ".tmp";
  if (SD.exists(tempPath.c_str())) {
    SD.remove(tempPath.c_str());
  }
  File file = SD.open(tempPath.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "SD file open failed";
    }
    return false;
  }

  Sha256 hasher;
  const bool streamed = streamHttpBody(
      info.downloadUrl,
      info.size,
      nullptr,
      [&](const uint8_t *data, size_t len, String *err) {
        hasher.update(data, len);
        if (file.write(data, len) != len) {
          if (err) {
            *err = "SD write failed";
          }
          return false;
        }
        return true;
      },
      backgroundTick,
      progressTick,
      bytesOut,
      error);
  file.close();

  const String actualSha = streamed ? hasher.finishHex() : String("");
  if (!streamed || !checkSha256(expectedSha, actualSha, error)) {
    SD.remove(tempPath.c_str());
    return false;
  }

  if (SD.exists(kLatestFirmwarePath)) {
    SD.remove(kLatestFirmwarePath);
  }
  if (!SD.rename(tempPath.c_str(), kLatestFirmwarePath)) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "SD rename failed";
    }
    return false;
  }

  // The sidecar lets "Install from SD" re-verify the image before committing it.
  writeSmallSdFile(kLatestShaPath, actualSha);
  writeSmallSdFile(kLatestTagPath, info.tag);
  return true;
}

bool flashFromSd(const std::function<void()> &backgroundTick,
                 const ProgressFn &progressTick,
                 String *error) {
  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  File file = SD.open(kLatestFirmwarePath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Firmware file open failed";
    }
    return false;
  }

  const uint32_t size = static_cast<uint32_t>(file.size());
  if (size == 0) {
    file.close();
    if (error) {
      *error = "Firmware file is empty";
    }
    return false;
  }

  const String expectedSha = normalizeSha256(readSmallSdFile(kLatestShaPath));
  if (!beginFlash(size, error)) {
    file.close();
    return false;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kTransferChunkBytes]);
  if (!buffer) {
    file.close();
    Update.abort();
    if (error) {
      *error = "Out of memory";
    }
    return false;
  }

  Sha256 hasher;
  uint32_t writtenTotal = 0;
  if (progressTick) {
    progressTick(0, size);
  }
  while (writtenTotal < size) {
    const int readLen = file.read(buffer.get(), kTransferChunkBytes);
    if (readLen <= 0) {
      break;
    }
    hasher.update(buffer.get(), static_cast<size_t>(readLen));
    if (!writeFlash(buffer.get(), static_cast<size_t>(readLen), error)) {
      file.close();
      Update.abort();
      return false;
    }
    writtenTotal += static_cast<uint32_t>(readLen);
    if (progressTick) {
      progressTick(writtenTotal, size);
    }
    if (backgroundTick) {
      backgroundTick();
    }
  }
  file.close();

  if (writtenTotal != size) {
    Update.abort();
    if (error) {
      *error = "SD read failed at " + formatBytes(writtenTotal);
    }
    return false;
  }
  return finishFlash(expectedSha, hasher, error);
}

// ---------------------------------------------------------------------------
// UI flows
// ---------------------------------------------------------------------------

struct UpdateSession {
  String lastAction;
  ReleaseInfo latest;
  bool haveLatest = false;
};

bool confirmInstall(AppContext &ctx,
                    const String &title,
                    const String &message,
                    const std::function<void()> &backgroundTick) {
  if (!ctx.uiRuntime->confirm(title, message, backgroundTick, "Install", "Cancel")) {
    return false;
  }
  return ctx.uiRuntime->confirm("Confirm Again",
                                "Device will reboot after install",
                                backgroundTick,
                                "Install",
                                "Cancel");
}

void rebootAfterInstall(AppContext &ctx, const std::function<void()> &backgroundTick) {
  ctx.uiRuntime->showToast("Firmware", "Install complete, rebooting", 1200, backgroundTick);
  delay(300);
  ESP.restart();
}

bool checkLatest(AppContext &ctx,
                 UpdateSession &session,
                 const std::function<void()> &backgroundTick) {
  String err;
  bool ok = false;
  {
    OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Checking GitHub releases...", -1);
    ok = fetchLatestReleaseInfo(session.latest, &err);
  }
  session.haveLatest = ok;
  if (!ok) {
    session.lastAction = "Check failed: " + err;
    ctx.uiRuntime->showToast("Firmware", err, 2000, backgroundTick);
  }
  return ok;
}

void showStatus(AppContext &ctx,
                const UpdateSession &session,
                const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("Current: " + currentVersion());
  lines.push_back("Board: " + buildEnvName());
  lines.push_back("Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  lines.push_back("OTA slot: " + formatBytes(ESP.getFreeSketchSpace()));
  if (session.haveLatest) {
    lines.push_back("Latest: " + session.latest.tag +
                    (isNewerThanCurrent(session.latest.tag) ? " (update)" : " (current)"));
  } else {
    lines.push_back("Latest: (not checked)");
  }

#if HAL_HAS_SD_CARD
  uint32_t sdSize = 0;
  if (ensureSdMounted(false, nullptr) && statSdFile(kLatestFirmwarePath, sdSize)) {
    const String tag = readSmallSdFile(kLatestTagPath);
    lines.push_back("SD: " + (tag.isEmpty() ? String("latest.bin") : tag) + " " +
                    formatBytes(sdSize));
  } else {
    lines.push_back("SD: (no package)");
  }
#endif

  lines.push_back("Repo: " + String(kFirmwareRepoSlug));
  if (!session.lastAction.isEmpty()) {
    lines.push_back("Last: " + session.lastAction);
  }

  ctx.uiRuntime->showInfo("Firmware Status", lines, backgroundTick, "OK/BACK Exit");
}

// Downloads and flashes directly over Wi-Fi; no SD card needed.
void updateOverTheAir(AppContext &ctx,
                      UpdateSession &session,
                      const std::function<void()> &backgroundTick) {
  if (!checkLatest(ctx, session, backgroundTick)) {
    return;
  }
  const ReleaseInfo &info = session.latest;

  String prompt;
  if (isNewerThanCurrent(info.tag)) {
    prompt = currentVersion() + " -> " + info.tag + " (" + formatBytes(info.size) + ")";
  } else {
    prompt = "Already on " + info.tag + ". Reinstall?";
  }
  if (!confirmInstall(ctx, "Update Firmware", prompt, backgroundTick)) {
    return;
  }

  String err;
  String expectedSha;
  if (!fetchExpectedSha256(info, expectedSha, &err)) {
    session.lastAction = "Update failed: " + err;
    ctx.uiRuntime->showToast("Firmware", err, 2000, backgroundTick);
    return;
  }
  if (expectedSha.isEmpty() &&
      !ctx.uiRuntime->confirm("No Checksum",
                              "Release has no SHA-256. Flash anyway?",
                              backgroundTick,
                              "Flash",
                              "Cancel")) {
    return;
  }

  bool ok = false;
  {
    OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Connecting...", 0);
    ok = flashFromUrl(info,
                      expectedSha,
                      backgroundTick,
                      [&](uint32_t written, uint32_t total) {
                        overlay.update("Firmware Update",
                                       progressText("Flashing", written, total),
                                       percentOf(written, total));
                      },
                      &err);
  }
  if (!ok) {
    session.lastAction = "Update failed: " + err;
    ctx.uiRuntime->showToast("Firmware", err + " (current firmware kept)", 2400, backgroundTick);
    return;
  }

  rebootAfterInstall(ctx, backgroundTick);
}

void downloadPackageToSd(AppContext &ctx,
                         UpdateSession &session,
                         const std::function<void()> &backgroundTick) {
  if (!checkLatest(ctx, session, backgroundTick)) {
    return;
  }
  const ReleaseInfo &info = session.latest;

  String err;
  String expectedSha;
  uint32_t downloaded = 0;
  bool ok = fetchExpectedSha256(info, expectedSha, &err);
  if (ok) {
    OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Preparing download...", -1);
    ok = downloadToSd(info,
                      expectedSha,
                      backgroundTick,
                      [&](uint32_t written, uint32_t total) {
                        overlay.update("Firmware Update",
                                       progressText("Downloading", written, total),
                                       percentOf(written, total));
                      },
                      &downloaded,
                      &err);
  }
  if (!ok) {
    session.lastAction = "Download failed: " + err;
    ctx.uiRuntime->showToast("Firmware", err, 2000, backgroundTick);
    return;
  }

  session.lastAction = "Saved " + info.tag + " (" + formatBytes(downloaded) +
                       (expectedSha.isEmpty() ? ", unverified)" : ", verified)");
  ctx.uiRuntime->showToast("Firmware", "Saved to /firmware/latest.bin", 1600, backgroundTick);
}

void installPackageFromSd(AppContext &ctx,
                          UpdateSession &session,
                          const std::function<void()> &backgroundTick) {
  uint32_t size = 0;
  if (!ensureSdMounted(false, nullptr) || !statSdFile(kLatestFirmwarePath, size)) {
    ctx.uiRuntime->showToast("Firmware", "Downloaded package not found", 1700, backgroundTick);
    return;
  }

  const String tag = readSmallSdFile(kLatestTagPath);
  const String label = tag.isEmpty() ? String("/firmware/latest.bin") : tag;
  if (!confirmInstall(ctx,
                      "Install Firmware",
                      "Flash " + label + " (" + formatBytes(size) + ")?",
                      backgroundTick)) {
    return;
  }

  String err;
  bool ok = false;
  {
    OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Flashing firmware...", 0);
    ok = flashFromSd(backgroundTick,
                     [&](uint32_t written, uint32_t total) {
                       overlay.update("Firmware Update",
                                      progressText("Flashing", written, total),
                                      percentOf(written, total));
                     },
                     &err);
  }
  if (!ok) {
    session.lastAction = "Install failed: " + err;
    ctx.uiRuntime->showToast("Firmware", err, 2000, backgroundTick);
    return;
  }

  rebootAfterInstall(ctx, backgroundTick);
}

void showLatest(AppContext &ctx,
                UpdateSession &session,
                const std::function<void()> &backgroundTick) {
  if (!checkLatest(ctx, session, backgroundTick)) {
    return;
  }
  const ReleaseInfo &info = session.latest;
  const bool newer = isNewerThanCurrent(info.tag);
  session.lastAction = "Checked: " + info.tag;

  std::vector<String> lines;
  lines.push_back(newer ? "Update available" : "Up to date");
  lines.push_back("Current: " + currentVersion());
  lines.push_back("Latest: " + info.tag + (info.prerelease ? " (pre)" : ""));
  lines.push_back("Asset: " + trimMiddle(info.assetName, 34));
  lines.push_back("Size: " + formatBytes(info.size));
  lines.push_back("SHA-256: " + String(info.sha256Url.isEmpty() ? "not published" : "published"));
  ctx.uiRuntime->showInfo("Latest Firmware", lines, backgroundTick, "OK/BACK Exit");

  if (newer && ctx.uiRuntime->confirm("Update Available",
                                      "Install " + info.tag + " now?",
                                      backgroundTick,
                                      "Update",
                                      "Later")) {
    updateOverTheAir(ctx, session, backgroundTick);
  }
}

}  // namespace

void runFirmwareUpdateApp(AppContext &ctx,
                          const std::function<void()> &backgroundTick) {
  enum class Action { Status, Check, UpdateNow, DownloadSd, InstallSd, Back };

  int selected = 0;
  UpdateSession session;

  while (true) {
    std::vector<String> menu;
    std::vector<Action> actions;
    menu.push_back("Status");
    actions.push_back(Action::Status);
    menu.push_back("Check for Update");
    actions.push_back(Action::Check);
    menu.push_back("Update Now (Wi-Fi)");
    actions.push_back(Action::UpdateNow);
#if HAL_HAS_SD_CARD
    uint32_t sdSize = 0;
    const bool sdPackage = ensureSdMounted(false, nullptr) &&
                           statSdFile(kLatestFirmwarePath, sdSize);
    menu.push_back("Download to SD");
    actions.push_back(Action::DownloadSd);
    menu.push_back(String("Install from SD ") +
                   (sdPackage ? "(" + formatBytes(sdSize) + ")" : "(missing)"));
    actions.push_back(Action::InstallSd);
#endif
    menu.push_back("Back");
    actions.push_back(Action::Back);

    const int choice = ctx.uiRuntime->menuLoop("Firmware Update",
                                                menu,
                                                selected,
                                                backgroundTick,
                                                "OK Select  BACK Exit",
                                                trimMiddle(currentVersion() + " / " + buildEnvName(), 26));
    if (choice < 0 || choice >= static_cast<int>(actions.size())) {
      return;
    }
    selected = choice;

    switch (actions[choice]) {
      case Action::Status:
        showStatus(ctx, session, backgroundTick);
        break;
      case Action::Check:
        showLatest(ctx, session, backgroundTick);
        break;
      case Action::UpdateNow:
        updateOverTheAir(ctx, session, backgroundTick);
        break;
      case Action::DownloadSd:
        downloadPackageToSd(ctx, session, backgroundTick);
        break;
      case Action::InstallSd:
        installPackageFromSd(ctx, session, backgroundTick);
        break;
      case Action::Back:
        return;
    }
  }
}
