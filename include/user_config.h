#pragma once

// Include HAL board config first so board-specific pin defines are available.
#include "hal/board_config.h"

// ============================================================================
// ZX-OS User Configuration
// ============================================================================
// These are compile-time default seeds. Values are used only when NVS has
// no saved config. Board-specific hardware pins are now defined in the HAL
// board headers (src/hal/boards/*.h) and should NOT be changed here.
// ============================================================================

// --- WiFi ---
#define USER_WIFI_SSID "REPLACE_WITH_WIFI_SSID"
#define USER_WIFI_PASSWORD "REPLACE_WITH_WIFI_PASSWORD"

// --- OpenClaw Gateway ---
// ws://host:port or wss://host:port
#define USER_GATEWAY_URL "REPLACE_WITH_GATEWAY_URL"
#define USER_GATEWAY_TOKEN "REPLACE_WITH_GATEWAY_TOKEN"
#define USER_GATEWAY_PASSWORD ""
// Default auth mode seed: 0=token, 1=password
#define USER_GATEWAY_AUTH_MODE 0

// --- APPMarket ---
#define USER_APPMARKET_GITHUB_REPO "HITEYY/AI-cc1101"
#define USER_APPMARKET_RELEASE_ASSET "openclaw-t-embed-cc1101-latest.bin"

// --- OpenClaw Node Identity ---
// These defaults adapt to the active board via HAL_BOARD_NAME at runtime.
// The macros below are compile-time fallbacks only.
#define USER_OPENCLAW_DISPLAY_NAME "ZX-OS Device"
#define USER_OPENCLAW_INSTANCE_ID "zx-os-device"
#define USER_OPENCLAW_DEFAULT_AGENT_ID "default"
#define USER_MESSENGER_ENABLE_LEGACY_MEDIA_FALLBACK 0
#define USER_MESSENGER_BINARY_ATTACH_MAX_BYTES 524288U
#define USER_MESSENGER_TEXT_FALLBACK_PREVIEW_MAX_CHARS 4000U

// --- CC1101 defaults ---
#define USER_DEFAULT_RF_FREQUENCY_MHZ 433.92f

// --- Voice recording ---
#define USER_MIC_ADC_PIN -1
// PDM mic pins: board HAL provides defaults; these are fallbacks.
#ifndef HAL_PIN_MIC_PDM_DATA
  #define USER_MIC_PDM_DATA_PIN -1
#else
  #define USER_MIC_PDM_DATA_PIN HAL_PIN_MIC_PDM_DATA
#endif
#ifndef HAL_PIN_MIC_PDM_CLK
  #define USER_MIC_PDM_CLK_PIN -1
#else
  #define USER_MIC_PDM_CLK_PIN HAL_PIN_MIC_PDM_CLK
#endif
#define USER_MIC_SAMPLE_RATE 8000U
#define USER_MIC_DEFAULT_SECONDS 5U
#define USER_MIC_MAX_SECONDS 30U
#define USER_BLE_AUDIO_SERVICE_UUID ""
#define USER_BLE_AUDIO_CHAR_UUID ""

// --- Audio playback (I2S) ---
#ifndef HAL_PIN_I2S_BCLK
  #define USER_AUDIO_I2S_BCLK_PIN -1
#else
  #define USER_AUDIO_I2S_BCLK_PIN HAL_PIN_I2S_BCLK
#endif
#ifndef HAL_PIN_I2S_LRCLK
  #define USER_AUDIO_I2S_LRCLK_PIN -1
#else
  #define USER_AUDIO_I2S_LRCLK_PIN HAL_PIN_I2S_LRCLK
#endif
#ifndef HAL_PIN_I2S_DOUT
  #define USER_AUDIO_I2S_DOUT_PIN -1
#else
  #define USER_AUDIO_I2S_DOUT_PIN HAL_PIN_I2S_DOUT
#endif
#define USER_AUDIO_PLAYBACK_VOLUME 12

// --- Infrared (-1 = not present) ---
#ifndef HAL_PIN_IR_TX
  #define USER_IR_TX_PIN -1
#else
  #define USER_IR_TX_PIN HAL_PIN_IR_TX
#endif
#ifndef HAL_PIN_IR_RX
  #define USER_IR_RX_PIN -1
#else
  #define USER_IR_RX_PIN HAL_PIN_IR_RX
#endif

// --- External modules (override per your wiring) ---
#ifndef HAL_I2C_SDA
  #define USER_NFC_I2C_SDA -1
  #define USER_NFC_I2C_SCL -1
#else
  #define USER_NFC_I2C_SDA HAL_I2C_SDA
  #define USER_NFC_I2C_SCL HAL_I2C_SCL
#endif
#ifndef HAL_PIN_NFC_IRQ
  #define USER_NFC_IRQ_PIN -1
#else
  #define USER_NFC_IRQ_PIN HAL_PIN_NFC_IRQ
#endif
#ifndef HAL_PIN_NFC_RST
  #define USER_NFC_RESET_PIN -1
#else
  #define USER_NFC_RESET_PIN HAL_PIN_NFC_RST
#endif
#ifndef HAL_PIN_RFID_SS
  #define USER_RFID_SS_PIN -1
#else
  #define USER_RFID_SS_PIN HAL_PIN_RFID_SS
#endif
#ifndef HAL_PIN_RFID_RST
  #define USER_RFID_RST_PIN -1
#else
  #define USER_RFID_RST_PIN HAL_PIN_RFID_RST
#endif
#ifndef HAL_PIN_NRF24_CE
  #define USER_NRF24_CE_PIN -1
#else
  #define USER_NRF24_CE_PIN HAL_PIN_NRF24_CE
#endif
#ifndef HAL_PIN_NRF24_CSN
  #define USER_NRF24_CSN_PIN -1
#else
  #define USER_NRF24_CSN_PIN HAL_PIN_NRF24_CSN
#endif
#define USER_NRF24_CHANNEL 76
#define USER_NRF24_DATA_RATE 1
#define USER_NRF24_PA_LEVEL 1

// --- Telemetry ---
#define USER_TELEMETRY_INTERVAL_MS 30000UL
#define USER_AUTO_CONNECT_DEFAULT false

// --- Clock (NTP) ---
#define USER_TIMEZONE_TZ "UTC0"
#define USER_NTP_SERVER_1 "pool.ntp.org"
#define USER_NTP_SERVER_2 "time.nist.gov"
#define USER_UNIX_TIME_SERVER_URL "http://worldtimeapi.org/api/timezone/Etc/UTC"

// --- Display ---
#define USER_DISPLAY_BRIGHTNESS_PERCENT 100

// --- Debug ---
#define USER_MEM_TRACE_ENABLED 0
#define USER_INPUT_TRACE_ENABLED 0

// --- Input pins (encoder defaults from HAL, override here if needed) ---
#ifndef HAL_PIN_ENCODER_A
  #define USER_ENCODER_A_PIN 4
#else
  #define USER_ENCODER_A_PIN HAL_PIN_ENCODER_A
#endif
#ifndef HAL_PIN_ENCODER_B
  #define USER_ENCODER_B_PIN 5
#else
  #define USER_ENCODER_B_PIN HAL_PIN_ENCODER_B
#endif
#ifndef HAL_PIN_BTN_OK
  #define USER_ENCODER_OK_PIN 0
#else
  #define USER_ENCODER_OK_PIN HAL_PIN_BTN_OK
#endif
#ifndef HAL_PIN_BTN_BACK
  #define USER_ENCODER_BACK_PIN 6
#else
  #define USER_ENCODER_BACK_PIN HAL_PIN_BTN_BACK
#endif

// --- Battery gauge ---
#ifndef HAL_HAS_BATTERY_GAUGE
  #define USER_BATTERY_GAUGE_ENABLED 0
#elif HAL_HAS_BATTERY_GAUGE
  #define USER_BATTERY_GAUGE_ENABLED 1
#else
  #define USER_BATTERY_GAUGE_ENABLED 0
#endif
#ifndef HAL_BATTERY_GAUGE_ADDR
  #define USER_BATTERY_GAUGE_ADDR 0x55
#else
  #define USER_BATTERY_GAUGE_ADDR HAL_BATTERY_GAUGE_ADDR
#endif
#ifndef HAL_BATTERY_GAUGE_SOC_REG
  #define USER_BATTERY_GAUGE_SOC_REG 0x2C
#else
  #define USER_BATTERY_GAUGE_SOC_REG HAL_BATTERY_GAUGE_SOC_REG
#endif
#ifndef HAL_I2C_SDA
  #define USER_BATTERY_GAUGE_SDA -1
  #define USER_BATTERY_GAUGE_SCL -1
#else
  #define USER_BATTERY_GAUGE_SDA HAL_I2C_SDA
  #define USER_BATTERY_GAUGE_SCL HAL_I2C_SCL
#endif
