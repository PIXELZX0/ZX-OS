#include "firmware_update_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <vector>

#include "../core/board_pins.h"
#include "../core/shared_spi_bus.h"
#include "../ui/ui_runtime.h"

namespace {

constexpr const char *kFirmwareRepoSlug = "HITEYY/AI-cc1101";
constexpr const char *kFirmwareDir = "/firmware";
constexpr const char *kLatestFirmwarePath = "/firmware/latest.bin";
constexpr size_t kTransferChunkBytes = 2048;
constexpr unsigned long kDownloadIdleTimeoutMs = 12000UL;

struct ReleaseInfo {
  String tag;
  String assetName;
  String downloadUrl;
  uint32_t size = 0;
};

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

bool hasBinExtension(const String &pathOrNameRaw) {
  String pathOrName = pathOrNameRaw;
  pathOrName.toLowerCase();
  return pathOrName.endsWith(".bin");
}

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

bool httpGetSecure(const String &url,
                   String &responseOut,
                   int &httpCodeOut,
                   String *error) {
  responseOut = "";
  httpCodeOut = -1;

  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (error) {
      *error = "HTTP begin failed";
    }
    return false;
  }

  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AI-cc1101-FirmwareUpdate");
  http.addHeader("Accept", "application/vnd.github+json");

  const int code = http.GET();
  httpCodeOut = code;
  if (code > 0) {
    responseOut = http.getString();
  }
  http.end();

  if (code <= 0) {
    if (error) {
      *error = "HTTP request failed";
    }
    return false;
  }
  if (code < 200 || code >= 300) {
    if (error) {
      String msg = "HTTP " + String(code);
      DynamicJsonDocument doc(768);
      const auto parseErr = deserializeJson(doc, responseOut);
      if (!parseErr && doc.is<JsonObjectConst>()) {
        const String detail = String(static_cast<const char *>(doc["message"] | ""));
        if (!detail.isEmpty()) {
          msg += ": " + detail;
        }
      }
      *error = msg;
    }
    return false;
  }
  return true;
}

bool parseLatestReleaseBody(const String &body,
                            const String &preferredAssetNameRaw,
                            ReleaseInfo &infoOut,
                            String *error) {
  DynamicJsonDocument doc(32768);
  const auto parseErr = deserializeJson(doc, body);
  if (parseErr || !doc.is<JsonObject>()) {
    if (error) {
      *error = "Release JSON parse failed";
    }
    return false;
  }

  const JsonObjectConst root = doc.as<JsonObjectConst>();
  infoOut.tag = String(static_cast<const char *>(root["tag_name"] | ""));
  if (infoOut.tag.isEmpty()) {
    infoOut.tag = String(static_cast<const char *>(root["name"] | ""));
  }
  if (infoOut.tag.isEmpty()) {
    infoOut.tag = "(unknown)";
  }

  if (!root["assets"].is<JsonArrayConst>()) {
    if (error) {
      *error = "Release has no assets";
    }
    return false;
  }

  const JsonArrayConst assets = root["assets"].as<JsonArrayConst>();
  if (assets.size() == 0) {
    if (error) {
      *error = "Release has empty assets";
    }
    return false;
  }

  String preferredAssetName = preferredAssetNameRaw;
  preferredAssetName.trim();

  JsonObjectConst selected;
  if (!preferredAssetName.isEmpty()) {
    for (JsonObjectConst asset : assets) {
      const String name = String(static_cast<const char *>(asset["name"] | ""));
      if (name == preferredAssetName) {
        selected = asset;
        break;
      }
    }
  }

  if (selected.isNull()) {
    for (JsonObjectConst asset : assets) {
      const String name = String(static_cast<const char *>(asset["name"] | ""));
      if (hasBinExtension(name)) {
        selected = asset;
        break;
      }
    }
  }

  if (selected.isNull()) {
    selected = assets[0];
  }

  infoOut.assetName = String(static_cast<const char *>(selected["name"] | ""));
  infoOut.downloadUrl =
      String(static_cast<const char *>(selected["browser_download_url"] | ""));
  infoOut.size = selected["size"] | 0;

  if (infoOut.assetName.isEmpty() || infoOut.downloadUrl.isEmpty()) {
    if (error) {
      *error = "Release asset URL missing";
    }
    return false;
  }
  return true;
}

bool parseReleaseCatalogBody(const String &body,
                             ReleaseInfo &infoOut,
                             String *error) {
  DynamicJsonDocument doc(16384);
  const auto parseErr = deserializeJson(doc, body);
  if (parseErr || !doc.is<JsonArrayConst>()) {
    if (error) {
      *error = "Release list parse failed";
    }
    return false;
  }

  const JsonArrayConst roots = doc.as<JsonArrayConst>();
  for (JsonObjectConst root : roots) {
    if (root["draft"] | false) {
      continue;
    }
    if (!root["assets"].is<JsonArrayConst>()) {
      continue;
    }

    const JsonArrayConst assets = root["assets"].as<JsonArrayConst>();
    if (assets.size() == 0) {
      continue;
    }

    JsonObjectConst selected;
    for (JsonObjectConst asset : assets) {
      const String name = String(static_cast<const char *>(asset["name"] | ""));
      if (hasBinExtension(name)) {
        selected = asset;
        break;
      }
    }
    if (selected.isNull()) {
      selected = assets[0];
    }

    infoOut.tag = String(static_cast<const char *>(root["tag_name"] | ""));
    if (infoOut.tag.isEmpty()) {
      infoOut.tag = String(static_cast<const char *>(root["name"] | ""));
    }
    if (infoOut.tag.isEmpty()) {
      infoOut.tag = "(unknown)";
    }

    infoOut.assetName = String(static_cast<const char *>(selected["name"] | ""));
    infoOut.downloadUrl =
        String(static_cast<const char *>(selected["browser_download_url"] | ""));
    infoOut.size = selected["size"] | 0;

    if (!infoOut.assetName.isEmpty() && !infoOut.downloadUrl.isEmpty()) {
      return true;
    }
  }

  if (error) {
    *error = "No releases with downloadable assets";
  }
  return false;
}

bool fetchLatestReleaseInfo(ReleaseInfo &infoOut,
                            String *error) {
  const String latestUrl = "https://api.github.com/repos/" + String(kFirmwareRepoSlug) +
                           "/releases/latest";
  String latestBody;
  int latestCode = -1;
  String latestErr;
  if (httpGetSecure(latestUrl, latestBody, latestCode, &latestErr)) {
    return parseLatestReleaseBody(latestBody, "", infoOut, error);
  }

  const String listUrl = "https://api.github.com/repos/" + String(kFirmwareRepoSlug) +
                         "/releases?per_page=8";
  String listBody;
  int listCode = -1;
  String listErr;
  if (httpGetSecure(listUrl, listBody, listCode, &listErr)) {
    if (parseReleaseCatalogBody(listBody, infoOut, error)) {
      return true;
    }
    if (error) {
      *error = "No published firmware release found. Publish a GitHub Release with a .bin asset.";
    }
    return false;
  }
  if (error) {
    *error = latestErr + " / " + listErr;
  }
  return false;
}

bool downloadUrlToSdFile(const String &url,
                         const char *destPath,
                         const std::function<void()> &backgroundTick,
                         const std::function<void(uint32_t, int)> &progressTick,
                         uint32_t *bytesOut,
                         String *error) {
  if (bytesOut) {
    *bytesOut = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }
  if (!ensureFirmwareDirectory(&sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  const String tempPath = String(destPath) + ".tmp";
  if (SD.exists(tempPath.c_str())) {
    SD.remove(tempPath.c_str());
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (error) {
      *error = "HTTP begin failed";
    }
    return false;
  }

  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AI-cc1101-FirmwareUpdate");

  const int code = http.GET();
  const int totalSize = http.getSize();
  if (code <= 0 || code < 200 || code >= 300) {
    const String response = code > 0 ? http.getString() : String("");
    http.end();
    if (error) {
      String msg = code <= 0 ? String("Download HTTP failed")
                             : String("HTTP ") + String(code);
      if (!response.isEmpty()) {
        msg += ": " + trimMiddle(response, 40);
      }
      *error = msg;
    }
    return false;
  }

  File file = SD.open(tempPath.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    http.end();
    if (error) {
      *error = "SD file open failed";
    }
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int remain = http.getSize();
  static uint8_t buffer[kTransferChunkBytes];
  uint32_t writtenTotal = 0;
  unsigned long lastProgressMs = millis();
  if (progressTick) {
    progressTick(0, totalSize);
  }

  while (http.connected() && (remain > 0 || remain == -1)) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastProgressMs > kDownloadIdleTimeoutMs) {
        file.close();
        http.end();
        SD.remove(tempPath.c_str());
        if (error) {
          *error = "Download timeout";
        }
        return false;
      }
      delay(5);
      if (backgroundTick) {
        backgroundTick();
      }
      continue;
    }

    const size_t toRead = std::min(available, sizeof(buffer));
    const int readLen = stream->readBytes(buffer, toRead);
    if (readLen <= 0) {
      continue;
    }

    const size_t written = file.write(buffer, static_cast<size_t>(readLen));
    if (written != static_cast<size_t>(readLen)) {
      file.close();
      http.end();
      SD.remove(tempPath.c_str());
      if (error) {
        *error = "SD write failed";
      }
      return false;
    }

    writtenTotal += static_cast<uint32_t>(written);
    if (remain > 0) {
      remain -= static_cast<int>(written);
    }

    lastProgressMs = millis();
    if (progressTick) {
      progressTick(writtenTotal, totalSize);
    }
    if (backgroundTick) {
      backgroundTick();
    }
  }

  file.close();
  http.end();

  if (writtenTotal == 0) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "Downloaded file is empty";
    }
    return false;
  }

  if (SD.exists(destPath)) {
    SD.remove(destPath);
  }
  if (!SD.rename(tempPath.c_str(), destPath)) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "SD rename failed";
    }
    return false;
  }

  if (bytesOut) {
    *bytesOut = writtenTotal;
  }
  if (progressTick) {
    progressTick(writtenTotal, totalSize);
  }
  return true;
}

bool installFirmwareFromSd(const String &path,
                           const std::function<void()> &backgroundTick,
                           const std::function<void(size_t, size_t)> &progressTick,
                           String *error) {
  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Firmware file open failed";
    }
    return false;
  }

  const size_t size = file.size();
  if (size == 0) {
    file.close();
    if (error) {
      *error = "Firmware file is empty";
    }
    return false;
  }

  if (!Update.begin(size, U_FLASH)) {
    file.close();
    if (error) {
      *error = String("Update begin failed: ") + Update.errorString();
    }
    return false;
  }

  static uint8_t buffer[kTransferChunkBytes];
  size_t writtenTotal = 0;
  if (progressTick) {
    progressTick(0, size);
  }
  while (file.available()) {
    const int readLen = file.read(buffer, sizeof(buffer));
    if (readLen <= 0) {
      continue;
    }

    const size_t written = Update.write(buffer, static_cast<size_t>(readLen));
    if (written != static_cast<size_t>(readLen)) {
      file.close();
      Update.abort();
      if (error) {
        *error = String("Update write failed: ") + Update.errorString();
      }
      return false;
    }
    writtenTotal += written;
    if (progressTick) {
      progressTick(writtenTotal, size);
    }

    if (backgroundTick) {
      backgroundTick();
    }
  }

  file.close();

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

  if (progressTick) {
    progressTick(size, size);
  }

  return true;
}

bool confirmInstall(AppContext &ctx,
                    const String &title,
                    const String &message,
                    const std::function<void()> &backgroundTick) {
  if (!ctx.uiRuntime->confirm(title, message, backgroundTick, "Install", "Cancel")) {
    return false;
  }
  if (!ctx.uiRuntime->confirm("Confirm Again",
                              "Device will reboot after install",
                              backgroundTick,
                              "Install",
                              "Cancel")) {
    return false;
  }
  return true;
}

void showStatus(AppContext &ctx,
                const String &lastAction,
                const String &lastTag,
                const String &lastAsset,
                const std::function<void()> &backgroundTick) {
  uint32_t latestSize = 0;

  String sdErr;
  if (ensureSdMounted(false, &sdErr)) {
    statSdFile(kLatestFirmwarePath, latestSize);
  }

  std::vector<String> lines;
  lines.push_back("Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  lines.push_back("Repo: " + String(kFirmwareRepoSlug));
  lines.push_back("Asset: (auto .bin)");
  lines.push_back("Latest tag: " + (lastTag.isEmpty() ? String("-") : lastTag));
  lines.push_back("Latest asset: " + (lastAsset.isEmpty() ? String("-") : lastAsset));
  lines.push_back("Downloaded: " +
                  (latestSize == 0 ? String("(none)")
                                   : String(kLatestFirmwarePath) + " " + formatBytes(latestSize)));
  if (!lastAction.isEmpty()) {
    lines.push_back("Last: " + lastAction);
  }

  ctx.uiRuntime->showInfo("Firmware Status", lines, backgroundTick, "OK/BACK Exit");
}

bool downloadLatest(AppContext &ctx,
                    ReleaseInfo &infoOut,
                    uint32_t &downloadedBytesOut,
                    const std::function<void()> &backgroundTick,
                    String *error) {
  if (!fetchLatestReleaseInfo(infoOut, error)) {
    return false;
  }

  downloadedBytesOut = 0;
  OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Preparing download...", -1);
  return downloadUrlToSdFile(infoOut.downloadUrl,
                             kLatestFirmwarePath,
                             backgroundTick,
                             [&](uint32_t written, int total) {
                               int percent = -1;
                               String progressText = "Downloading " + formatBytes(written);
                               if (total > 0) {
                                 percent = static_cast<int>((static_cast<uint64_t>(written) * 100ULL) /
                                                            static_cast<uint64_t>(total));
                                 progressText += " / " + formatBytes(static_cast<uint32_t>(total));
                               }
                               overlay.update("Firmware Update", progressText, percent);
                             },
                             &downloadedBytesOut,
                             error);
}

bool installDownloaded(AppContext &ctx,
                       const std::function<void()> &backgroundTick,
                       String *error) {
  OverlayScope overlay(ctx.uiRuntime, "Firmware Update", "Flashing firmware...", 0);
  return installFirmwareFromSd(kLatestFirmwarePath,
                               backgroundTick,
                               [&](size_t written, size_t total) {
                                 int percent = -1;
                                 if (total > 0) {
                                   percent = static_cast<int>((written * 100ULL) / total);
                                 }
                                 const String progressText =
                                     "Flashing " + formatBytes(static_cast<uint64_t>(written)) +
                                     " / " + formatBytes(static_cast<uint64_t>(total));
                                 overlay.update("Firmware Update", progressText, percent);
                               },
                               error);
}

}  // namespace

void runFirmwareUpdateApp(AppContext &ctx,
                          const std::function<void()> &backgroundTick) {
  int selected = 0;
  String lastAction;
  String lastTag;
  String lastAsset;

  while (true) {
    uint32_t latestSize = 0;
    const bool latestExists = ensureSdMounted(false, nullptr) &&
                              statSdFile(kLatestFirmwarePath, latestSize);

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Check Latest");
    menu.push_back("Download Latest");
    menu.push_back(String("Install Downloaded ") +
                   (latestExists ? "(" + formatBytes(latestSize) + ")" : "(missing)"));
    menu.push_back("Update Now");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("Firmware Update",
                                                menu,
                                                selected,
                                                backgroundTick,
                                                "OK Select  BACK Exit",
                                                "Repo: " + trimMiddle(String(kFirmwareRepoSlug), 22));
    if (choice < 0 || choice == 5) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      showStatus(ctx, lastAction, lastTag, lastAsset, backgroundTick);
      continue;
    }

    if (choice == 1) {
      ReleaseInfo info;
      String err;
      if (!fetchLatestReleaseInfo(info, &err)) {
        lastAction = "Latest check failed: " + err;
        ctx.uiRuntime->showToast("Firmware", err, 1800, backgroundTick);
        continue;
      }

      lastTag = info.tag;
      lastAsset = info.assetName;
      lastAction = "Latest: " + info.tag + " / " + info.assetName;

      std::vector<String> lines;
      lines.push_back("Tag: " + info.tag);
      lines.push_back("Asset: " + info.assetName);
      lines.push_back("Size: " + formatBytes(info.size));
      lines.push_back("URL:");
      lines.push_back(trimMiddle(info.downloadUrl, 38));
      ctx.uiRuntime->showInfo("Latest Firmware", lines, backgroundTick, "OK/BACK Exit");
      continue;
    }

    if (choice == 2) {
      ReleaseInfo info;
      String err;
      uint32_t downloaded = 0;
      if (!downloadLatest(ctx, info, downloaded, backgroundTick, &err)) {
        lastAction = "Download failed: " + err;
        ctx.uiRuntime->showToast("Firmware", err, 1800, backgroundTick);
        continue;
      }

      lastTag = info.tag;
      lastAsset = info.assetName;
      lastAction = "Downloaded " + info.assetName + " (" + formatBytes(downloaded) + ")";
      ctx.uiRuntime->showToast("Firmware", "Downloaded to /firmware/latest.bin", 1600, backgroundTick);
      continue;
    }

    if (choice == 3) {
      if (!latestExists) {
        ctx.uiRuntime->showToast("Firmware", "Downloaded package not found", 1700, backgroundTick);
        continue;
      }
      if (!confirmInstall(ctx,
                          "Install Firmware",
                          "Flash /firmware/latest.bin?",
                          backgroundTick)) {
        continue;
      }

      String err;
      if (!installDownloaded(ctx, backgroundTick, &err)) {
        lastAction = "Install failed: " + err;
        ctx.uiRuntime->showToast("Firmware", err, 1900, backgroundTick);
        continue;
      }

      ctx.uiRuntime->showToast("Firmware", "Install complete, rebooting", 1200, backgroundTick);
      delay(300);
      ESP.restart();
      return;
    }

    if (choice == 4) {
      if (!ctx.uiRuntime->confirm("Update Now",
                                  "Download and install latest firmware?",
                                  backgroundTick,
                                  "Update",
                                  "Cancel")) {
        continue;
      }

      ReleaseInfo info;
      String err;
      uint32_t downloaded = 0;
      if (!downloadLatest(ctx, info, downloaded, backgroundTick, &err)) {
        lastAction = "Update failed: " + err;
        ctx.uiRuntime->showToast("Firmware", err, 1900, backgroundTick);
        continue;
      }

      lastTag = info.tag;
      lastAsset = info.assetName;
      lastAction = "Downloaded " + info.assetName + " (" + formatBytes(downloaded) + ")";

      if (!confirmInstall(ctx,
                          "Install Latest",
                          "Install downloaded latest firmware?",
                          backgroundTick)) {
        lastAction = "Latest downloaded (install canceled)";
        continue;
      }

      if (!installDownloaded(ctx, backgroundTick, &err)) {
        lastAction = "Install failed: " + err;
        ctx.uiRuntime->showToast("Firmware", err, 1900, backgroundTick);
        continue;
      }

      ctx.uiRuntime->showToast("Firmware", "Update complete, rebooting", 1200, backgroundTick);
      delay(300);
      ESP.restart();
      return;
    }
  }
}
