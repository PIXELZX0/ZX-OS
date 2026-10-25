#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_sleep.h>

#include "hal/board_config.h"

#if HAL_HAS_PMU
#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#endif

#include "apps/app_context.h"
#include "core/cc1101_radio.h"
#include "core/ble_manager.h"
#include "core/board_pins.h"
#include "core/gateway_client.h"
#include "core/node_command_handler.h"
#include "core/runtime_config.h"
#include "core/wifi_manager.h"
#include "ui/i18n.h"
#include "ui/ui_navigator.h"
#include "ui/ui_runtime.h"

// RTC memory persists across software resets (ESP.restart()), allowing the
// firmware to communicate a reboot reason to the next boot.
RTC_DATA_ATTR static char gRtcRebootReason[72] = {};
RTC_DATA_ATTR static bool gRtcRebootReasonSet = false;

namespace {

UiRuntime gUiRuntime;
UiNavigator gUiNav;
WifiManager gWifi;
GatewayClient gGateway;
BleManager gBle;
NodeCommandHandler gNodeHandler;
AppContext gAppContext;
#if HAL_HAS_PMU
XPowersPPM gPmu;
#endif
bool gSleepDetectionArmed = false;

constexpr unsigned long kDeepSleepHoldMs = 3000UL;
constexpr unsigned long kSleepReleaseDebounceMs = 80UL;
constexpr unsigned long kSleepReleasePollMs = 5UL;
constexpr unsigned long kRamWatchPollMs = 1000UL;
constexpr uint8_t kRamWatchRebootPercent = 100U;
constexpr uint8_t kBacklightFullDuty = 254U;
constexpr unsigned long kMemTraceLogMs = 5000UL;

// Returns a user-visible description for hardware-level reset reasons that
// indicate a system problem.  Returns nullptr for normal (non-problem) resets.
const char *systemProblemResetReason(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:    return "Crash (Panic)";
    case ESP_RST_INT_WDT:  return "Interrupt Watchdog Timeout";
    case ESP_RST_TASK_WDT: return "Task Watchdog Timeout";
    case ESP_RST_WDT:      return "Watchdog Timeout";
    case ESP_RST_BROWNOUT: return "Brownout (Low Voltage)";
    default:               return nullptr;
  }
}

void saveRtcRebootReason(const char *reason) {
  strncpy(gRtcRebootReason, reason, sizeof(gRtcRebootReason) - 1);
  gRtcRebootReason[sizeof(gRtcRebootReason) - 1] = '\0';
  gRtcRebootReasonSet = true;
}

void clearRtcRebootReason() {
  gRtcRebootReason[0] = '\0';
  gRtcRebootReasonSet = false;
}

uint8_t heapUsedPercent(uint32_t caps) {
  const uint32_t total = heap_caps_get_total_size(caps);
  if (total == 0U) {
    return 0U;
  }

  const uint32_t free = heap_caps_get_free_size(caps);
  const uint32_t used = free < total ? (total - free) : 0U;
  const uint32_t pct = (used * 100U) / total;
  return static_cast<uint8_t>(pct > 100U ? 100U : pct);
}

void tickRamWatchdog() {
  static unsigned long lastPollMs = 0;
  static unsigned long lastTraceLogMs = 0;

  const unsigned long now = millis();
  if (lastPollMs != 0 && now - lastPollMs < kRamWatchPollMs) {
    return;
  }
  lastPollMs = now;

  const uint8_t internalPct = heapUsedPercent(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint8_t psramPct = heapUsedPercent(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

#if USER_MEM_TRACE_ENABLED
  if (lastTraceLogMs == 0 || now - lastTraceLogMs >= kMemTraceLogMs) {
    lastTraceLogMs = now;
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internalLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t psramLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    Serial.printf(
        "[mem] int=%u%% free=%u largest=%u | psram=%u%% free=%u largest=%u\n",
        static_cast<unsigned int>(internalPct),
        static_cast<unsigned int>(internalFree),
        static_cast<unsigned int>(internalLargest),
        static_cast<unsigned int>(psramPct),
        static_cast<unsigned int>(psramFree),
        static_cast<unsigned int>(psramLargest));
  }
#endif

  if (internalPct < kRamWatchRebootPercent && psramPct < kRamWatchRebootPercent) {
    return;
  }

  Serial.printf("[ram] high usage detected (internal=%u%%, psram=%u%%) -> reboot\n",
                static_cast<unsigned int>(internalPct),
                static_cast<unsigned int>(psramPct));
  Serial.flush();
  saveRtcRebootReason("Out of Memory (RAM Watchdog)");
  ESP.restart();
}

// Deep sleep functions require a physical BACK button.
#if (HAL_HAS_ENCODER || HAL_HAS_BUTTONS) && defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0

void enableTopButtonWakeup() {
  const gpio_num_t wakePin = static_cast<gpio_num_t>(boardpins::kEncoderBack);
  rtc_gpio_init(wakePin);
  rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(wakePin);
  rtc_gpio_pulldown_dis(wakePin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(wakePin, 0);
}

void waitTopButtonReleased() {
  unsigned long releasedSince = 0;
  while (true) {
    const bool pressed = digitalRead(boardpins::kEncoderBack) == LOW;
    const unsigned long now = millis();

    if (pressed) {
      releasedSince = 0;
    } else if (releasedSince == 0) {
      releasedSince = now;
    } else if (now - releasedSince >= kSleepReleaseDebounceMs) {
      return;
    }

    delay(kSleepReleasePollMs);
  }
}

[[noreturn]] void enterDeepSleepNow() {
  Serial.println("[power] entering deep sleep");

  gGateway.disconnectNow();
  gBle.disconnectNow();
  gWifi.disconnect();

#if HAL_HAS_DISPLAY
  pinMode(boardpins::kTftBacklight, OUTPUT);
  analogWrite(boardpins::kTftBacklight, 0);
  digitalWrite(boardpins::kTftBacklight, LOW);
#endif

  // ext0 wake level is LOW, so arm wake only after the button is released.
  waitTopButtonReleased();
  enableTopButtonWakeup();
  delay(120);
  Serial.flush();
  esp_deep_sleep_start();

  while (true) {
    delay(1000);
  }
}

void tickDeepSleepButton() {
  static unsigned long pressedAtMs = 0;

  const bool pressed = digitalRead(boardpins::kEncoderBack) == LOW;
  const unsigned long now = millis();

  if (!gSleepDetectionArmed) {
    if (!pressed) {
      gSleepDetectionArmed = true;
    }
    return;
  }

  if (!pressed) {
    pressedAtMs = 0;
    return;
  }

  if (pressedAtMs == 0) {
    pressedAtMs = now;
    return;
  }

  if (now - pressedAtMs >= kDeepSleepHoldMs) {
    enterDeepSleepNow();
  }
}

#else
// No physical back button: deep sleep via button not available.
void tickDeepSleepButton() {}
#endif  // deep sleep button support

void runBackgroundTick() {
  tickDeepSleepButton();
  tickRamWatchdog();
  gWifi.tick();
  gGateway.tick();
  gBle.tick();
#if HAL_HAS_DISPLAY
  gUiRuntime.tick();
#endif
}

void configureGatewayCallbacks() {
  gGateway.setInvokeRequestHandler([](const String &invokeId,
                                      const String &nodeId,
                                      const String &command,
                                      JsonObjectConst params) {
    gNodeHandler.handleInvoke(invokeId, nodeId, command, params);
  });

  gGateway.setTelemetryBuilder([](JsonObject payload) {
    appendCc1101Info(payload);
    payload["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    payload["wifiRssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    payload["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    payload["uptimeMs"] = millis();
  });
}

void initBoardPower() {
#if HAL_HAS_POWER_ENABLE
  // Enable power rail for TFT/backlight/radio domain.
  pinMode(boardpins::kPowerEnable, OUTPUT);
  digitalWrite(boardpins::kPowerEnable, HIGH);
  delay(30);
#endif

#if HAL_HAS_DISPLAY
  // Ensure TFT backlight is enabled after cold boot/wakeup.
  pinMode(boardpins::kTftBacklight, OUTPUT);
  analogWrite(boardpins::kTftBacklight, kBacklightFullDuty);
#endif

#if HAL_HAS_PMU && defined(HAL_I2C_SDA) && HAL_I2C_SDA >= 0
  Wire.begin(HAL_I2C_SDA, HAL_I2C_SCL);
  if (gPmu.init(Wire, HAL_I2C_SDA, HAL_I2C_SCL, BQ25896_SLAVE_ADDRESS)) {
    gPmu.resetDefault();
    gPmu.setChargeTargetVoltage(4208);
    gPmu.enableMeasure(PowersBQ25896::CONTINUOUS);
    Serial.println("[boot] pmu ready");
  } else {
    Serial.println("[boot] pmu init failed");
  }
#elif defined(HAL_I2C_SDA) && HAL_I2C_SDA >= 0
  Wire.begin(HAL_I2C_SDA, HAL_I2C_SCL);
#endif
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("[boot] start | board: %s\n", HAL_BOARD_NAME);
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[boot] wake source: top button");
  }
  Serial.printf("[boot] reset reason: %d\n", static_cast<int>(resetReason));

  // Collect reboot reason to show after splash if applicable.
  // Priority: hardware crash reason > RTC-saved software reason.
  String rebootReasonMsg;
  const char *hwReason = systemProblemResetReason(resetReason);
  if (hwReason) {
    rebootReasonMsg = hwReason;
    clearRtcRebootReason();
  } else if (gRtcRebootReasonSet && gRtcRebootReason[0] != '\0') {
    rebootReasonMsg = gRtcRebootReason;
    clearRtcRebootReason();
  }

  // Keep shared SPI devices deselected before any peripheral init.
#if HAL_HAS_DISPLAY && defined(HAL_PIN_TFT_CS)
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_SD_CARD && defined(HAL_PIN_SD_CS)
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
#endif
#if HAL_HAS_CC1101 && defined(HAL_PIN_CC1101_CS)
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif

  initBoardPower();

#if HAL_HAS_DISPLAY
  Serial.println("[boot] ui.begin()");
  gUiRuntime.begin();
#endif

#if (HAL_HAS_ENCODER || HAL_HAS_BUTTONS) && defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
  gSleepDetectionArmed = digitalRead(boardpins::kEncoderBack) == HIGH;
#endif

  // Boot splash: show ZX-OS branding on every startup.
  // Waking from deep sleep skips the full splash to reduce latency.
#if HAL_HAS_DISPLAY
  if (wakeCause != ESP_SLEEP_WAKEUP_EXT0) {
    gUiRuntime.showBootSplash("", 1400, []() {});
  }

  // If the previous boot ended due to a system problem, show the reason.
  if (rebootReasonMsg.length() > 0) {
    Serial.printf("[boot] previous reboot reason: %s\n", rebootReasonMsg.c_str());
    gUiRuntime.showToast("Reboot Reason", rebootReasonMsg, 3000, []() {});
  }
#endif

#if HAL_HAS_CC1101
  Serial.println("[boot] cc1101.init()");
  const bool ccReady = initCc1101Radio();
#if HAL_HAS_DISPLAY
  if (!ccReady) {
    gUiRuntime.showToast("CC1101", "CC1101 not detected", 1500, []() {});
  }
#endif
  Serial.println(ccReady ? "[boot] cc1101 ready" : "[boot] cc1101 missing");
#else
  Serial.println("[boot] cc1101 not present on this board");
#endif

  ConfigLoadSource configLoadSource = ConfigLoadSource::Defaults;
  String loadErr;
  if (!loadConfig(gAppContext.config, &configLoadSource, nullptr, &loadErr)) {
    gAppContext.config = makeDefaultConfig();
  }
  Serial.printf("[boot] cfg.uiLanguage=%s koreanFont=%d\n",
                gAppContext.config.uiLanguage.c_str(),
                gAppContext.config.koreanFontInstalled ? 1 : 0);
#if HAL_HAS_DISPLAY
  const bool koreanFontReady =
      gUiRuntime.setKoreanFontInstalled(gAppContext.config.koreanFontInstalled);
  UiLanguage bootLanguage = uiLanguageFromConfigCode(gAppContext.config.uiLanguage);
  if (bootLanguage == UiLanguage::Korean && !koreanFontReady) {
    // Font pack missing from SD: fall back to English for this session only.
    bootLanguage = UiLanguage::English;
  }
  gUiRuntime.setLanguage(bootLanguage);
  gUiRuntime.setTimezone(gAppContext.config.timezoneTz);
  gUiRuntime.setDisplayBrightnessPercent(gAppContext.config.displayBrightnessPercent);
#endif

  gWifi.begin();
  gWifi.configure(gAppContext.config);

  gGateway.begin();
  gGateway.configure(gAppContext.config);
  configureGatewayCallbacks();

  gBle.configure(gAppContext.config);
  gBle.begin();

  gNodeHandler.setGatewayClient(&gGateway);

  gAppContext.wifi = &gWifi;
  gAppContext.gateway = &gGateway;
  gAppContext.ble = &gBle;
  gAppContext.uiRuntime = &gUiRuntime;
  gAppContext.uiNav = &gUiNav;
  gAppContext.configDirty = false;

  if (gAppContext.config.bleAutoConnect &&
      !gAppContext.config.bleDeviceAddress.isEmpty()) {
    gBle.connectToDevice(gAppContext.config.bleDeviceAddress,
                         effectiveDeviceName(gAppContext.config),
                         nullptr);
  }

#if HAL_HAS_DISPLAY
  if (!loadErr.isEmpty()) {
    gUiRuntime.showToast("Config", loadErr, 1800, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::SdCard) {
    gUiRuntime.showToast("Config", "Loaded from SD", 900, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::Nvs) {
    gUiRuntime.showToast("Config", "Loaded from NVS", 900, runBackgroundTick);
  } else {
    gUiRuntime.showToast("Config", "Using default seeds", 900, runBackgroundTick);
  }
#endif
}

void loop() {
#if HAL_HAS_DISPLAY
  gUiNav.runLauncher(gAppContext, runBackgroundTick);
#else
  // Headless mode: just run background services.
  runBackgroundTick();
  delay(10);
#endif
}
