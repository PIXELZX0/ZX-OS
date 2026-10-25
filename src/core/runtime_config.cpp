#include "runtime_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include "board_pins.h"
#include "shared_spi_bus.h"
#include "user_config.h"

namespace {

constexpr const char *kPrefsNamespace = "oc_cfg";
constexpr const char *kConfigVersionKey = "cfg_ver";
constexpr const char *kConfigBlobKey = "cfg_blob";
constexpr uint32_t kConfigVersion = 2;
constexpr const char *kSdConfigPath = "/oc_cfg.json";
constexpr const char *kSdConfigTempPath = "/oc_cfg.tmp";
constexpr const char *kSdEnvPath = "/.env";
constexpr uint32_t kSdSpiFrequencyHz = 25000000UL;

void fromJson(const JsonObjectConst &obj, RuntimeConfig &config);
bool mountSd(String *reason);

struct EnvGatewayOverrides {
  bool hasGatewayUrl = false;
  bool hasGatewayToken = false;
  bool hasGatewayPassword = false;
  bool hasGatewayAuthMode = false;
  bool hasGatewayDeviceId = false;
  bool hasGatewayDevicePublicKey = false;
  bool hasGatewayDevicePrivateKey = false;
  bool hasGatewayDeviceToken = false;

  String gatewayUrl;
  String gatewayToken;
  String gatewayPassword;
  GatewayAuthMode gatewayAuthMode = GatewayAuthMode::Token;
  String gatewayDeviceId;
  String gatewayDevicePublicKey;
  String gatewayDevicePrivateKey;
  String gatewayDeviceToken;
};

bool isPlaceholder(const char *value) {
  if (!value || !value[0]) {
    return true;
  }
  return strncmp(value, "REPLACE_WITH_", 13) == 0;
}

String trimDeviceName(String value) {
  value.trim();
  return value;
}

String defaultDeviceNameValue() {
  String name = trimDeviceName(String(USER_OPENCLAW_DISPLAY_NAME));
  if (name.isEmpty()) {
    name = "ZX-OS Node";
  }
  return name;
}

bool startsWithWsScheme(const String &url) {
  return url.startsWith("ws://") || url.startsWith("wss://");
}

bool isLikelyHexString(const String &value) {
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[static_cast<unsigned int>(i)];
    const bool isHex = (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
    if (!isHex) {
      return false;
    }
  }
  return true;
}

void appendMessage(String &target, const String &message) {
  if (message.isEmpty()) {
    return;
  }
  if (!target.isEmpty()) {
    target += "; ";
  }
  target += message;
}

String trimAndUnquote(String value) {
  value.trim();
  if (value.length() >= 2) {
    const char first = value[0];
    const char last = value[value.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      value = value.substring(1, value.length() - 1);
    }
  }
  value.trim();
  return value;
}

bool parseGatewayAuthModeValue(const String &raw, GatewayAuthMode &mode) {
  String normalized = raw;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "token" || normalized == "0") {
    mode = GatewayAuthMode::Token;
    return true;
  }
  if (normalized == "password" || normalized == "1") {
    mode = GatewayAuthMode::Password;
    return true;
  }
  return false;
}

bool applyEnvGatewayKey(const String &key,
                        const String &value,
                        EnvGatewayOverrides &overrides) {
  if (key == "OPENCLAW_GATEWAY_URL" || key == "GATEWAY_URL") {
    overrides.hasGatewayUrl = true;
    overrides.gatewayUrl = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_TOKEN" || key == "GATEWAY_TOKEN") {
    overrides.hasGatewayToken = true;
    overrides.gatewayToken = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_PASSWORD" || key == "GATEWAY_PASSWORD") {
    overrides.hasGatewayPassword = true;
    overrides.gatewayPassword = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_AUTH_MODE" || key == "GATEWAY_AUTH_MODE") {
    GatewayAuthMode mode = GatewayAuthMode::Token;
    if (parseGatewayAuthModeValue(value, mode)) {
      overrides.hasGatewayAuthMode = true;
      overrides.gatewayAuthMode = mode;
    }
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_DEVICE_ID" || key == "GATEWAY_DEVICE_ID") {
    overrides.hasGatewayDeviceId = true;
    overrides.gatewayDeviceId = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_DEVICE_PUBLIC_KEY" || key == "GATEWAY_DEVICE_PUBLIC_KEY") {
    overrides.hasGatewayDevicePublicKey = true;
    overrides.gatewayDevicePublicKey = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_DEVICE_PRIVATE_KEY" || key == "GATEWAY_DEVICE_PRIVATE_KEY") {
    overrides.hasGatewayDevicePrivateKey = true;
    overrides.gatewayDevicePrivateKey = value;
    return true;
  }
  if (key == "OPENCLAW_GATEWAY_DEVICE_TOKEN" || key == "GATEWAY_DEVICE_TOKEN") {
    overrides.hasGatewayDeviceToken = true;
    overrides.gatewayDeviceToken = value;
    return true;
  }
  return false;
}

void parseEnvGatewayOverridesFromFile(File &file, EnvGatewayOverrides &out) {
  while (file.available()) {
    String line = file.readStringUntil('\n');

    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }

    if (line.startsWith("export ")) {
      line = line.substring(7);
      line.trim();
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, static_cast<unsigned int>(eq));
    key.trim();
    if (key.isEmpty()) {
      continue;
    }

    String value = line.substring(static_cast<unsigned int>(eq + 1));
    value = trimAndUnquote(value);
    applyEnvGatewayKey(key, value, out);
  }
}

bool readEnvGatewayOverridesFromSd(EnvGatewayOverrides &overrides,
                                   bool &found,
                                   String *error = nullptr) {
  found = false;

  String mountErr;
  if (!mountSd(&mountErr)) {
    return true;
  }

  if (!SD.exists(kSdEnvPath)) {
    return true;
  }

  File file = SD.open(kSdEnvPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = ".env open failed";
    }
    return false;
  }

  found = true;
  parseEnvGatewayOverridesFromFile(file, overrides);
  file.close();
  return true;
}

void applyEnvGatewayOverrides(RuntimeConfig &config,
                              const EnvGatewayOverrides &overrides) {
  if (overrides.hasGatewayUrl) {
    config.gatewayUrl = overrides.gatewayUrl;
  }
  if (overrides.hasGatewayToken) {
    config.gatewayToken = overrides.gatewayToken;
  }
  if (overrides.hasGatewayPassword) {
    config.gatewayPassword = overrides.gatewayPassword;
  }
  if (overrides.hasGatewayDeviceId) {
    config.gatewayDeviceId = overrides.gatewayDeviceId;
  }
  if (overrides.hasGatewayDevicePublicKey) {
    config.gatewayDevicePublicKey = overrides.gatewayDevicePublicKey;
  }
  if (overrides.hasGatewayDevicePrivateKey) {
    config.gatewayDevicePrivateKey = overrides.gatewayDevicePrivateKey;
  }
  if (overrides.hasGatewayDeviceToken) {
    config.gatewayDeviceToken = overrides.gatewayDeviceToken;
  }

  if (overrides.hasGatewayAuthMode) {
    config.gatewayAuthMode = overrides.gatewayAuthMode;
    return;
  }

  if (overrides.hasGatewayToken && !overrides.hasGatewayPassword) {
    config.gatewayAuthMode = GatewayAuthMode::Token;
  } else if (!overrides.hasGatewayToken && overrides.hasGatewayPassword) {
    config.gatewayAuthMode = GatewayAuthMode::Password;
  } else if (overrides.hasGatewayToken && overrides.hasGatewayPassword) {
    if (!config.gatewayToken.isEmpty() && config.gatewayPassword.isEmpty()) {
      config.gatewayAuthMode = GatewayAuthMode::Token;
    } else if (config.gatewayToken.isEmpty() && !config.gatewayPassword.isEmpty()) {
      config.gatewayAuthMode = GatewayAuthMode::Password;
    }
  }

  if (config.gatewayToken.isEmpty() && !config.gatewayPassword.isEmpty()) {
    config.gatewayAuthMode = GatewayAuthMode::Password;
  }
}

bool mountSd(String *reason) {
  if (boardpins::kTftCs >= 0) {
    pinMode(boardpins::kTftCs, OUTPUT);
    digitalWrite(boardpins::kTftCs, HIGH);
  }
  if (boardpins::kCc1101Cs >= 0) {
    pinMode(boardpins::kCc1101Cs, OUTPUT);
    digitalWrite(boardpins::kCc1101Cs, HIGH);
  }
  if (boardpins::kSdCs >= 0) {
    pinMode(boardpins::kSdCs, OUTPUT);
    digitalWrite(boardpins::kSdCs, HIGH);
  }

#if !HAL_HAS_SD_CARD
  if (reason) *reason = "No SD card support on this board";
  return false;
#else
  SPIClass *spiBus = sharedspi::bus();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                kSdSpiFrequencyHz,
                                "/sd",
                                8,
                                false);
  if (!mounted && reason) {
    *reason = "SD mount failed";
  }
  return mounted;
#endif  // HAL_HAS_SD_CARD
}

bool parseConfigBlob(const String &blob,
                     RuntimeConfig &outConfig,
                     String *error = nullptr) {
  DynamicJsonDocument doc(4096);
  const auto parseErr = deserializeJson(doc, blob);
  if (parseErr || !doc.is<JsonObject>()) {
    if (error) {
      *error = "Config parse failed";
    }
    return false;
  }

  RuntimeConfig parsed = outConfig;
  fromJson(doc.as<JsonObjectConst>(), parsed);

  String validateErr;
  if (!validateConfig(parsed, &validateErr)) {
    if (error) {
      *error = "Config validation failed: " + validateErr;
    }
    return false;
  }

  outConfig = parsed;
  return true;
}

bool readConfigFromSd(RuntimeConfig &outConfig,
                      bool &found,
                      String *error = nullptr) {
  found = false;

  String mountErr;
  if (!mountSd(&mountErr)) {
    return true;
  }

  if (!SD.exists(kSdConfigPath)) {
    return true;
  }

  File file = SD.open(kSdConfigPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "SD config open failed";
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  const auto parseErr = deserializeJson(doc, file);
  file.close();

  if (parseErr || !doc.is<JsonObject>()) {
    if (error) {
      *error = "SD config parse failed";
    }
    return false;
  }

  RuntimeConfig parsed = makeDefaultConfig();
  fromJson(doc.as<JsonObjectConst>(), parsed);

  String validateErr;
  if (!validateConfig(parsed, &validateErr)) {
    if (error) {
      *error = "SD config validation failed: " + validateErr;
    }
    return false;
  }

  outConfig = parsed;
  found = true;
  return true;
}

bool readConfigFromNvs(RuntimeConfig &outConfig,
                       bool &found,
                       String *error = nullptr) {
  found = false;

  Preferences prefs;
  // Open read-write so first boot can create namespace without noisy NOT_FOUND log.
  const bool opened = prefs.begin(kPrefsNamespace, false);
  if (!opened) {
    return true;
  }

  const uint32_t storedVersion = prefs.getULong(kConfigVersionKey, 0);
  const String blob = prefs.getString(kConfigBlobKey, "");
  prefs.end();

  if (storedVersion != kConfigVersion || blob.isEmpty()) {
    return true;
  }

  RuntimeConfig parsed = makeDefaultConfig();
  String parseErr;
  if (!parseConfigBlob(blob, parsed, &parseErr)) {
    if (error) {
      *error = "NVS " + parseErr;
    }
    return false;
  }

  outConfig = parsed;
  found = true;
  return true;
}

bool writeConfigToSd(const String &blob, String *error = nullptr) {
  String mountErr;
  if (!mountSd(&mountErr)) {
    if (error) {
      *error = mountErr;
    }
    return false;
  }

  if (SD.exists(kSdConfigTempPath)) {
    SD.remove(kSdConfigTempPath);
  }

  File temp = SD.open(kSdConfigTempPath, FILE_WRITE);
  if (!temp || temp.isDirectory()) {
    if (temp) {
      temp.close();
    }
    if (error) {
      *error = "SD temp write open failed";
    }
    return false;
  }

  const size_t written = temp.print(blob);
  temp.close();
  if (written != blob.length()) {
    SD.remove(kSdConfigTempPath);
    if (error) {
      *error = "SD write failed";
    }
    return false;
  }

  if (SD.exists(kSdConfigPath)) {
    SD.remove(kSdConfigPath);
  }
  if (!SD.rename(kSdConfigTempPath, kSdConfigPath)) {
    SD.remove(kSdConfigTempPath);
    if (error) {
      *error = "SD rename failed";
    }
    return false;
  }
  return true;
}

bool isHexDigitChar(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isValidBleAddress(const String &address) {
  if (address.isEmpty()) {
    return true;
  }
  if (address.length() != 17) {
    return false;
  }

  for (int i = 0; i < 17; ++i) {
    const char c = address[static_cast<unsigned int>(i)];
    if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
      if (c != ':') {
        return false;
      }
    } else if (!isHexDigitChar(c)) {
      return false;
    }
  }
  return true;
}

bool isValidGithubRepoSlug(const String &repoSlug) {
  if (repoSlug.isEmpty()) {
    return true;
  }

  const String value = trimAndUnquote(repoSlug);
  const int slash = value.indexOf('/');
  if (slash <= 0 || slash >= static_cast<int>(value.length()) - 1) {
    return false;
  }

  if (value.indexOf('/', static_cast<unsigned int>(slash + 1)) >= 0) {
    return false;
  }

  auto isValidPart = [](const String &part) {
    if (part.isEmpty()) {
      return false;
    }
    for (size_t i = 0; i < part.length(); ++i) {
      const char c = part[static_cast<unsigned int>(i)];
      const bool isAlnum = (c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z');
      if (!isAlnum && c != '.' && c != '_' && c != '-') {
        return false;
      }
    }
    return true;
  };

  const String owner = value.substring(0, static_cast<unsigned int>(slash));
  const String repo = value.substring(static_cast<unsigned int>(slash + 1));
  return isValidPart(owner) && isValidPart(repo);
}

bool isValidUiLanguageCode(const String &langCode) {
  if (langCode.isEmpty()) {
    return true;
  }

  String normalized = langCode;
  normalized.trim();
  normalized.toLowerCase();

  return normalized == "en" || normalized == "ko";
}

GatewayAuthMode sanitizeAuthMode(int mode) {
  return mode == 1 ? GatewayAuthMode::Password : GatewayAuthMode::Token;
}

uint8_t sanitizeDisplayBrightnessPercent(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return static_cast<uint8_t>(value);
}

void toJson(const RuntimeConfig &config, JsonObject obj) {
  obj["version"] = config.version;
  obj["deviceName"] = config.deviceName;
  obj["wifiSsid"] = config.wifiSsid;
  obj["wifiPassword"] = config.wifiPassword;
  obj["gatewayUrl"] = config.gatewayUrl;
  obj["gatewayAuthMode"] = static_cast<uint8_t>(config.gatewayAuthMode);
  obj["gatewayToken"] = config.gatewayToken;
  obj["gatewayPassword"] = config.gatewayPassword;
  obj["gatewayDeviceId"] = config.gatewayDeviceId;
  obj["gatewayDevicePublicKey"] = config.gatewayDevicePublicKey;
  obj["gatewayDevicePrivateKey"] = config.gatewayDevicePrivateKey;
  obj["gatewayDeviceToken"] = config.gatewayDeviceToken;
  obj["autoConnect"] = config.autoConnect;
  obj["bleDeviceAddress"] = config.bleDeviceAddress;
  obj["bleAutoConnect"] = config.bleAutoConnect;
  obj["appMarketGithubRepo"] = config.appMarketGithubRepo;
  obj["appMarketReleaseAsset"] = config.appMarketReleaseAsset;
  obj["uiLanguage"] = config.uiLanguage;
  obj["koreanFontInstalled"] = config.koreanFontInstalled;
  obj["timezoneTz"] = config.timezoneTz;
  obj["displayBrightnessPercent"] = config.displayBrightnessPercent;
}

void fromJson(const JsonObjectConst &obj, RuntimeConfig &config) {
  config.version = obj["version"] | kConfigVersion;
  if (obj.containsKey("deviceName")) {
    config.deviceName = String(static_cast<const char *>(obj["deviceName"] | ""));
  } else {
    config.deviceName = defaultDeviceNameValue();
  }
  config.deviceName = trimDeviceName(config.deviceName);
  if (config.deviceName.isEmpty()) {
    config.deviceName = defaultDeviceNameValue();
  }
  config.wifiSsid = String(static_cast<const char *>(obj["wifiSsid"] | ""));
  config.wifiPassword = String(static_cast<const char *>(obj["wifiPassword"] | ""));
  config.gatewayUrl = String(static_cast<const char *>(obj["gatewayUrl"] | ""));
  config.gatewayAuthMode = sanitizeAuthMode(obj["gatewayAuthMode"] | 0);
  config.gatewayToken = String(static_cast<const char *>(obj["gatewayToken"] | ""));
  config.gatewayPassword = String(static_cast<const char *>(obj["gatewayPassword"] | ""));
  config.gatewayDeviceId =
      String(static_cast<const char *>(obj["gatewayDeviceId"] | ""));
  config.gatewayDevicePublicKey =
      String(static_cast<const char *>(obj["gatewayDevicePublicKey"] | ""));
  config.gatewayDevicePrivateKey =
      String(static_cast<const char *>(obj["gatewayDevicePrivateKey"] | ""));
  config.gatewayDeviceToken =
      String(static_cast<const char *>(obj["gatewayDeviceToken"] | ""));
  config.autoConnect = obj["autoConnect"] | false;
  config.bleDeviceAddress = String(static_cast<const char *>(obj["bleDeviceAddress"] | ""));
  config.bleAutoConnect = obj["bleAutoConnect"] | false;
  config.appMarketGithubRepo =
      String(static_cast<const char *>(obj["appMarketGithubRepo"] |
                                       USER_APPMARKET_GITHUB_REPO));
  config.appMarketReleaseAsset =
      String(static_cast<const char *>(obj["appMarketReleaseAsset"] |
                                       USER_APPMARKET_RELEASE_ASSET));
  config.uiLanguage = String(static_cast<const char *>(obj["uiLanguage"] | "en"));
  config.koreanFontInstalled = obj["koreanFontInstalled"] | false;
  config.timezoneTz =
      String(static_cast<const char *>(obj["timezoneTz"] | USER_TIMEZONE_TZ));
  config.displayBrightnessPercent = sanitizeDisplayBrightnessPercent(
      obj["displayBrightnessPercent"] | USER_DISPLAY_BRIGHTNESS_PERCENT);
}

}  // namespace

RuntimeConfig makeDefaultConfig() {
  RuntimeConfig config;
  config.version = kConfigVersion;
  config.deviceName = defaultDeviceNameValue();

  if (!isPlaceholder(USER_WIFI_SSID)) {
    config.wifiSsid = USER_WIFI_SSID;
  }
  if (!isPlaceholder(USER_WIFI_PASSWORD)) {
    config.wifiPassword = USER_WIFI_PASSWORD;
  }

  if (!isPlaceholder(USER_GATEWAY_URL)) {
    config.gatewayUrl = USER_GATEWAY_URL;
  }

  config.gatewayAuthMode = sanitizeAuthMode(USER_GATEWAY_AUTH_MODE);
  if (!isPlaceholder(USER_GATEWAY_TOKEN)) {
    config.gatewayToken = USER_GATEWAY_TOKEN;
  }
  if (!isPlaceholder(USER_GATEWAY_PASSWORD)) {
    config.gatewayPassword = USER_GATEWAY_PASSWORD;
  }

  if (config.gatewayToken.isEmpty() && !config.gatewayPassword.isEmpty()) {
    config.gatewayAuthMode = GatewayAuthMode::Password;
  }

  config.autoConnect = USER_AUTO_CONNECT_DEFAULT;
  if (!isPlaceholder(USER_APPMARKET_GITHUB_REPO)) {
    config.appMarketGithubRepo = USER_APPMARKET_GITHUB_REPO;
  }
  if (!isPlaceholder(USER_APPMARKET_RELEASE_ASSET)) {
    config.appMarketReleaseAsset = USER_APPMARKET_RELEASE_ASSET;
  }
  config.uiLanguage = "en";
  config.timezoneTz = USER_TIMEZONE_TZ;
  config.displayBrightnessPercent =
      sanitizeDisplayBrightnessPercent(USER_DISPLAY_BRIGHTNESS_PERCENT);

  return config;
}

String effectiveDeviceName(const RuntimeConfig &config) {
  String name = trimDeviceName(config.deviceName);
  if (name.isEmpty()) {
    name = defaultDeviceNameValue();
  }
  if (name.length() > kRuntimeDeviceNameMaxLen) {
    name = name.substring(0, kRuntimeDeviceNameMaxLen);
  }
  return name;
}

bool hasGatewayCredentials(const RuntimeConfig &config) {
  if (!config.gatewayDeviceToken.isEmpty()) {
    return true;
  }
  if (config.gatewayAuthMode == GatewayAuthMode::Token) {
    return config.gatewayToken.length() > 0;
  }
  return config.gatewayPassword.length() > 0;
}

bool validateConfig(const RuntimeConfig &config, String *error) {
  String deviceName = trimDeviceName(config.deviceName);
  if (deviceName.isEmpty()) {
    if (error) {
      *error = "Device name cannot be empty";
    }
    return false;
  }

  if (deviceName.length() > kRuntimeDeviceNameMaxLen) {
    if (error) {
      *error = "Device name must be 1~31 chars";
    }
    return false;
  }

  if (config.wifiSsid.isEmpty() && !config.wifiPassword.isEmpty()) {
    if (error) {
      *error = "Wi-Fi password exists but SSID is empty";
    }
    return false;
  }

  if (!config.wifiPassword.isEmpty()) {
    const size_t passLen = config.wifiPassword.length();
    const bool is64Hex = passLen == 64 && isLikelyHexString(config.wifiPassword);
    if (passLen < 8) {
      if (error) {
        *error = "Wi-Fi password must be 8+ chars";
      }
      return false;
    }
    if (passLen > 63 && !is64Hex) {
      if (error) {
        *error = "Wi-Fi password must be 8~63 chars (or 64 hex)";
      }
      return false;
    }
  }

  if (!config.gatewayUrl.isEmpty()) {
    if (!startsWithWsScheme(config.gatewayUrl)) {
      if (error) {
        *error = "Gateway URL must start with ws:// or wss://";
      }
      return false;
    }

    if (!hasGatewayCredentials(config)) {
      if (error) {
        *error = "Gateway auth credential is missing";
      }
      return false;
    }
  }

  if (!isValidBleAddress(config.bleDeviceAddress)) {
    if (error) {
      *error = "BLE address format must be XX:XX:XX:XX:XX:XX";
    }
    return false;
  }

  if (!isValidGithubRepoSlug(config.appMarketGithubRepo)) {
    if (error) {
      *error = "APPMarket GitHub repo must be owner/repo";
    }
    return false;
  }

  if (!isValidUiLanguageCode(config.uiLanguage)) {
    if (error) {
      *error = "UI language must be en or ko";
    }
    return false;
  }

  if (config.timezoneTz.isEmpty()) {
    if (error) {
      *error = "Timezone cannot be empty";
    }
    return false;
  }

  if (config.displayBrightnessPercent > 100) {
    if (error) {
      *error = "Display brightness must be 0~100";
    }
    return false;
  }

  return true;
}

bool loadConfig(RuntimeConfig &outConfig,
                ConfigLoadSource *source,
                bool *loadedFromNvs,
                String *error) {
  RuntimeConfig config = makeDefaultConfig();
  outConfig = config;
  if (source) {
    *source = ConfigLoadSource::Defaults;
  }
  if (loadedFromNvs) {
    *loadedFromNvs = false;
  }
  String warnings;

  RuntimeConfig sdConfig = config;
  bool sdFound = false;
  String sdErr;
  if (readConfigFromSd(sdConfig, sdFound, &sdErr) && sdFound) {
    outConfig = sdConfig;
    if (source) {
      *source = ConfigLoadSource::SdCard;
    }
  } else {
    RuntimeConfig nvsConfig = config;
    bool nvsFound = false;
    String nvsErr;
    if (readConfigFromNvs(nvsConfig, nvsFound, &nvsErr) && nvsFound) {
      outConfig = nvsConfig;
      if (source) {
        *source = ConfigLoadSource::Nvs;
      }
      if (loadedFromNvs) {
        *loadedFromNvs = true;
      }
      if (!sdErr.isEmpty()) {
        appendMessage(warnings, sdErr + " (using NVS backup)");
      }
    } else {
      appendMessage(warnings, sdErr);
      appendMessage(warnings, nvsErr);
    }
  }

  EnvGatewayOverrides envOverrides;
  bool envFound = false;
  String envErr;
  if (!readEnvGatewayOverridesFromSd(envOverrides, envFound, &envErr)) {
    appendMessage(warnings, envErr);
  } else if (envFound) {
    RuntimeConfig envConfig = outConfig;
    applyEnvGatewayOverrides(envConfig, envOverrides);

    String envValidateErr;
    if (validateConfig(envConfig, &envValidateErr)) {
      outConfig = envConfig;
    } else {
      appendMessage(warnings, ".env ignored: " + envValidateErr);
    }
  }

  if (error) {
    *error = warnings;
  }

  return true;
}

bool saveConfig(const RuntimeConfig &config, String *error) {
  if (error) {
    error->clear();
  }

  String validateErr;
  if (!validateConfig(config, &validateErr)) {
    if (error) {
      *error = validateErr;
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  toJson(config, doc.to<JsonObject>());

  String blob;
  serializeJson(doc, blob);

  String sdErr;
  if (!writeConfigToSd(blob, &sdErr)) {
    if (error) {
      *error = "Failed to write config to SD: " + sdErr;
    }
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("[config] warning: failed to open NVS namespace for backup");
    return true;
  }

  const bool okVersion = prefs.putULong(kConfigVersionKey, kConfigVersion) > 0;
  const bool okBlob = prefs.putString(kConfigBlobKey, blob) > 0;
  prefs.end();

  if (!okVersion || !okBlob) {
    Serial.println("[config] warning: failed to write NVS backup");
  }

  return true;
}

bool resetConfig(String *error) {
  if (error) {
    error->clear();
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    if (error) {
      *error = "Failed to open NVS namespace";
    }
    return false;
  }

  const bool cleared = prefs.clear();
  prefs.end();

  if (!cleared) {
    if (error) {
      *error = "Failed to clear NVS config";
    }
    return false;
  }

  String mountErr;
  if (!mountSd(&mountErr)) {
    // SD card missing/unavailable: treat as reset complete for NVS.
    return true;
  }

  if (SD.exists(kSdConfigPath) && !SD.remove(kSdConfigPath)) {
    if (error) {
      *error = "Failed to remove SD config file";
    }
    return false;
  }
  if (SD.exists(kSdConfigTempPath) && !SD.remove(kSdConfigTempPath)) {
    if (error) {
      *error = "Failed to remove SD temp config file";
    }
    return false;
  }

  return true;
}

bool isKoreanFontInstalled(const RuntimeConfig &config) {
  return config.koreanFontInstalled;
}

bool mountSdCard(String *error) {
  return mountSd(error);
}

const char *gatewayAuthModeName(GatewayAuthMode mode) {
  return mode == GatewayAuthMode::Password ? "Password" : "Token";
}
