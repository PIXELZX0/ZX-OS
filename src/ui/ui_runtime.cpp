#include "ui_runtime.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "../core/board_pins.h"
#include "sd_font.h"
#include "../core/runtime_config.h"
#include "input_adapter.h"
#include "launcher_icons.h"
#include "lvgl_port.h"
#include "user_config.h"

namespace {

constexpr int kHeaderHeight = 24;
constexpr int kSubtitleHeight = 17;
constexpr int kFooterHeight = 18;
constexpr int kRowHeight = 20;
constexpr int kSidePadding = 8;
constexpr int kMinContentHeight = 24;
constexpr lv_style_selector_t kStyleAny =
    static_cast<lv_style_selector_t>(LV_PART_MAIN) |
    static_cast<lv_style_selector_t>(LV_STATE_ANY);

constexpr uint32_t kClrBg = 0x0B0F14;
constexpr uint32_t kClrPanel = 0x121923;
constexpr uint32_t kClrPanelSoft = 0x0F151E;
constexpr uint32_t kClrBorder = 0x2A3544;
constexpr uint32_t kClrAccent = 0x58A6FF;
constexpr uint32_t kClrAccentSoft = 0x1D304B;
constexpr uint32_t kClrTextPrimary = 0xF5F7FA;
constexpr uint32_t kClrTextMuted = 0xAAB7C8;
constexpr lv_opa_t kOpa75 = static_cast<lv_opa_t>(191);
constexpr lv_opa_t kOpa85 = static_cast<lv_opa_t>(217);
constexpr lv_opa_t kOpa90 = static_cast<lv_opa_t>(230);
constexpr lv_opa_t kOpa92 = static_cast<lv_opa_t>(235);

constexpr unsigned long kHeaderRefreshMs = 1000UL;
constexpr unsigned long kBatteryPollMs = 5000UL;
constexpr unsigned long kNtpRetryMs = 30000UL;
constexpr unsigned long kUnixSyncRetryMs = 30000UL;
constexpr unsigned long kUnixSyncRefreshMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kUiLoopDelayMs = 2UL;
constexpr unsigned long kBackgroundTickMinIntervalMs = 20UL;
constexpr uint32_t kTlsMinInternalFreeBytesUi = 36000U;
constexpr uint32_t kTlsMinInternalLargestBytesUi = 18000U;
constexpr time_t kMinValidUnixTimeSec = 946684800;  // 2000-01-01T00:00:00Z
constexpr uint64_t kWindowsEpochOffset100Ns = 116444736000000000ULL;
constexpr uint64_t kHundredNsPerSecond = 10000000ULL;

constexpr uint32_t kLauncherBg = kClrBg;
constexpr uint32_t kLauncherPrimary = 0xEAF6FF;
constexpr uint32_t kLauncherSide = 0x2D6F93;
constexpr uint32_t kLauncherMuted = 0x8FB6CC;
constexpr uint32_t kLauncherLine = 0x1A3344;
constexpr uint32_t kLauncherCharging = 0x4CD964;
// Avoid LEDC full-on edge behavior at max duty.
constexpr uint8_t kBacklightPwmMaxDuty = 254U;

uint8_t clampBrightnessPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return static_cast<uint8_t>(percent);
}

uint8_t brightnessPwmFromPercent(uint8_t percent) {
  const uint32_t scaled =
      static_cast<uint32_t>(percent) * static_cast<uint32_t>(kBacklightPwmMaxDuty);
  return static_cast<uint8_t>((scaled + 50U) / 100U);
}

int wrapIndex(int value, int count) {
  if (count <= 0) {
    return 0;
  }
  while (value < 0) {
    value += count;
  }
  while (value >= count) {
    value -= count;
  }
  return value;
}

String maskIfNeeded(const String &value, bool mask) {
  if (!mask) {
    return value;
  }
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    out += "*";
  }
  return out;
}

bool isValidUnixTime(time_t unixSec) {
  return unixSec >= kMinValidUnixTimeSec;
}

bool isAllDigits(const String &value) {
  if (value.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

bool hasTlsHeapHeadroomUi() {
  const uint32_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return freeBytes >= kTlsMinInternalFreeBytesUi &&
         largest >= kTlsMinInternalLargestBytesUi;
}

bool parseUtcOffsetMinutes(const String &input, int *outMinutes) {
  if (!outMinutes) {
    return false;
  }

  String work = input;
  work.trim();
  if (work.length() == 0) {
    return false;
  }

  if (work.equalsIgnoreCase("UTC") ||
      work.equalsIgnoreCase("GMT") ||
      work.equalsIgnoreCase("Z")) {
    *outMinutes = 0;
    return true;
  }

  if (work.length() >= 3 &&
      (work.substring(0, 3).equalsIgnoreCase("UTC") ||
       work.substring(0, 3).equalsIgnoreCase("GMT"))) {
    work = work.substring(3);
    work.trim();
    if (work.length() == 0) {
      *outMinutes = 0;
      return true;
    }
  }

  const char signChar = work[0];
  if (signChar != '+' && signChar != '-') {
    return false;
  }
  const int sign = signChar == '+' ? 1 : -1;

  String digits = work.substring(1);
  digits.trim();
  digits.replace(":", "");
  if (digits.length() < 1 || digits.length() > 4 || !isAllDigits(digits)) {
    return false;
  }

  int hours = 0;
  int mins = 0;
  if (digits.length() <= 2) {
    hours = digits.toInt();
  } else {
    const int cut = static_cast<int>(digits.length()) - 2;
    const String hh = digits.substring(0, cut);
    const String mm = digits.substring(cut);
    if (!isAllDigits(hh) || !isAllDigits(mm)) {
      return false;
    }
    hours = hh.toInt();
    mins = mm.toInt();
  }

  if (hours > 14 || mins > 59) {
    return false;
  }

  *outMinutes = sign * ((hours * 60) + mins);
  return true;
}

String posixTzFromUtcOffsetMinutes(int utcOffsetMinutes) {
  if (utcOffsetMinutes == 0) {
    return String("UTC0");
  }

  const int posixMinutes = -utcOffsetMinutes;
  const char sign = posixMinutes >= 0 ? '+' : '-';
  const int absMinutes = posixMinutes >= 0 ? posixMinutes : -posixMinutes;
  const int hours = absMinutes / 60;
  const int mins = absMinutes % 60;

  char buf[16];
  if (mins == 0) {
    snprintf(buf, sizeof(buf), "UTC%c%d", sign, hours);
  } else {
    snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, hours, mins);
  }
  return String(buf);
}

String normalizeTimezoneForPosix(const String &tz) {
  String trimmed = tz;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    return String(USER_TIMEZONE_TZ);
  }

  // ESP32/newlib TZ parser does not understand IANA names directly.
  if (trimmed.equalsIgnoreCase("Asia/Seoul")) {
    return String("KST-9");
  }
  if (trimmed.equalsIgnoreCase("Etc/UTC") ||
      trimmed.equalsIgnoreCase("UTC") ||
      trimmed.equalsIgnoreCase("GMT")) {
    return String("UTC0");
  }

  int utcOffsetMinutes = 0;
  if (parseUtcOffsetMinutes(trimmed, &utcOffsetMinutes)) {
    return posixTzFromUtcOffsetMinutes(utcOffsetMinutes);
  }

  // Assume caller already provided a POSIX TZ string.
  return trimmed;
}

bool extractUint64JsonField(const String &json, const char *key, uint64_t *valueOut) {
  if (!key || !valueOut) {
    return false;
  }

  String token = "\"";
  token += key;
  token += "\":";

  int idx = json.indexOf(token);
  if (idx < 0) {
    return false;
  }
  idx += token.length();

  while (idx < static_cast<int>(json.length())) {
    const char c = json[static_cast<unsigned int>(idx)];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++idx;
      continue;
    }
    break;
  }

  uint64_t value = 0;
  bool hasDigit = false;
  constexpr uint64_t kMaxBeforeMul10 = UINT64_MAX / 10ULL;
  constexpr uint64_t kMaxLastDigit = UINT64_MAX % 10ULL;

  while (idx < static_cast<int>(json.length())) {
    const char c = json[static_cast<unsigned int>(idx)];
    if (c < '0' || c > '9') {
      break;
    }
    hasDigit = true;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > kMaxBeforeMul10 || (value == kMaxBeforeMul10 && digit > kMaxLastDigit)) {
      return false;
    }
    value = value * 10ULL + digit;
    ++idx;
  }

  if (!hasDigit) {
    return false;
  }

  *valueOut = value;
  return true;
}

String ellipsize(const String &text, size_t maxLen) {
  if (maxLen < 4) {
    return text;
  }

  auto isUtf8ContinuationByte = [](uint8_t byte) -> bool {
    return (byte & 0xC0U) == 0x80U;
  };

  size_t glyphCount = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const uint8_t byte = static_cast<uint8_t>(text[static_cast<unsigned int>(i)]);
    if (!isUtf8ContinuationByte(byte)) {
      ++glyphCount;
    }
  }
  if (glyphCount <= maxLen) {
    return text;
  }

  const size_t keepGlyphs = maxLen - 3;
  size_t keepBytes = text.length();
  size_t seenGlyphs = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const uint8_t byte = static_cast<uint8_t>(text[static_cast<unsigned int>(i)]);
    if (!isUtf8ContinuationByte(byte)) {
      if (seenGlyphs == keepGlyphs) {
        keepBytes = i;
        break;
      }
      ++seenGlyphs;
    }
  }

  return text.substring(0, keepBytes) + "...";
}

LauncherIconId iconIdFromLauncherIndex(int index) {
  switch (wrapIndex(index, 4)) {
    case 0:
      return LauncherIconId::AppMarket;
    case 1:
      return LauncherIconId::Settings;
    case 2:
      return LauncherIconId::FileExplorer;
    case 3:
    default:
      return LauncherIconId::OpenClaw;
  }
}

}  // namespace

class UiRuntime::Impl {
 public:
  LvglPort port;
  InputAdapter input;

  String statusLine;
  UiLanguage language = UiLanguage::English;
  bool koreanFontInstalled = false;
  const lv_font_t *koreanFont = nullptr;
  String timezoneTz = USER_TIMEZONE_TZ;
  String timezonePosixTz = USER_TIMEZONE_TZ;

  String headerTime;
  String headerStatus;
  int batteryPct = -1;
  bool batteryCharging = false;
  bool batteryChargingKnown = false;
  bool batteryWireReady = false;
  uint8_t displayBrightnessPercent = clampBrightnessPercent(USER_DISPLAY_BRIGHTNESS_PERCENT);
  bool ntpStarted = false;
  bool launcherIconsAvailable = false;
  unsigned long lastNtpAttemptMs = 0;
  unsigned long lastUnixSyncAttemptMs = 0;
  unsigned long lastUnixSyncSuccessMs = 0;
  unsigned long lastBatteryPollMs = 0;
  unsigned long lastHeaderUpdateMs = 0;

  lv_obj_t *progressOverlay = nullptr;
  lv_obj_t *progressPanel = nullptr;
  lv_obj_t *progressTitle = nullptr;
  lv_obj_t *progressMessage = nullptr;
  lv_obj_t *progressSpinner = nullptr;
  lv_obj_t *progressBar = nullptr;
  lv_obj_t *progressPercent = nullptr;
  String textInputCacheTitle;
  String textInputCachePreview;
  std::vector<lv_area_t> textInputCacheAreas;
  std::vector<lv_obj_t *> textInputCacheButtons;
  std::vector<lv_obj_t *> textInputCacheLabels;
  std::vector<String> textInputCacheKeyLabels;
  int textInputCacheSelected = -1;
  int textInputCacheCapsIndex = -1;
  unsigned long textInputLastFullRenderMs = 0;
  bool serviceActive = false;
  unsigned long lastBackgroundTickMs = 0;

  bool begin() {
    if (!port.begin()) {
      return false;
    }

    applyBacklight();
    input.begin(port.display());
    applyTheme();
    launcherIconsAvailable = initLauncherIcons();
    return true;
  }

  void applyBacklight() {
    pinMode(boardpins::kTftBacklight, OUTPUT);
    analogWrite(boardpins::kTftBacklight, brightnessPwmFromPercent(displayBrightnessPercent));
  }

  void setDisplayBrightnessPercent(uint8_t percent) {
    displayBrightnessPercent = clampBrightnessPercent(percent);
    applyBacklight();
  }

  uint8_t getDisplayBrightnessPercent() const {
    return displayBrightnessPercent;
  }

  void applyTheme() {
    lv_theme_t *theme = lv_theme_default_init(port.display(),
                                              lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_BLUE_GREY),
                                              true,
                                              font());
    lv_display_set_theme(port.display(), theme);
  }

  const lv_font_t *font() const {
    if (koreanFontInstalled && koreanFont) {
      return koreanFont;
    }
    return &lv_font_montserrat_14;
  }

  void service(const std::function<void()> *backgroundTick = nullptr) {
    if (serviceActive) {
      return;
    }

    serviceActive = true;
    const unsigned long startMs = millis();
    input.tick();
    port.pump();
    const unsigned long now = millis();
    if (backgroundTick && *backgroundTick &&
        (lastBackgroundTickMs == 0 ||
         now - lastBackgroundTickMs >= kBackgroundTickMinIntervalMs)) {
      lastBackgroundTickMs = now;
      (*backgroundTick)();
    }
    // Poll once more after background work so short pulses are less likely
    // to be missed when Wi-Fi/gateway ticks temporarily block.
    input.tick();
    port.pump();
#if USER_INPUT_TRACE_ENABLED
    const unsigned long elapsedMs = millis() - startMs;
    if (elapsedMs >= 40UL) {
      Serial.printf("[ui] service stall: %lu ms\n", elapsedMs);
    }
#endif
    serviceActive = false;
  }

  UiEvent pollInput() {
    const InputEvent ev = input.pollEvent();
    UiEvent out;
    out.delta = ev.delta;
    out.ok = ev.ok || ev.okCount > 0;
    out.back = ev.back || ev.backCount > 0;
    out.okLong = ev.okLong || ev.okLongCount > 0;
    out.okCount = ev.okCount;
    out.backCount = ev.backCount;
    out.okLongCount = ev.okLongCount;
    return out;
  }

  bool ensureBatteryI2cReady() {
#if USER_BATTERY_GAUGE_ENABLED
    if (!batteryWireReady) {
      Wire.begin(USER_BATTERY_GAUGE_SDA, USER_BATTERY_GAUGE_SCL);
      Wire.setTimeOut(5);
      batteryWireReady = true;
    }
    return true;
#else
    return false;
#endif
  }

  int readBatteryPercent() {
#if USER_BATTERY_GAUGE_ENABLED
    if (!ensureBatteryI2cReady()) {
      return -1;
    }

    Wire.beginTransmission(USER_BATTERY_GAUGE_ADDR);
    Wire.write(USER_BATTERY_GAUGE_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      return -1;
    }

    const int readCount = Wire.requestFrom(static_cast<int>(USER_BATTERY_GAUGE_ADDR), 2);
    if (readCount < 2) {
      return -1;
    }

    const uint8_t lo = static_cast<uint8_t>(Wire.read());
    const uint8_t hi = static_cast<uint8_t>(Wire.read());
    const int pct = (static_cast<int>(hi) << 8) | static_cast<int>(lo);
    if (pct < 0 || pct > 100) {
      return -1;
    }
    return pct;
#else
    return -1;
#endif
  }

  bool readBatteryCharging(bool *known = nullptr) {
#if USER_BATTERY_GAUGE_ENABLED
    if (known) {
      *known = false;
    }

    if (!ensureBatteryI2cReady()) {
      return false;
    }

    constexpr uint8_t kPmuAddr = 0x6B;   // BQ25896
    constexpr uint8_t kStatusReg = 0x0B; // CHRG_STAT[4:3]

    Wire.beginTransmission(kPmuAddr);
    Wire.write(kStatusReg);
    if (Wire.endTransmission(false) != 0) {
      return false;
    }

    const int readCount = Wire.requestFrom(static_cast<int>(kPmuAddr), 1);
    if (readCount < 1) {
      return false;
    }

    const uint8_t status = static_cast<uint8_t>(Wire.read());
    const uint8_t chargeState = static_cast<uint8_t>((status >> 3) & 0x03);
    if (known) {
      *known = true;
    }
    return chargeState == 1U || chargeState == 2U;
#else
    if (known) {
      *known = false;
    }
    return false;
#endif
  }

  void updateHeaderIndicators() {
    const unsigned long now = millis();
    if (now - lastHeaderUpdateMs < kHeaderRefreshMs) {
      return;
    }
    lastHeaderUpdateMs = now;

    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected && !ntpStarted) {
      const char *tz = timezonePosixTz.isEmpty() ? USER_TIMEZONE_TZ : timezonePosixTz.c_str();
      configTzTime(tz, USER_NTP_SERVER_1, USER_NTP_SERVER_2);
      ntpStarted = true;
      lastNtpAttemptMs = now;
    }

    time_t unixNow = time(nullptr);
    bool unixValid = isValidUnixTime(unixNow);
    if (wifiConnected && ntpStarted && !unixValid &&
        now - lastNtpAttemptMs >= kNtpRetryMs) {
      const char *tz = timezonePosixTz.isEmpty() ? USER_TIMEZONE_TZ : timezonePosixTz.c_str();
      configTzTime(tz, USER_NTP_SERVER_1, USER_NTP_SERVER_2);
      lastNtpAttemptMs = now;
    }

    const bool unixSyncDue =
        wifiConnected &&
        (lastUnixSyncAttemptMs == 0 ||
         (!unixValid && (now - lastUnixSyncAttemptMs >= kUnixSyncRetryMs)) ||
         (unixValid && (lastUnixSyncSuccessMs == 0 ||
                        now - lastUnixSyncSuccessMs >= kUnixSyncRefreshMs)));
    if (unixSyncDue) {
      lastUnixSyncAttemptMs = now;
      if (syncUnixTimeFromServer(nullptr)) {
        lastUnixSyncSuccessMs = now;
        unixNow = time(nullptr);
        unixValid = isValidUnixTime(unixNow);
      }
    }

    if (unixValid) {
      struct tm timeInfo;
      if (localtime_r(&unixNow, &timeInfo) != nullptr) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min);
        headerTime = String(buf);
      } else {
        headerTime = "--:--";
      }
    } else {
      headerTime = "--:--";
    }

    if (now - lastBatteryPollMs >= kBatteryPollMs || batteryPct < 0) {
      lastBatteryPollMs = now;
      batteryPct = readBatteryPercent();
      batteryCharging = readBatteryCharging(&batteryChargingKnown);
    }

    String status = "W:";
    if (WiFi.status() == WL_CONNECTED) {
      status += String(WiFi.RSSI());
    } else {
      status += "--";
    }
    status += " B:";
    if (batteryPct >= 0) {
      status += String(batteryPct);
      status += "%";
    } else {
      status += "--";
    }
    headerStatus = status;
  }

  void setTimezone(const String &tz) {
    String next = tz;
    next.trim();
    if (next.isEmpty()) {
      next = USER_TIMEZONE_TZ;
    }
    timezoneTz = next;
    timezonePosixTz = normalizeTimezoneForPosix(next);
    setenv("TZ", timezonePosixTz.c_str(), 1);
    tzset();
    ntpStarted = false;
    lastNtpAttemptMs = 0;
    lastUnixSyncAttemptMs = 0;
    lastUnixSyncSuccessMs = 0;
  }

  String timezone() const {
    if (timezoneTz.isEmpty()) {
      return String(USER_TIMEZONE_TZ);
    }
    return timezoneTz;
  }

  bool syncTimezoneFromIp(String *resolvedTz, String *error) {
    if (error) {
      error->clear();
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (error) {
        *error = "Wi-Fi not connected";
      }
      return false;
    }
    if (!hasTlsHeapHeadroomUi()) {
      if (error) {
        *error = "Low heap (skip timezone sync)";
      }
      return false;
    }

    WiFiClient client;

    HTTPClient http;
    http.setConnectTimeout(3500);
    http.setTimeout(4500);

    const char *url = "https://ipwho.is/?fields=success,timezone,message";
    if (!http.begin(client, url)) {
      if (error) {
        *error = "HTTP begin failed";
      }
      return false;
    }

    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
      if (error) {
        *error = "IP lookup failed (" + String(statusCode) + ")";
      }
      http.end();
      return false;
    }

    const String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(768);
    const auto jsonErr = deserializeJson(doc, payload);
    if (jsonErr) {
      if (error) {
        *error = "IP lookup parse failed";
      }
      return false;
    }

    const bool success = doc["success"] | false;
    if (!success) {
      if (error) {
        const char *msg = doc["message"] | "IP lookup rejected";
        *error = String(msg);
      }
      return false;
    }

    const JsonVariantConst timezoneNode = doc["timezone"];
    const char *zone = "";
    if (timezoneNode.is<JsonObjectConst>()) {
      zone = timezoneNode["id"] | "";
    } else {
      zone = timezoneNode | "";
    }
    String tz = String(zone);
    tz.trim();
    if (tz.isEmpty()) {
      if (error) {
        *error = "Timezone not found from IP";
      }
      return false;
    }

    setTimezone(tz);
    configTzTime(timezonePosixTz.c_str(), USER_NTP_SERVER_1, USER_NTP_SERVER_2);
    ntpStarted = true;
    lastNtpAttemptMs = millis();

    if (resolvedTz) {
      *resolvedTz = timezoneTz;
    }
    return true;
  }

  bool syncUnixTimeFromServer(String *error) {
    if (error) {
      error->clear();
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (error) {
        *error = "Wi-Fi not connected";
      }
      return false;
    }
    if (!hasTlsHeapHeadroomUi()) {
      if (error) {
        *error = "Low heap (skip time sync)";
      }
      return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(1800);
    http.setTimeout(2200);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    String url = USER_UNIX_TIME_SERVER_URL;
    if (url.startsWith("https://")) {
      url.remove(0, 8);
      url = "http://" + url;
    }

    if (!http.begin(client, url)) {
      if (error) {
        *error = "Time server begin failed";
      }
      return false;
    }

    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
      if (error) {
        *error = "Time server failed (" + String(statusCode) + ")";
      }
      http.end();
      return false;
    }

    const String payload = http.getString();
    http.end();

    uint64_t unixSec64 = 0;
    uint64_t fileTime100Ns = 0;
    if (extractUint64JsonField(payload, "currentFileTime", &fileTime100Ns)) {
      if (fileTime100Ns <= kWindowsEpochOffset100Ns) {
        if (error) {
          *error = "Time field invalid";
        }
        return false;
      }
      unixSec64 = (fileTime100Ns - kWindowsEpochOffset100Ns) / kHundredNsPerSecond;
    } else if (!extractUint64JsonField(payload, "unixtime", &unixSec64) &&
               !extractUint64JsonField(payload, "unixTime", &unixSec64) &&
               !extractUint64JsonField(payload, "unix", &unixSec64)) {
      if (error) {
        *error = "Time field missing";
      }
      return false;
    }

    const time_t unixSec = static_cast<time_t>(unixSec64);
    if (!isValidUnixTime(unixSec)) {
      if (error) {
        *error = "Unix time invalid";
      }
      return false;
    }

    timeval tv;
    tv.tv_sec = unixSec;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
      if (error) {
        *error = "settimeofday failed";
      }
      return false;
    }

    return true;
  }

  void drawBatteryIcon(lv_obj_t *parent, int x, int y) const {
    constexpr int bodyW = 18;
    constexpr int bodyH = 9;
    constexpr int capW = 2;
    constexpr int capH = 4;
    constexpr int segCount = 4;

    const uint32_t batteryColor =
        (batteryChargingKnown && batteryCharging) ? kLauncherCharging : kLauncherPrimary;

    lv_obj_t *body = lv_obj_create(parent);
    disableScroll(body);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, x, y);
    lv_obj_set_size(body, bodyW, bodyH);
    lv_obj_set_style_radius(body, 1, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(batteryColor), 0);

    lv_obj_t *cap = lv_obj_create(parent);
    disableScroll(cap);
    lv_obj_remove_style_all(cap);
    lv_obj_set_pos(cap, x + bodyW, y + ((bodyH - capH) / 2));
    lv_obj_set_size(cap, capW, capH);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(batteryColor), 0);

    int filled = 0;
    if (batteryPct >= 0) {
      filled = (batteryPct + 24) / 25;
      if (filled < 0) {
        filled = 0;
      }
      if (filled > segCount) {
        filled = segCount;
      }
    }

    const int usableW = bodyW - 4;
    const int segGap = 1;
    const int segW = (usableW - ((segCount - 1) * segGap)) / segCount;
    const int segH = bodyH - 4;
    for (int i = 0; i < segCount; ++i) {
      lv_obj_t *seg = lv_obj_create(body);
      disableScroll(seg);
      lv_obj_remove_style_all(seg);
      lv_obj_set_pos(seg, 2 + i * (segW + segGap), 2);
      lv_obj_set_size(seg, segW, segH);
      lv_obj_set_style_radius(seg, 1, 0);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
      if (i < filled) {
        lv_obj_set_style_bg_color(seg, lv_color_hex(batteryColor), 0);
      } else {
        lv_obj_set_style_bg_color(seg, lv_color_hex(kLauncherLine), 0);
      }
    }
  }

  void setLabelFont(lv_obj_t *obj) const {
    lv_obj_set_style_text_font(obj, font(), kStyleAny);
    lv_obj_set_style_text_opa(obj, LV_OPA_COVER, kStyleAny);
    lv_obj_set_style_text_color(obj, lv_color_white(), kStyleAny);
  }

  void disableScroll(lv_obj_t *obj) const {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
  }

  void prepareLabel(lv_obj_t *label) const {
    lv_obj_remove_style_all(label);
    disableScroll(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  }

  void setSingleLineLabel(lv_obj_t *label,
                          int width,
                          lv_text_align_t align = LV_TEXT_ALIGN_LEFT) const {
    prepareLabel(label);
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, static_cast<int32_t>(font()->line_height + 2));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, kStyleAny);
    lv_obj_set_style_pad_all(label, 0, kStyleAny);
  }

  void setWrapLabel(lv_obj_t *label, int width, int height = -1) const {
    prepareLabel(label);
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    if (height > 0) {
      lv_obj_set_height(label, height);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, kStyleAny);
    lv_obj_set_style_pad_all(label, 0, kStyleAny);
  }

  void clearProgressHandles() {
    progressOverlay = nullptr;
    progressPanel = nullptr;
    progressTitle = nullptr;
    progressMessage = nullptr;
    progressSpinner = nullptr;
    progressBar = nullptr;
    progressPercent = nullptr;
  }

  void clearTextInputHandles() {
    textInputCacheTitle = "";
    textInputCachePreview = "";
    textInputCacheAreas.clear();
    textInputCacheButtons.clear();
    textInputCacheLabels.clear();
    textInputCacheKeyLabels.clear();
    textInputCacheSelected = -1;
    textInputCacheCapsIndex = -1;
    textInputLastFullRenderMs = 0;
  }

  bool textInputLayoutMatches(const std::vector<lv_area_t> &areas) const {
    if (textInputCacheAreas.size() != areas.size()) {
      return false;
    }
    for (size_t i = 0; i < areas.size(); ++i) {
      const lv_area_t &cached = textInputCacheAreas[i];
      const lv_area_t &next = areas[i];
      if (cached.x1 != next.x1 ||
          cached.y1 != next.y1 ||
          cached.x2 != next.x2 ||
          cached.y2 != next.y2) {
        return false;
      }
    }
    return true;
  }

  bool textInputWidgetsValid() const {
    if (textInputCacheButtons.size() != textInputCacheLabels.size()) {
      return false;
    }
    for (size_t i = 0; i < textInputCacheButtons.size(); ++i) {
      if (!textInputCacheButtons[i] ||
          !textInputCacheLabels[i] ||
          !lv_obj_is_valid(textInputCacheButtons[i]) ||
          !lv_obj_is_valid(textInputCacheLabels[i])) {
        return false;
      }
    }
    return true;
  }

  void renderBase(const String &title,
                  const String &subtitle,
                  const String &footer,
                  int &contentTop,
                  int &contentBottom) {
    updateHeaderIndicators();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    clearTextInputHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kClrBg), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());
    const int frameX = 4;
    const int frameW = w - (frameX * 2);
    const int innerW = frameW - (kSidePadding * 2);

    lv_obj_t *header = lv_obj_create(screen);
    disableScroll(header);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, frameX, 4);
    lv_obj_set_size(header, frameW, kHeaderHeight);
    lv_obj_set_style_radius(header, 8, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kClrPanel), 0);
    lv_obj_set_style_bg_opa(header, kOpa90, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(kClrBorder), 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    int timeWidth = 54;
    if (timeWidth > innerW - 24) {
      timeWidth = innerW - 24;
    }
    if (timeWidth < 18) {
      timeWidth = 18;
    }
    int titleWidth = innerW - timeWidth - 6;
    if (titleWidth < 20) {
      titleWidth = innerW;
    }

    lv_obj_t *titleLabel = lv_label_create(header);
    setSingleLineLabel(titleLabel, titleWidth, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, title.c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_set_pos(titleLabel, kSidePadding, 4);

    lv_obj_t *timeLabel = lv_label_create(header);
    setSingleLineLabel(timeLabel, timeWidth, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(timeLabel, headerTime.length() > 0 ? headerTime.c_str() : "--:--");
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(kClrTextMuted), 0);
    lv_obj_set_pos(timeLabel, frameW - kSidePadding - timeWidth, 4);

    int y = 4 + kHeaderHeight + 4;
    if (subtitle.length() > 0) {
      lv_obj_t *sub = lv_obj_create(screen);
      disableScroll(sub);
      lv_obj_remove_style_all(sub);
      lv_obj_set_pos(sub, frameX, y);
      lv_obj_set_size(sub, frameW, kSubtitleHeight);
      lv_obj_set_style_radius(sub, 6, 0);
      lv_obj_set_style_bg_color(sub, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_opa(sub, kOpa85, 0);
      lv_obj_set_style_border_width(sub, 1, 0);
      lv_obj_set_style_border_color(sub, lv_color_hex(kClrBorder), 0);

      lv_obj_t *subLabel = lv_label_create(sub);
      setSingleLineLabel(subLabel, innerW, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(subLabel, subtitle.c_str());
      lv_obj_set_style_text_color(subLabel, lv_color_hex(kClrTextMuted), 0);
      int subLabelY = (kSubtitleHeight - static_cast<int>(font()->line_height + 2)) / 2;
      if (subLabelY < 0) {
        subLabelY = 0;
      }
      lv_obj_set_pos(subLabel, kSidePadding, subLabelY);
      y += kSubtitleHeight + 4;
    }

    int footerY = h - 6;
    if (footer.length() > 0) {
      footerY = h - kFooterHeight - 4;
      lv_obj_t *foot = lv_obj_create(screen);
      disableScroll(foot);
      lv_obj_remove_style_all(foot);
      lv_obj_set_pos(foot, frameX, footerY);
      lv_obj_set_size(foot, frameW, kFooterHeight);
      lv_obj_set_style_radius(foot, 6, 0);
      lv_obj_set_style_bg_color(foot, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_opa(foot, kOpa85, 0);
      lv_obj_set_style_border_width(foot, 1, 0);
      lv_obj_set_style_border_color(foot, lv_color_hex(kClrBorder), 0);

      lv_obj_t *footLabel = lv_label_create(foot);
      setSingleLineLabel(footLabel, innerW, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(footLabel, footer.c_str());
      lv_obj_set_style_text_color(footLabel, lv_color_hex(kClrTextMuted), 0);
      int footLabelY = (kFooterHeight - static_cast<int>(font()->line_height + 2)) / 2;
      if (footLabelY < 0) {
        footLabelY = 0;
      }
      lv_obj_set_pos(footLabel, kSidePadding, footLabelY);
    }

    contentTop = y + 2;
    contentBottom = footer.length() > 0 ? (footerY - 4) : (h - 6);
    if (contentBottom > h - 6) {
      contentBottom = h - 6;
    }
    if (contentBottom < contentTop + kMinContentHeight) {
      contentBottom = contentTop + kMinContentHeight;
      if (contentBottom > h - 6) {
        contentBottom = h - 6;
      }
    }
    if (contentBottom < contentTop) {
      contentBottom = contentTop;
    }
  }

  void renderMenu(const String &title,
                  const std::vector<String> &items,
                  int selected,
                  const String &subtitle,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, subtitle, footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 18 && usableHeight >= 18) {
      rowHeight = 18;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

    int start = selected - (maxRows / 2);
    if (start < 0) {
      start = 0;
    }
    if (start + maxRows > static_cast<int>(items.size())) {
      start = static_cast<int>(items.size()) - maxRows;
      if (start < 0) {
        start = 0;
      }
    }

    for (int row = 0; row < maxRows; ++row) {
      const int index = start + row;
      const int y = contentTop + row * rowHeight;
      if (index < 0 || index >= static_cast<int>(items.size())) {
        continue;
      }

      lv_obj_t *btn = lv_obj_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_remove_style_all(btn);
      const int btnW = w - 20;
      int btnH = rowHeight - 2;
      if (btnH < 1) {
        btnH = 1;
      }
      lv_obj_set_pos(btn, 10, y);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_style_radius(btn, 8, kStyleAny);
      lv_obj_set_style_border_width(btn, 1, kStyleAny);
      lv_obj_set_style_pad_all(btn, 0, kStyleAny);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, kStyleAny);

      const bool isSelected = index == selected;
      lv_obj_set_style_bg_color(btn,
                                isSelected ? lv_color_hex(kClrAccentSoft) : lv_color_hex(kClrPanel),
                                kStyleAny);
      lv_obj_set_style_border_color(btn,
                                    isSelected ? lv_color_hex(kClrAccent) : lv_color_hex(kClrBorder),
                                kStyleAny);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label, btnW - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, items[static_cast<size_t>(index)].c_str());
      lv_obj_set_style_text_color(label,
                                  isSelected ? lv_color_hex(kClrTextPrimary)
                                             : lv_color_hex(kClrTextPrimary),
                                  kStyleAny);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

      if (isSelected) {
        lv_obj_t *marker = lv_obj_create(btn);
        disableScroll(marker);
        lv_obj_remove_style_all(marker);
        lv_obj_set_size(marker, 3, btnH - 8);
        lv_obj_set_pos(marker, 4, 4);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kClrAccent), 0);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
      }
    }

    service(nullptr);
  }

  int renderMessengerHome(const std::vector<String> &previewLines,
                          int focus,
                          bool scrollMode,
                          int scrollOffsetLines) {
    int contentTop = 0;
    int contentBottom = 0;
    const String footer = scrollMode
                              ? String("ROT Scroll  OK Done  BACK Done")
                              : String("ROT Move  OK Select  BACK Exit");
    renderBase("Messenger", "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int contentH = contentBottom - contentTop + 1;
    if (contentH < 1) {
      contentH = 1;
    }

    int buttonH = 28;
    if (buttonH > contentH / 2) {
      buttonH = contentH / 2;
    }
    if (buttonH < 18) {
      buttonH = 18;
    }

    const int sectionGap = 4;
    int boxAvailH = contentH - buttonH - sectionGap;
    if (boxAvailH < 24) {
      boxAvailH = 24;
    }

    const int boxX = 4;
    const int boxW = w - 8;
    const int boxY = contentTop;
    int boxH = boxAvailH;
    int buttonY = boxY + boxH + sectionGap;
    if (buttonY + buttonH - 1 > contentBottom) {
      buttonY = contentBottom - buttonH + 1;
    }
    if (buttonY < contentTop) {
      buttonY = contentTop;
    }
    boxH = buttonY - sectionGap - boxY;
    if (boxH < 1) {
      boxH = 1;
    }

    lv_obj_t *box = lv_obj_create(lv_screen_active());
    disableScroll(box);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, boxX, boxY);
    lv_obj_set_size(box, boxW, boxH);
    const int safeFocus = wrapIndex(focus, 4);
    const bool boxSelected = safeFocus == 0;
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_bg_color(box,
                              boxSelected ? lv_color_hex(kClrPanel) : lv_color_hex(kClrPanelSoft),
                              0);
    lv_obj_set_style_bg_opa(box, kOpa85, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box,
                                  boxSelected ? lv_color_hex(kClrAccent)
                                              : lv_color_hex(kClrBorder),
                                  0);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_set_style_outline_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    String messageText;
    if (previewLines.empty()) {
      messageText = "(no messages)";
    } else {
      for (size_t i = 0; i < previewLines.size(); ++i) {
        if (i > 0) {
          messageText += "\n";
        }
        messageText += previewLines[i];
      }
    }

    lv_obj_t *messageLabel = lv_label_create(box);
    setWrapLabel(messageLabel, boxW - 12);
    lv_label_set_text(messageLabel, messageText.c_str());
    lv_obj_set_style_text_color(messageLabel, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_set_pos(messageLabel, 6, 6);

    lv_obj_update_layout(box);
    int viewportH = boxH - 12;
    if (viewportH < 1) {
      viewportH = 1;
    }
    int contentTextH = lv_obj_get_height(messageLabel);
    if (contentTextH < 0) {
      contentTextH = 0;
    }
    int lineStep = static_cast<int>(font()->line_height + 2);
    if (lineStep < 1) {
      lineStep = 1;
    }
    const int maxScrollPx = contentTextH > viewportH ? contentTextH - viewportH : 0;
    int maxScrollLines = 0;
    if (maxScrollPx > 0) {
      maxScrollLines = (maxScrollPx + lineStep - 1) / lineStep;
    }
    int clampedScrollLines = scrollOffsetLines;
    if (clampedScrollLines < 0) {
      clampedScrollLines = 0;
    }
    if (clampedScrollLines > maxScrollLines) {
      clampedScrollLines = maxScrollLines;
    }
    int scrollPx = clampedScrollLines * lineStep;
    if (scrollPx > maxScrollPx) {
      scrollPx = maxScrollPx;
    }
    lv_obj_set_pos(messageLabel, 6, 6 - scrollPx);

    if (boxSelected && scrollMode) {
      lv_obj_t *modeLabel = lv_label_create(box);
      setSingleLineLabel(modeLabel, boxW - 14, LV_TEXT_ALIGN_RIGHT);
      lv_label_set_text(modeLabel, "SCROLL");
      lv_obj_set_style_text_color(modeLabel, lv_color_hex(kClrAccent), 0);
      lv_obj_align(modeLabel, LV_ALIGN_TOP_RIGHT, -6, 2);
    }

    const int kButtonCount = 3;
    const char *buttonLabels[kButtonCount] = {"Text", "Voice", "File"};
    const int safeSelected = safeFocus;

    int rowX = 10;
    int rowW = w - 20;
    const int buttonGap = 6;
    int buttonW = (rowW - (buttonGap * (kButtonCount - 1))) / kButtonCount;
    if (buttonW < 24) {
      rowX = 4;
      rowW = w - 8;
      buttonW = (rowW - (buttonGap * (kButtonCount - 1))) / kButtonCount;
      if (buttonW < 20) {
        buttonW = 20;
      }
    }

    for (int i = 0; i < kButtonCount; ++i) {
      lv_obj_t *btn = lv_obj_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_remove_style_all(btn);
      const int btnX = rowX + i * (buttonW + buttonGap);
      lv_obj_set_pos(btn, btnX, buttonY);
      lv_obj_set_size(btn, buttonW, buttonH);
      lv_obj_set_style_radius(btn, 6, 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, 0);
      lv_obj_set_style_pad_all(btn, 0, 0);
      lv_obj_set_style_bg_opa(btn, kOpa85, 0);
      lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_outline_width(btn, 0, 0);
      lv_obj_set_style_outline_opa(btn, LV_OPA_TRANSP, 0);

      const bool isSelected = (i + 1) == safeSelected;
      lv_obj_set_style_bg_color(btn,
                                isSelected ? lv_color_hex(kClrPanel) : lv_color_hex(kClrPanelSoft),
                                0);
      lv_obj_set_style_border_color(btn,
                                    isSelected ? lv_color_hex(kClrAccent) : lv_color_hex(kClrBorder),
                                    0);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label, buttonW, LV_TEXT_ALIGN_CENTER);
      lv_obj_set_width(label, LV_PCT(100));
      lv_obj_set_height(label, static_cast<int>(font()->line_height + 2));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_text(label, buttonLabels[i]);
      lv_obj_set_style_text_color(label, lv_color_hex(kClrTextPrimary), kStyleAny);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }

    service(nullptr);
    return maxScrollLines;
  }

  void renderLauncher(const String &title,
                      const std::vector<String> &items,
                      int selected) {
    updateHeaderIndicators();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kLauncherBg), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());
    const int count = static_cast<int>(items.size());
    if (count <= 0) {
      lv_obj_t *emptyLabel = lv_label_create(screen);
      setSingleLineLabel(emptyLabel, w - 12, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(emptyLabel, "No apps");
      lv_obj_set_style_text_color(emptyLabel, lv_color_hex(kLauncherMuted), 0);
      lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, 0);
      service(nullptr);
      return;
    }

    const int safeSelected = wrapIndex(selected, count);
    const int prevIndex = wrapIndex(safeSelected - 1, count);
    const int nextIndex = wrapIndex(safeSelected + 1, count);

    const int topX = 4;
    const int topY = 4;
    const int topW = w - (topX * 2);
    const int topH = kHeaderHeight;

    lv_obj_t *topBar = lv_obj_create(screen);
    disableScroll(topBar);
    lv_obj_remove_style_all(topBar);
    lv_obj_set_pos(topBar, topX, topY);
    lv_obj_set_size(topBar, topW, topH);
    lv_obj_set_style_radius(topBar, 8, 0);
    lv_obj_set_style_bg_color(topBar, lv_color_hex(kLauncherBg), 0);
    lv_obj_set_style_bg_opa(topBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topBar, 1, 0);
    lv_obj_set_style_border_color(topBar, lv_color_hex(kLauncherLine), 0);

    constexpr int kBatteryBodyW = 18;
    constexpr int kBatteryCapW = 2;
    constexpr int kBatteryIconW = kBatteryBodyW + kBatteryCapW;
    constexpr int kBatteryIconH = 9;
    const int batteryX = topW - kSidePadding - kBatteryIconW;
    const int batteryY = (topH - kBatteryIconH) / 2;
    const int timeLabelW = 50;
    const int timeBatteryGap = 12;
    const int timeX = batteryX - timeBatteryGap - timeLabelW;
    const int titleX = kSidePadding;
    int titleW = timeX - titleX - 6;
    if (titleW < 16) {
      titleW = 16;
    }
    int labelY = (topH - static_cast<int>(font()->line_height + 2)) / 2;
    if (labelY < 0) {
      labelY = 0;
    }
    lv_obj_t *titleLabel = lv_label_create(topBar);
    setSingleLineLabel(titleLabel, titleW > 10 ? titleW : 10, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, ellipsize(title, 18).c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_pos(titleLabel, titleX, labelY);

    const String timeText = headerTime.length() > 0 ? headerTime : "--:--";
    lv_obj_t *timeLabel = lv_label_create(topBar);
    setSingleLineLabel(timeLabel, timeLabelW, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(timeLabel, timeText.c_str());
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_pos(timeLabel, timeX, labelY);

    drawBatteryIcon(topBar, batteryX, batteryY);

    const String selectedName = ellipsize(items[static_cast<size_t>(safeSelected)], 18);
    const String prevName = ellipsize(items[static_cast<size_t>(prevIndex)], 10);
    const String nextName = ellipsize(items[static_cast<size_t>(nextIndex)], 10);

    constexpr int kMainIconOffsetY = -6;
    const int mainIconRenderH = launcherIconRenderSize(LauncherIconVariant::Main);
    bool iconDrawn = launcherIconsAvailable && launcherIconsReady();
    if (iconDrawn) {
      const LauncherIconId selectedIcon = iconIdFromLauncherIndex(safeSelected);
      const LauncherIconId prevIcon = iconIdFromLauncherIndex(prevIndex);
      const LauncherIconId nextIcon = iconIdFromLauncherIndex(nextIndex);

      lv_obj_t *centerIcon = createLauncherIcon(
          screen, selectedIcon, LauncherIconVariant::Main, lv_color_hex(kLauncherPrimary));
      lv_obj_t *leftIcon = createLauncherIcon(
          screen, prevIcon, LauncherIconVariant::Side, lv_color_hex(kLauncherSide));
      lv_obj_t *rightIcon = createLauncherIcon(
          screen, nextIcon, LauncherIconVariant::Side, lv_color_hex(kLauncherSide));
      if (!centerIcon || !leftIcon || !rightIcon) {
        iconDrawn = false;
      } else {
        disableScroll(centerIcon);
        lv_obj_align(centerIcon, LV_ALIGN_CENTER, 0, kMainIconOffsetY);

        disableScroll(leftIcon);
        lv_obj_align(leftIcon, LV_ALIGN_CENTER, -92, kMainIconOffsetY);

        disableScroll(rightIcon);
        lv_obj_align(rightIcon, LV_ALIGN_CENTER, 92, kMainIconOffsetY);
      }
    }

    if (!iconDrawn) {
      lv_obj_t *fallback = lv_label_create(screen);
      setSingleLineLabel(fallback, w - 16, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(fallback, selectedName.c_str());
      lv_obj_set_style_text_color(fallback, lv_color_hex(kLauncherPrimary), 0);
      lv_obj_align(fallback, LV_ALIGN_CENTER, 0, -6);

      lv_obj_t *sideNames = lv_label_create(screen);
      setSingleLineLabel(sideNames, w - 16, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(sideNames, (prevName + "   |   " + nextName).c_str());
      lv_obj_set_style_text_color(sideNames, lv_color_hex(kLauncherMuted), 0);
      lv_obj_align(sideNames, LV_ALIGN_CENTER, 0, 16);
    }

    lv_obj_t *nameLabel = lv_label_create(screen);
    prepareLabel(nameLabel);
    setLabelFont(nameLabel);
    lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(nameLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(nameLabel, static_cast<int32_t>(font()->line_height + 2));
    lv_label_set_text(nameLabel, selectedName.c_str());
    lv_obj_set_style_text_color(nameLabel, lv_color_hex(kLauncherMuted), 0);
    int nameY = (h / 2) + 42;
    if (iconDrawn) {
      const int iconBottom = (h / 2) + kMainIconOffsetY + (mainIconRenderH / 2);
      nameY = iconBottom + 4;
    }
    if (nameY > h - 16) {
      nameY = h - 16;
    }
    lv_obj_align(nameLabel, LV_ALIGN_TOP_MID, 0, nameY);

    service(nullptr);
  }

  void renderInfo(const String &title,
                  const std::vector<String> &lines,
                  int start,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 18 && usableHeight >= 18) {
      rowHeight = 18;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

    for (int row = 0; row < maxRows; ++row) {
      const int lineIndex = start + row;
      const int y = contentTop + row * rowHeight;
      if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) {
        continue;
      }

      lv_obj_t *holder = lv_obj_create(lv_screen_active());
      disableScroll(holder);
      lv_obj_remove_style_all(holder);
      const int holderW = w - 20;
      int holderH = rowHeight - 1;
      if (holderH < 1) {
        holderH = 1;
      }
      lv_obj_set_pos(holder, 10, y);
      lv_obj_set_size(holder, holderW, holderH);
      lv_obj_set_style_bg_color(holder, lv_color_hex(kClrPanel), kStyleAny);
      lv_obj_set_style_bg_opa(holder, kOpa92, kStyleAny);
      lv_obj_set_style_border_width(holder, 1, kStyleAny);
      lv_obj_set_style_border_color(holder, lv_color_hex(kClrBorder), kStyleAny);
      lv_obj_set_style_radius(holder, 8, kStyleAny);
      lv_obj_set_style_pad_all(holder, 0, kStyleAny);

      lv_obj_t *label = lv_label_create(holder);
      setSingleLineLabel(label, holderW - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, lines[static_cast<size_t>(lineIndex)].c_str());
      lv_obj_set_style_text_color(label, lv_color_hex(kClrTextPrimary), kStyleAny);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    }

    service(nullptr);
  }

  void renderToast(const String &title,
                   const String &message,
                   const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int areaH = contentBottom - contentTop + 1;
    if (areaH < 1) {
      areaH = 1;
    }

    lv_obj_t *box = lv_obj_create(lv_screen_active());
    disableScroll(box);
    lv_obj_remove_style_all(box);
    int boxW = w - 16;
    if (boxW < 80) {
      boxW = w - 4;
    }
    int boxH = areaH - 8;
    if (boxH < 24) {
      boxH = areaH;
    }
    int boxY = contentTop + (areaH - boxH) / 2;
    lv_obj_set_size(box, boxW, boxH);
    lv_obj_set_pos(box, (w - boxW) / 2, boxY);
    lv_obj_set_style_bg_color(box, lv_color_hex(kClrPanel), kStyleAny);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, kStyleAny);
    lv_obj_set_style_border_color(box, lv_color_hex(kClrAccent), kStyleAny);
    lv_obj_set_style_border_width(box, 1, kStyleAny);
    lv_obj_set_style_radius(box, 10, kStyleAny);
    lv_obj_set_style_pad_all(box, 6, kStyleAny);

    lv_obj_t *label = lv_label_create(box);
    setWrapLabel(label, boxW - 18, boxH - 14);
    lv_label_set_text(label, message.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_center(label);

    service(nullptr);
  }

  void renderNumberWheel(const String &title,
                         int selectedValue,
                         int minValue,
                         int maxValue,
                         int step,
                         const String &suffix,
                         const String &footer) {
    String subtitle = title;
    subtitle += ": ";
    subtitle += String(static_cast<long>(selectedValue));
    subtitle += suffix;

    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, subtitle, footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int areaH = contentBottom - contentTop + 1;
    if (areaH < 1) {
      areaH = 1;
    }

    int panelW = w - 26;
    if (panelW < 96) {
      panelW = w - 8;
    }
    if (panelW < 16) {
      panelW = 16;
    }

    int panelH = areaH - 4;
    if (panelH < 44) {
      panelH = 44;
    }
    if (panelH > areaH) {
      panelH = areaH;
    }
    if (panelH < 1) {
      panelH = 1;
    }

    const int panelX = (w - panelW) / 2;
    int panelY = contentTop + (areaH - panelH) / 2;
    if (panelY < contentTop) {
      panelY = contentTop;
    }

    lv_obj_t *panel = lv_obj_create(lv_screen_active());
    disableScroll(panel);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, panelX, panelY);
    lv_obj_set_size(panel, panelW, panelH);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kClrPanel), 0);
    lv_obj_set_style_bg_opa(panel, kOpa92, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kClrBorder), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    int rowHeight = panelH / 5;
    if (rowHeight < 10) {
      rowHeight = 10;
    }
    int focusH = rowHeight;
    if (focusH > panelH - 6) {
      focusH = panelH - 6;
    }
    if (focusH < 8) {
      focusH = 8;
    }

    const int focusY = (panelH - focusH) / 2;
    const int focusCenterY = focusY + (focusH / 2);

    lv_obj_t *focus = lv_obj_create(panel);
    disableScroll(focus);
    lv_obj_remove_style_all(focus);
    constexpr int kFocusMarginX = 4;
    int focusW = panelW - (kFocusMarginX * 2);
    if (focusW < 1) {
      focusW = 1;
    }
    const int focusX = (panelW - focusW) / 2;
    lv_obj_set_pos(focus, focusX, focusY);
    lv_obj_set_size(focus, focusW, focusH);
    lv_obj_set_style_radius(focus, 8, 0);
    lv_obj_set_style_bg_color(focus, lv_color_hex(kClrAccentSoft), 0);
    lv_obj_set_style_bg_opa(focus, kOpa85, 0);
    lv_obj_set_style_border_width(focus, 1, 0);
    lv_obj_set_style_border_color(focus, lv_color_hex(kClrAccent), 0);

    const int separatorW = panelW - 14;
    if (separatorW > 2) {
      const int separatorX = (panelW - separatorW) / 2;
      lv_obj_t *sepTop = lv_obj_create(panel);
      disableScroll(sepTop);
      lv_obj_remove_style_all(sepTop);
      lv_obj_set_pos(sepTop, separatorX, focusY - 1);
      lv_obj_set_size(sepTop, separatorW, 1);
      lv_obj_set_style_bg_color(sepTop, lv_color_hex(kClrAccent), 0);
      lv_obj_set_style_bg_opa(sepTop, static_cast<lv_opa_t>(120), 0);

      lv_obj_t *sepBottom = lv_obj_create(panel);
      disableScroll(sepBottom);
      lv_obj_remove_style_all(sepBottom);
      lv_obj_set_pos(sepBottom, separatorX, focusY + focusH);
      lv_obj_set_size(sepBottom, separatorW, 1);
      lv_obj_set_style_bg_color(sepBottom, lv_color_hex(kClrAccent), 0);
      lv_obj_set_style_bg_opa(sepBottom, static_cast<lv_opa_t>(120), 0);
    }

    if (step <= 0 || maxValue < minValue) {
      service(nullptr);
      return;
    }

    const int slotCount = ((maxValue - minValue) / step) + 1;
    const int maxSelectableValue = minValue + ((slotCount - 1) * step);
    const int rowOffsets[5] = {-2, -1, 0, 1, 2};

    for (int i = 0; i < 5; ++i) {
      const int offset = rowOffsets[i];
      // Descending wheel: higher values are rendered above the center row.
      const int value = selectedValue - (offset * step);
      if (value < minValue || value > maxSelectableValue) {
        continue;
      }

      String text = String(static_cast<long>(value));
      text += suffix;

      lv_obj_t *label = lv_label_create(panel);
      const int labelW = panelW;
      setSingleLineLabel(label, labelW, LV_TEXT_ALIGN_CENTER);
      const bool isCenter = offset == 0;
      if (isCenter) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, kStyleAny);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE8F3FF), kStyleAny);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, kStyleAny);
      } else {
        lv_obj_set_style_text_font(label, font(), kStyleAny);
        lv_obj_set_style_text_color(label, lv_color_hex(kClrTextMuted), kStyleAny);
        int distance = offset < 0 ? -offset : offset;
        lv_opa_t textOpa = static_cast<lv_opa_t>(distance == 1 ? 180 : 110);
        lv_obj_set_style_text_opa(label, textOpa, kStyleAny);
      }
      lv_label_set_text(label, text.c_str());

      // Place each value by center point so selected text sits exactly in the
      // vertical center of the focused slot.
      lv_obj_set_width(label, LV_SIZE_CONTENT);
      lv_obj_set_height(label, LV_SIZE_CONTENT);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, offset * rowHeight);
    }

    service(nullptr);
  }

  void renderTextInput(const String &title,
                       const String &preview,
                       const std::vector<String> &keyLabels,
                       int selected,
                       int selectedCapsIndex,
                       const std::vector<lv_area_t> &areas) {
    const size_t keyCount = keyLabels.size();
    if (keyCount == 0 || areas.size() != keyCount) {
      return;
    }

    const unsigned long now = millis();
    const bool periodicRefresh =
        textInputLastFullRenderMs == 0 || now - textInputLastFullRenderMs >= kHeaderRefreshMs;
    const bool needFullRender =
        periodicRefresh ||
        textInputCacheTitle != title ||
        textInputCachePreview != preview ||
        textInputCacheButtons.size() != keyCount ||
        textInputCacheLabels.size() != keyCount ||
        !textInputLayoutMatches(areas) ||
        !textInputWidgetsValid();

    if (needFullRender) {
      int contentTop = 0;
      int contentBottom = 0;
      renderBase(title,
                 preview,
                 "ROTATE Move   OK Type   BACK",
                 contentTop,
                 contentBottom);

      textInputCacheTitle = title;
      textInputCachePreview = preview;
      textInputCacheAreas = areas;
      textInputCacheButtons.clear();
      textInputCacheLabels.clear();
      textInputCacheKeyLabels.clear();
      textInputCacheButtons.reserve(keyCount);
      textInputCacheLabels.reserve(keyCount);
      textInputCacheKeyLabels.reserve(keyCount);

      for (size_t i = 0; i < keyCount; ++i) {
        const lv_area_t &a = areas[i];
        lv_obj_t *btn = lv_button_create(lv_screen_active());
        disableScroll(btn);
        lv_obj_remove_style_all(btn);
        lv_obj_set_pos(btn, a.x1, a.y1);
        lv_obj_set_size(btn,
                        static_cast<int32_t>(a.x2 - a.x1 + 1),
                        static_cast<int32_t>(a.y2 - a.y1 + 1));

        lv_obj_t *label = lv_label_create(btn);
        setSingleLineLabel(label,
                           static_cast<int>(a.x2 - a.x1 + 1),
                           LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

        textInputCacheButtons.push_back(btn);
        textInputCacheLabels.push_back(label);
        textInputCacheKeyLabels.push_back("");
      }

      textInputCacheSelected = -1;
      textInputCacheCapsIndex = -1;
      textInputLastFullRenderMs = now;
    }

    for (size_t i = 0; i < keyCount; ++i) {
      lv_obj_t *btn = textInputCacheButtons[i];
      lv_obj_t *label = textInputCacheLabels[i];
      if (!btn || !label || !lv_obj_is_valid(btn) || !lv_obj_is_valid(label)) {
        clearTextInputHandles();
        return;
      }
    }

    bool labelsChanged = needFullRender;
    if (!labelsChanged && textInputCacheKeyLabels.size() == keyCount) {
      for (size_t i = 0; i < keyCount; ++i) {
        if (textInputCacheKeyLabels[i] != keyLabels[i]) {
          labelsChanged = true;
          break;
        }
      }
    } else if (!labelsChanged) {
      labelsChanged = true;
    }

    if (labelsChanged) {
      textInputCacheKeyLabels.assign(keyLabels.begin(), keyLabels.end());
      for (size_t i = 0; i < keyCount; ++i) {
        lv_obj_t *label = textInputCacheLabels[i];
        lv_label_set_text(label, keyLabels[i].c_str());
      }
    }

    auto applyButtonStyle = [&](int index) {
      if (index < 0 || index >= static_cast<int>(keyCount)) {
        return;
      }
      lv_obj_t *btn = textInputCacheButtons[static_cast<size_t>(index)];
      lv_obj_t *label = textInputCacheLabels[static_cast<size_t>(index)];

      const bool isSelected = selected == index;
      const bool isCapsActive = selectedCapsIndex == index;

      lv_color_t bg = lv_color_hex(kClrPanel);
      lv_color_t fg = lv_color_hex(kClrTextPrimary);
      lv_color_t border = lv_color_hex(kClrTextMuted);
      int borderWidth = 1;
      int outlineWidth = 0;
      lv_opa_t outlineOpa = LV_OPA_TRANSP;

      if (isCapsActive) {
        bg = lv_color_hex(0x2A4F8C);
        border = lv_color_hex(0x83AEE8);
      }
      if (isSelected) {
        bg = lv_color_hex(0x2B4E75);
        border = lv_color_hex(0xCBE2FF);
        borderWidth = 2;
        outlineWidth = 1;
        outlineOpa = static_cast<lv_opa_t>(180);
      }

      lv_obj_set_style_bg_color(btn, bg, 0);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(btn, borderWidth, 0);
      lv_obj_set_style_border_color(btn, border, 0);
      lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, 0);
      lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_outline_width(btn, outlineWidth, 0);
      lv_obj_set_style_outline_color(btn, lv_color_hex(kClrAccent), 0);
      lv_obj_set_style_outline_opa(btn, outlineOpa, 0);
      lv_obj_set_style_outline_pad(btn, 0, 0);
      lv_obj_set_style_radius(btn, 4, 0);
      lv_obj_set_style_pad_all(btn, 0, 0);

      lv_obj_set_style_text_color(label, fg, kStyleAny);
      lv_obj_set_width(label, LV_PCT(100));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    };

    if (needFullRender) {
      for (size_t i = 0; i < keyCount; ++i) {
        applyButtonStyle(static_cast<int>(i));
      }
    } else {
      int dirty[4] = {-1, -1, -1, -1};
      int dirtyCount = 0;
      auto pushDirty = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(keyCount)) {
          return;
        }
        for (int n = 0; n < dirtyCount; ++n) {
          if (dirty[n] == idx) {
            return;
          }
        }
        if (dirtyCount < 4) {
          dirty[dirtyCount++] = idx;
        }
      };

      pushDirty(textInputCacheSelected);
      pushDirty(selected);
      pushDirty(textInputCacheCapsIndex);
      pushDirty(selectedCapsIndex);

      for (int n = 0; n < dirtyCount; ++n) {
        applyButtonStyle(dirty[n]);
      }
    }

    textInputCacheSelected = selected;
    textInputCacheCapsIndex = selectedCapsIndex;

    service(nullptr);
  }

  void renderProgressOverlay(const String &title,
                             const String &message,
                             int percent) {
    lv_obj_t *screen = lv_screen_active();
    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());

    int panelW = w - 20;
    if (panelW > 300) {
      panelW = 300;
    }
    if (panelW < 120) {
      panelW = w - 8;
    }
    if (panelW < 80) {
      panelW = w;
    }

    int panelH = h - 24;
    if (panelH > 118) {
      panelH = 118;
    }
    if (panelH < 72) {
      panelH = h - 6;
    }
    if (panelH < 48) {
      panelH = 48;
    }

    const int innerPad = 10;
    const int titleY = 8;
    const int spinnerSize = 22;
    const int messageY = 34;
    const int barY = panelH - 22;
    int messageHeight = panelH - messageY - 16;
    if (messageHeight < 12) {
      messageHeight = 12;
    }
    if (percent >= 0) {
      messageHeight = barY - messageY - 6;
      if (messageHeight < 12) {
        messageHeight = 12;
      }
    }

    const bool needsCreate =
        progressOverlay == nullptr ||
        !lv_obj_is_valid(progressOverlay) ||
        lv_obj_get_parent(progressOverlay) != screen;

    if (needsCreate) {
      clearProgressHandles();

      progressOverlay = lv_obj_create(screen);
      disableScroll(progressOverlay);
      lv_obj_remove_style_all(progressOverlay);
      lv_obj_set_style_bg_color(progressOverlay, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(progressOverlay, kOpa75, 0);
      lv_obj_set_style_border_width(progressOverlay, 0, 0);
      lv_obj_set_style_radius(progressOverlay, 0, 0);
      lv_obj_move_foreground(progressOverlay);

      progressPanel = lv_obj_create(progressOverlay);
      disableScroll(progressPanel);
      lv_obj_remove_style_all(progressPanel);
      lv_obj_set_style_bg_color(progressPanel, lv_color_hex(kClrPanel), kStyleAny);
      lv_obj_set_style_bg_opa(progressPanel, LV_OPA_COVER, kStyleAny);
      lv_obj_set_style_border_color(progressPanel, lv_color_hex(kClrAccent), kStyleAny);
      lv_obj_set_style_border_width(progressPanel, 1, kStyleAny);
      lv_obj_set_style_radius(progressPanel, 10, kStyleAny);
      lv_obj_set_style_pad_all(progressPanel, 0, kStyleAny);

      progressTitle = lv_label_create(progressPanel);
      setSingleLineLabel(progressTitle, panelW - 56, LV_TEXT_ALIGN_LEFT);
      lv_obj_set_style_text_color(progressTitle, lv_color_white(), 0);

      progressSpinner = lv_spinner_create(progressPanel);

      progressMessage = lv_label_create(progressPanel);
      setWrapLabel(progressMessage, panelW - (innerPad * 2), messageHeight);
      lv_obj_set_style_text_color(progressMessage, lv_color_white(), 0);

      progressBar = lv_bar_create(progressPanel);
      lv_bar_set_range(progressBar, 0, 100);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(kClrAccent), LV_PART_INDICATOR);

      progressPercent = lv_label_create(progressPanel);
      setSingleLineLabel(progressPercent, 44, LV_TEXT_ALIGN_RIGHT);
      lv_obj_set_style_text_color(progressPercent, lv_color_hex(0xA5E8FF), 0);
    }

    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_set_size(progressOverlay, w, h);
      lv_obj_set_pos(progressOverlay, 0, 0);
    }
    if (progressPanel && lv_obj_is_valid(progressPanel)) {
      lv_obj_set_size(progressPanel, panelW, panelH);
      lv_obj_center(progressPanel);
    }
    if (progressTitle && lv_obj_is_valid(progressTitle)) {
      lv_obj_set_width(progressTitle, panelW - 56);
      lv_obj_set_pos(progressTitle, innerPad, titleY);
    }
    if (progressSpinner && lv_obj_is_valid(progressSpinner)) {
      lv_obj_set_size(progressSpinner, spinnerSize, spinnerSize);
      lv_obj_set_pos(progressSpinner, panelW - innerPad - spinnerSize, 6);
    }
    if (progressMessage && lv_obj_is_valid(progressMessage)) {
      lv_obj_set_width(progressMessage, panelW - (innerPad * 2));
      lv_obj_set_height(progressMessage, messageHeight);
      lv_obj_set_pos(progressMessage, innerPad, messageY);
    }
    if (progressBar && lv_obj_is_valid(progressBar)) {
      lv_obj_set_size(progressBar, panelW - (innerPad * 2), 10);
      lv_obj_set_pos(progressBar, innerPad, barY);
    }
    if (progressPercent && lv_obj_is_valid(progressPercent)) {
      lv_obj_set_pos(progressPercent, panelW - innerPad - 44, barY - 16);
    }

    if (progressTitle) {
      lv_label_set_text(progressTitle, title.c_str());
    }
    if (progressMessage) {
      lv_label_set_text(progressMessage, message.c_str());
    }

    if (progressBar && progressPercent) {
      if (percent < 0) {
        lv_obj_add_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
      } else {
        if (percent > 100) {
          percent = 100;
        }
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, percent, LV_ANIM_OFF);

        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        lv_label_set_text(progressPercent, pct);
      }
    }

    service(nullptr);
  }

  void hideProgressOverlay() {
    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_del(progressOverlay);
    }
    clearProgressHandles();
    service(nullptr);
  }

  void renderBootSplash(const String &subtitle) {
    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    clearTextInputHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kClrBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());

    // Accent border line at top
    lv_obj_t *topLine = lv_obj_create(screen);
    disableScroll(topLine);
    lv_obj_remove_style_all(topLine);
    lv_obj_set_size(topLine, w, 3);
    lv_obj_set_pos(topLine, 0, 0);
    lv_obj_set_style_bg_color(topLine, lv_color_hex(kClrAccent), 0);
    lv_obj_set_style_bg_opa(topLine, LV_OPA_COVER, 0);

    // "ZX-OS" logo label - use montserrat 18 for larger ASCII-only branding
    lv_obj_t *logoLabel = lv_label_create(screen);
    lv_obj_set_style_text_font(logoLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(logoLabel, lv_color_hex(kClrAccent), 0);
    lv_obj_set_style_text_opa(logoLabel, LV_OPA_COVER, 0);
    lv_label_set_text(logoLabel, "ZX-OS");
    lv_obj_set_width(logoLabel, w - 16);
    lv_obj_set_style_text_align(logoLabel, LV_TEXT_ALIGN_CENTER, 0);

    const int logoY = subtitle.length() > 0 ? (h / 2) - 22 : (h / 2) - 10;
    lv_obj_set_pos(logoLabel, 8, logoY);

    if (subtitle.length() > 0) {
      lv_obj_t *subLabel = lv_label_create(screen);
      setLabelFont(subLabel);
      lv_obj_set_style_text_color(subLabel, lv_color_hex(kClrTextMuted), 0);
      lv_obj_set_style_text_opa(subLabel, LV_OPA_COVER, 0);
      lv_obj_set_width(subLabel, w - 16);
      lv_obj_set_style_text_align(subLabel, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(subLabel, LV_LABEL_LONG_WRAP);
      lv_label_set_text(subLabel, subtitle.c_str());
      lv_obj_set_pos(subLabel, 8, logoY + 28);
    }

    service(nullptr);
  }
};

UiRuntime::UiRuntime() : impl_(new Impl()) {}

void UiRuntime::begin() {
  if (!impl_->begin()) {
    Serial.println("[ui] runtime begin failed");
    return;
  }

  impl_->service(nullptr);

  int contentTop = 0;
  int contentBottom = 0;
  impl_->renderBase("Boot", "", "", contentTop, contentBottom);

  lv_obj_t *label = lv_label_create(lv_screen_active());
  impl_->setSingleLineLabel(label, 120, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(label, "Booting...");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  impl_->service(nullptr);
  delay(40);
  impl_->service(nullptr);
}

void UiRuntime::tick() {
  impl_->service(nullptr);
}

UiEvent UiRuntime::pollInput() {
  return impl_->pollInput();
}

void UiRuntime::resetInputState() {
  impl_->input.resetState();
}

void UiRuntime::setOkBackBlocked(bool blocked) {
  impl_->input.setOkBackBlocked(blocked);
}

void UiRuntime::setStatusLine(const String &line) {
  impl_->statusLine = line;
}

void UiRuntime::setLanguage(UiLanguage language) {
  impl_->language = language;
  if (impl_->port.ready()) {
    impl_->applyTheme();
  }
}

UiLanguage UiRuntime::language() const {
  return impl_->language;
}

bool UiRuntime::setKoreanFontInstalled(bool installed, String *error) {
  if (installed) {
    String err;
    const lv_font_t *font = nullptr;
    if (mountSdCard(&err)) {
      font = sdfont::open(sdfont::kKoreanFontPath, &lv_font_montserrat_14, &err);
    }
    if (font) {
      impl_->koreanFont = font;
    } else {
      installed = false;
      Serial.printf("[ui] Korean font unavailable: %s\n", err.c_str());
      if (error) {
        *error = err;
      }
    }
  } else {
    sdfont::close();
  }

  impl_->koreanFontInstalled = installed;
  if (impl_->port.ready()) {
    impl_->applyTheme();
  }
  return installed;
}

bool UiRuntime::isKoreanFontInstalled() const {
  return impl_->koreanFontInstalled;
}

void UiRuntime::setTimezone(const String &tz) {
  impl_->setTimezone(tz);
}

String UiRuntime::timezone() const {
  return impl_->timezone();
}

bool UiRuntime::syncTimezoneFromIp(String *resolvedTz, String *error) {
  return impl_->syncTimezoneFromIp(resolvedTz, error);
}

void UiRuntime::setDisplayBrightnessPercent(uint8_t percent) {
  impl_->setDisplayBrightnessPercent(percent);
}

uint8_t UiRuntime::displayBrightnessPercent() const {
  return impl_->getDisplayBrightnessPercent();
}

int UiRuntime::launcherLoop(const String &title,
                            const std::vector<String> &items,
                            int selectedIndex,
                            const std::function<void()> &backgroundTick) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;

  while (true) {
    if (redraw) {
      impl_->renderLauncher(title, items, selected);
      redraw = false;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + ev.delta,
                           static_cast<int>(items.size()));
      redraw = true;
    }
    if (ev.ok) {
      return selected;
    }
    if (ev.back) {
      return -1;
    }

    delay(kUiLoopDelayMs);
  }
}

int UiRuntime::menuLoop(const String &title,
                        const std::vector<String> &items,
                        int selectedIndex,
                        const std::function<void()> &backgroundTick,
                        const String &footer,
                        const String &subtitle) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderMenu(title, items, selected, subtitle, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + ev.delta,
                           static_cast<int>(items.size()));
      redraw = true;
    }
    if (ev.ok) {
      return selected;
    }
    if (ev.back) {
      return -1;
    }

    delay(kUiLoopDelayMs);
  }
}

MessengerAction UiRuntime::messengerHomeLoop(const std::vector<String> &previewLines,
                                             int selectedIndex,
                                             const std::function<void()> &backgroundTick) {
  constexpr int kButtonCount = 3;
  constexpr int kSelectableCount = kButtonCount + 1;  // + message box
  constexpr unsigned long kMessageRefreshMs = 5000;
  constexpr int kInitialBottomScrollLines = 1000000;
  int focus = wrapIndex(selectedIndex + 1, kSelectableCount);
  bool scrollMode = false;
  int scrollOffsetLines = kInitialBottomScrollLines;  // Clamp to bottom on first render.
  int maxScrollLines = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();
  unsigned long lastMessageRefreshMs = lastRefreshMs;

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      maxScrollLines = impl_->renderMessengerHome(previewLines,
                                                  focus,
                                                  scrollMode,
                                                  scrollOffsetLines);
      if (scrollOffsetLines > maxScrollLines) {
        scrollOffsetLines = maxScrollLines;
      }
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();
    const bool interacted = ev.delta != 0 || ev.ok || ev.back || ev.okLong ||
                            ev.okCount != 0 || ev.backCount != 0 ||
                            ev.okLongCount != 0;
    if (interacted) {
      lastMessageRefreshMs = millis();
    }

    if (scrollMode) {
      if (ev.delta != 0) {
        int nextOffset = scrollOffsetLines + ev.delta;
        if (nextOffset < 0) {
          nextOffset = 0;
        }
        if (nextOffset > maxScrollLines) {
          nextOffset = maxScrollLines;
        }
        if (nextOffset != scrollOffsetLines) {
          scrollOffsetLines = nextOffset;
          redraw = true;
        }
      }

      if (ev.ok || ev.back) {
        scrollMode = false;
        redraw = true;
      }

      delay(kUiLoopDelayMs);
      continue;
    }

    if (ev.delta != 0) {
      focus = wrapIndex(focus + ev.delta, kSelectableCount);
      redraw = true;
    }

    if (ev.okLong && focus == 1) {
      return MessengerAction::TextLong;
    }

    if (ev.ok) {
      if (focus == 0) {
        scrollMode = true;
        redraw = true;
        continue;
      }
      if (focus == 1) {
        return MessengerAction::Text;
      }
      if (focus == 2) {
        return MessengerAction::Voice;
      }
      return MessengerAction::File;
    }

    if (ev.back) {
      return MessengerAction::Back;
    }

    if (millis() - lastMessageRefreshMs >= kMessageRefreshMs) {
      return MessengerAction::Refresh;
    }

    delay(kUiLoopDelayMs);
  }
}

void UiRuntime::showInfo(const String &title,
                         const std::vector<String> &lines,
                         const std::function<void()> &backgroundTick,
                         const String &footer) {
  int startIndex = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderInfo(title, lines, startIndex, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      int next = startIndex + ev.delta;
      if (next < 0) {
        next = 0;
      }
      if (next > static_cast<int>(lines.size()) - 1) {
        next = static_cast<int>(lines.size()) - 1;
      }
      if (next < 0) {
        next = 0;
      }
      if (next != startIndex) {
        startIndex = next;
        redraw = true;
      }
    }

    if (ev.ok || ev.back) {
      return;
    }

    delay(kUiLoopDelayMs);
  }
}

bool UiRuntime::confirm(const String &title,
                        const String &message,
                        const std::function<void()> &backgroundTick,
                        const String &confirmLabel,
                        const String &cancelLabel) {
  std::vector<String> options;
  options.push_back(confirmLabel);
  options.push_back(cancelLabel);
  const int selected = menuLoop(title,
                                options,
                                1,
                                backgroundTick,
                                "OK   BACK",
                                message);
  return selected == 0;
}

bool UiRuntime::numberWheelInput(const String &title,
                                 int minValue,
                                 int maxValue,
                                 int step,
                                 int &inOutValue,
                                 const std::function<void()> &backgroundTick,
                                 const String &suffix,
                                 const std::function<void(int)> &onValueChanged) {
  if (step <= 0 || maxValue < minValue) {
    return false;
  }

  const int slotCount = ((maxValue - minValue) / step) + 1;
  const int maxSelectableValue = minValue + ((slotCount - 1) * step);
  if (slotCount <= 0) {
    return false;
  }

  int value = inOutValue;
  if (value < minValue) {
    value = minValue;
  }
  if (value > maxSelectableValue) {
    value = maxSelectableValue;
  }
  value = minValue + (((value - minValue) / step) * step);
  if (onValueChanged) {
    onValueChanged(value);
  }

  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderNumberWheel(title,
                               value,
                               minValue,
                               maxValue,
                               step,
                               suffix,
                               "ROTATE Wheel   OK Save   BACK");
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      int nextValue = value - (ev.delta * step);
      if (nextValue < minValue) {
        nextValue = minValue;
      }
      if (nextValue > maxSelectableValue) {
        nextValue = maxSelectableValue;
      }
      if (nextValue != value) {
        value = nextValue;
        if (onValueChanged) {
          onValueChanged(value);
        }
        redraw = true;
      }
    }

    if (ev.ok) {
      inOutValue = value;
      return true;
    }
    if (ev.back) {
      return false;
    }

    delay(kUiLoopDelayMs);
  }
}

bool UiRuntime::textInput(const String &title,
                          String &inOutValue,
                          bool mask,
                          const std::function<void()> &backgroundTick) {
  struct CharKeyPair {
    char normal;
    char shifted;
  };

  enum class KeyAction : uint8_t {
    Character,
    Done,
    Caps,
    Del,
    Space,
    Cancel,
  };

  struct KeySlot {
    KeyAction action = KeyAction::Character;
    char normal = 0;
    char shifted = 0;
    const char *label = "";
    lv_area_t area{};
  };

  static const CharKeyPair kRow0[] = {
      {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
      {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}};
  static const CharKeyPair kRow1[] = {
      {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'},
      {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}};
  static const CharKeyPair kRow2[] = {
      {'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'},
      {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {'\\', '|'}};
  static const CharKeyPair kRow3[] = {
      {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'}, {'b', 'B'},
      {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}};

  String working = inOutValue;
  bool caps = false;
  int selected = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  const int displayWidth = lv_display_get_horizontal_resolution(impl_->port.display());
  const int displayHeight = lv_display_get_vertical_resolution(impl_->port.display());
  const int maxColumns = 12;
  const int keyGap = displayWidth >= 260 ? 2 : 1;
  int keyWidth = (displayWidth - 8 - (keyGap * (maxColumns - 1))) / maxColumns;
  if (keyWidth < 10) {
    keyWidth = 10;
  }

  int fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  if (fullRowWidth > displayWidth - 4) {
    keyWidth = (displayWidth - 4 - (keyGap * (maxColumns - 1))) / maxColumns;
    if (keyWidth < 8) {
      keyWidth = 8;
    }
    fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  }

  // Keep keyboard area aligned with renderBase() content bounds so it never
  // overlaps the preview subtitle or footer hint bar.
  int contentTop = 4 + kHeaderHeight + 4;
  contentTop += kSubtitleHeight + 4;  // textInput always shows preview subtitle
  contentTop += 2;

  int contentBottom = displayHeight - 6;
  const int footerY = displayHeight - kFooterHeight - 4;  // textInput always shows footer
  contentBottom = footerY - 4;
  if (contentBottom > displayHeight - 6) {
    contentBottom = displayHeight - 6;
  }
  if (contentBottom < contentTop + kMinContentHeight) {
    contentBottom = contentTop + kMinContentHeight;
    if (contentBottom > displayHeight - 6) {
      contentBottom = displayHeight - 6;
    }
  }
  if (contentBottom < contentTop) {
    contentBottom = contentTop;
  }

  const int availableHeight = contentBottom - contentTop + 1;
  const int rowCount = 5;
  int maxFitKeyHeight = (availableHeight - (keyGap * (rowCount - 1))) / rowCount;
  if (maxFitKeyHeight < 1) {
    maxFitKeyHeight = 1;
  }
  int keyHeight = (availableHeight - (keyGap * (rowCount - 1))) / rowCount;
  if (keyHeight > 24) {
    keyHeight = 24;
  }
  if (keyHeight < 12) {
    keyHeight = 12;
  }
  if (keyHeight > maxFitKeyHeight) {
    keyHeight = maxFitKeyHeight;
  }
  const int keyboardHeight = rowCount * keyHeight + (rowCount - 1) * keyGap;
  int keyboardTop = contentTop + (availableHeight - keyboardHeight) / 2;
  if (keyboardTop < contentTop) {
    keyboardTop = contentTop;
  }
  if (keyboardTop + keyboardHeight - 1 > contentBottom) {
    keyboardTop = contentBottom - keyboardHeight + 1;
    if (keyboardTop < contentTop) {
      keyboardTop = contentTop;
    }
  }
  int keyboardLeft = (displayWidth - fullRowWidth) / 2;
  if (keyboardLeft < 2) {
    keyboardLeft = 2;
  }
  const bool compactKeyLabels = keyWidth < 16;

  std::vector<KeySlot> keys;
  keys.reserve(64);

  auto addCharRow = [&](const CharKeyPair *row, size_t len, int rowIndex) {
    const int y = keyboardTop + rowIndex * (keyHeight + keyGap);
    const int rowWidth = static_cast<int>(len) * keyWidth +
                         (static_cast<int>(len) - 1) * keyGap;
    int x = (displayWidth - rowWidth) / 2;
    if (x < 2) {
      x = 2;
    }

    for (size_t i = 0; i < len; ++i) {
      KeySlot slot;
      slot.action = KeyAction::Character;
      slot.normal = row[i].normal;
      slot.shifted = row[i].shifted;
      slot.area.x1 = x;
      slot.area.y1 = y;
      slot.area.x2 = x + keyWidth - 1;
      slot.area.y2 = y + keyHeight - 1;
      keys.push_back(slot);
      x += keyWidth + keyGap;
    }
  };

  addCharRow(kRow0, sizeof(kRow0) / sizeof(kRow0[0]), 0);
  addCharRow(kRow1, sizeof(kRow1) / sizeof(kRow1[0]), 1);
  addCharRow(kRow2, sizeof(kRow2) / sizeof(kRow2[0]), 2);
  addCharRow(kRow3, sizeof(kRow3) / sizeof(kRow3[0]), 3);

  const int actionRowY = keyboardTop + (keyHeight + keyGap) * 4;
  static const int kActionUnits[5] = {2, 2, 2, 4, 2};
  static const KeyAction kActionKinds[5] = {
      KeyAction::Done,
      KeyAction::Caps,
      KeyAction::Del,
      KeyAction::Space,
      KeyAction::Cancel};
  static const char *kActionLabelsWide[5] = {"DONE", "CAPS", "DEL", "SPACE", "CANCEL"};
  static const char *kActionLabelsCompact[5] = {"OK", "CAP", "DEL", "SPC", "ESC"};
  const char *const *actionLabels = compactKeyLabels ? kActionLabelsCompact : kActionLabelsWide;

  int actionX = keyboardLeft;
  int capsIndex = -1;
  for (int i = 0; i < 5; ++i) {
    KeySlot slot;
    slot.action = kActionKinds[i];
    slot.label = actionLabels[i];
    const int width = kActionUnits[i] * keyWidth + (kActionUnits[i] - 1) * keyGap;
    slot.area.x1 = actionX;
    slot.area.y1 = actionRowY;
    slot.area.x2 = actionX + width - 1;
    slot.area.y2 = actionRowY + keyHeight - 1;
    if (slot.action == KeyAction::Caps) {
      capsIndex = static_cast<int>(keys.size());
    }
    keys.push_back(slot);
    actionX += width + keyGap;
  }

  auto buildPreview = [&]() -> String {
    String preview = maskIfNeeded(working, mask);
    if (preview.isEmpty()) {
      preview = "(empty)";
    }

    const size_t kMaxPreviewChars = displayWidth >= 260 ? 40 : 24;
    if (preview.length() > kMaxPreviewChars) {
      const size_t tail = kMaxPreviewChars - 3;
      preview = "..." + preview.substring(preview.length() - tail);
    }
    return preview;
  };

  auto labelForKey = [&](const KeySlot &slot) -> String {
    if (slot.action == KeyAction::Character) {
      return String(caps ? slot.shifted : slot.normal);
    }
    if (slot.action == KeyAction::Caps) {
      if (compactKeyLabels) {
        return caps ? "ON" : "CAP";
      }
      return caps ? "CAPS ON" : "CAPS";
    }
    return String(slot.label);
  };

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      std::vector<String> labels;
      labels.reserve(keys.size());
      for (std::vector<KeySlot>::const_iterator it = keys.begin();
           it != keys.end();
           ++it) {
        labels.push_back(labelForKey(*it));
      }

      impl_->renderTextInput(title,
                             buildPreview(),
                             labels,
                             selected,
                             (caps && capsIndex >= 0) ? capsIndex : -1,
                             [&]() {
                               std::vector<lv_area_t> areas;
                               areas.reserve(keys.size());
                               for (size_t i = 0; i < keys.size(); ++i) {
                                 areas.push_back(keys[i].area);
                               }
                               return areas;
                             }());
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + ev.delta,
                           static_cast<int>(keys.size()));
      redraw = true;
    }

    if (ev.back) {
      return false;
    }

    uint8_t okPresses = ev.okCount;
    if (okPresses == 0 && ev.ok) {
      okPresses = 1;
    }

    while (okPresses > 0) {
      --okPresses;
      const KeySlot &slot = keys[static_cast<size_t>(selected)];
      if (slot.action == KeyAction::Character) {
        working += caps ? slot.shifted : slot.normal;
        redraw = true;
      } else if (slot.action == KeyAction::Done) {
        inOutValue = working;
        return true;
      } else if (slot.action == KeyAction::Caps) {
        caps = !caps;
        redraw = true;
      } else if (slot.action == KeyAction::Del) {
        if (working.length() > 0) {
          working.remove(working.length() - 1);
        }
        redraw = true;
      } else if (slot.action == KeyAction::Space) {
        working += " ";
        redraw = true;
      } else if (slot.action == KeyAction::Cancel) {
        return false;
      }
    }

    delay(kUiLoopDelayMs);
  }
}

void UiRuntime::showProgressOverlay(const String &title,
                                    const String &message,
                                    int percent) {
  impl_->renderProgressOverlay(title, message, percent);
}

void UiRuntime::hideProgressOverlay() {
  impl_->hideProgressOverlay();
}

void UiRuntime::showToast(const String &title,
                          const String &message,
                          unsigned long showMs,
                          const std::function<void()> &backgroundTick) {
  const unsigned long start = millis();
  unsigned long lastRefreshMs = 0;

  while (true) {
    const unsigned long now = millis();
    if (lastRefreshMs == 0 || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderToast(title, message, uiText(language(), UiTextKey::OkBackClose));
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();
    if (ev.ok || ev.back || now - start >= showMs) {
      return;
    }

    delay(kUiLoopDelayMs);
  }
}

void UiRuntime::showBootSplash(const String &subtitle,
                               unsigned long durationMs,
                               const std::function<void()> &backgroundTick) {
  const unsigned long start = millis();
  bool rendered = false;

  while (true) {
    if (!rendered) {
      impl_->renderBootSplash(subtitle);
      rendered = true;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();
    if (ev.ok || ev.back || millis() - start >= durationMs) {
      return;
    }

    delay(kUiLoopDelayMs);
  }
}
