#pragma once

#include <Arduino.h>
#include "user_config.h"

enum class GatewayAuthMode : uint8_t {
  Token = 0,
  Password = 1,
};

constexpr size_t kRuntimeDeviceNameMaxLen = 31;

struct RuntimeConfig {
  uint32_t version = 2;
  String deviceName;
  String wifiSsid;
  String wifiPassword;
  String gatewayUrl;
  GatewayAuthMode gatewayAuthMode = GatewayAuthMode::Token;
  String gatewayToken;
  String gatewayPassword;
  String gatewayDeviceId;
  String gatewayDevicePublicKey;
  String gatewayDevicePrivateKey;
  String gatewayDeviceToken;
  bool autoConnect = false;
  String bleDeviceAddress;
  bool bleAutoConnect = false;
  String appMarketGithubRepo;
  String appMarketReleaseAsset;
  String uiLanguage = "en";
  bool koreanFontInstalled = false;
  String timezoneTz = "UTC0";
  uint8_t displayBrightnessPercent = USER_DISPLAY_BRIGHTNESS_PERCENT;
};

enum class ConfigLoadSource : uint8_t {
  Defaults = 0,
  SdCard = 1,
  Nvs = 2,
};

RuntimeConfig makeDefaultConfig();
String effectiveDeviceName(const RuntimeConfig &config);

bool validateConfig(const RuntimeConfig &config, String *error = nullptr);
bool hasGatewayCredentials(const RuntimeConfig &config);

bool loadConfig(RuntimeConfig &outConfig,
                ConfigLoadSource *source = nullptr,
                bool *loadedFromNvs = nullptr,
                String *error = nullptr);
bool saveConfig(const RuntimeConfig &config, String *error = nullptr);
bool resetConfig(String *error = nullptr);

bool isKoreanFontInstalled(const RuntimeConfig &config);

// Mounts the SD card on the shared SPI bus (no-op when already mounted).
bool mountSdCard(String *error = nullptr);

const char *gatewayAuthModeName(GatewayAuthMode mode);
