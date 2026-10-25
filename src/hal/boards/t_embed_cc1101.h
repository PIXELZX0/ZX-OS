#pragma once

// ============================================================================
// LilyGo T-Embed CC1101
// MCU: ESP32-S3  |  Flash: 16 MB  |  PSRAM: 8 MB
// Display: ST7789 170x320  |  Input: Rotary encoder + 2 buttons
// RF: Built-in CC1101  |  PMU: BQ25896  |  Battery: BQ27220
// ============================================================================

#define HAL_BOARD_NAME        "LilyGo T-Embed CC1101"
#define HAL_MCU               "ESP32-S3"

// --- Capabilities ---
#define HAL_HAS_DISPLAY       1
#define HAL_HAS_CC1101        1
#define HAL_HAS_ENCODER       1
#define HAL_HAS_BUTTONS       1
#define HAL_HAS_SD_CARD       1
#define HAL_HAS_PMU           1
#define HAL_HAS_BATTERY_GAUGE 1
#define HAL_HAS_SPEAKER       1
#define HAL_HAS_MIC           1
#define HAL_HAS_ANTENNA_SWITCH 1
#define HAL_HAS_POWER_ENABLE  1
#define HAL_HAS_IR            1

// --- Power ---
#define HAL_PIN_POWER_ENABLE  15

// --- Display (ST7789, SPI) ---
#define HAL_DISPLAY_WIDTH     170
#define HAL_DISPLAY_HEIGHT    320
#define HAL_DISPLAY_ROTATION  3
#define HAL_PIN_TFT_CS        41
#define HAL_PIN_TFT_DC        16
#define HAL_PIN_TFT_RST       40
#define HAL_PIN_TFT_BACKLIGHT 21
#define HAL_TFT_INVERSION_ON  1

// --- Shared SPI Bus (TFT + SD + CC1101) ---
#define HAL_SPI_SCK           11
#define HAL_SPI_MISO          10
#define HAL_SPI_MOSI          9
#define HAL_SPI_FREQUENCY     40000000
#define HAL_SPI_READ_FREQ     20000000

// --- SD Card ---
#define HAL_PIN_SD_CS         13

// --- CC1101 RF ---
#define HAL_PIN_CC1101_CS     12
#define HAL_PIN_CC1101_GDO0   3
#define HAL_PIN_CC1101_SW1    47
#define HAL_PIN_CC1101_SW0    48

// --- Rotary Encoder + Buttons ---
#define HAL_PIN_ENCODER_A     4
#define HAL_PIN_ENCODER_B     5
#define HAL_PIN_BTN_OK        0
#define HAL_PIN_BTN_BACK      6

// --- I2C (PMU + Battery Gauge + NFC) ---
#define HAL_I2C_SDA           8
#define HAL_I2C_SCL           18

// --- PMU (BQ25896) ---
#define HAL_PMU_TYPE          "BQ25896"

// --- Battery Gauge (BQ27220) ---
#define HAL_BATTERY_GAUGE_ADDR    0x55
#define HAL_BATTERY_GAUGE_SOC_REG 0x2C

// --- Audio: PDM Microphone ---
#define HAL_PIN_MIC_PDM_DATA  42
#define HAL_PIN_MIC_PDM_CLK   39

// --- Audio: I2S Speaker ---
#define HAL_PIN_I2S_BCLK      46
#define HAL_PIN_I2S_LRCLK     40
#define HAL_PIN_I2S_DOUT      7

// --- External modules (optional wiring) ---
#define HAL_PIN_NFC_IRQ       17
#define HAL_PIN_NFC_RST       45
#define HAL_PIN_RFID_SS       2
#define HAL_PIN_RFID_RST      1
#define HAL_PIN_NRF24_CE      17
#define HAL_PIN_NRF24_CSN     14

// --- Infrared (IR) transceiver (optional external wiring on the GROVE/UART header) ---
// The base T-Embed CC1101 has no IR hardware. Wire an IR LED (via a transistor)
// and a 38 kHz IR receiver to these pins, or override them in user_config.h.
#define HAL_PIN_IR_TX         44
#define HAL_PIN_IR_RX         43
