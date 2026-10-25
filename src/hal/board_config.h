#pragma once

// ============================================================================
// ZX-OS Hardware Abstraction Layer (HAL) - Board Configuration
// ============================================================================
//
// This header selects the correct board-specific pin map and capability flags
// based on the BOARD_* preprocessor define set in platformio.ini.
//
// To add a new board:
//   1. Create src/hal/boards/my_board.h  (copy an existing one as template)
//   2. Add a #elif for your BOARD_xxx define below
//   3. Add a [env:my-board] section in platformio.ini
//   4. Optionally add a boards/my-board.json PlatformIO board definition
// ============================================================================

#if defined(BOARD_T_EMBED_CC1101)
  #include "boards/t_embed_cc1101.h"
#elif defined(BOARD_T_DECK)
  #include "boards/t_deck.h"
#elif defined(BOARD_CARDPUTER)
  #include "boards/cardputer.h"
#elif defined(BOARD_CYD_2432S028)
  #include "boards/cyd_2432s028.h"
#elif defined(BOARD_ESP32_GENERIC)
  #include "boards/esp32_generic.h"
#else
  // Default: original LilyGo T-Embed CC1101
  #define BOARD_T_EMBED_CC1101
  #include "boards/t_embed_cc1101.h"
#endif

// ============================================================================
// Sanity checks - every board header must define these mandatory symbols.
// ============================================================================
#ifndef HAL_BOARD_NAME
  #error "Board header must define HAL_BOARD_NAME"
#endif
#ifndef HAL_MCU
  #error "Board header must define HAL_MCU"
#endif

// Default capability flags to 0 if the board header did not define them.
#ifndef HAL_HAS_DISPLAY
  #define HAL_HAS_DISPLAY 0
#endif
#ifndef HAL_HAS_CC1101
  #define HAL_HAS_CC1101 0
#endif
#ifndef HAL_HAS_ENCODER
  #define HAL_HAS_ENCODER 0
#endif
#ifndef HAL_HAS_BUTTONS
  #define HAL_HAS_BUTTONS 0
#endif
#ifndef HAL_HAS_KEYBOARD
  #define HAL_HAS_KEYBOARD 0
#endif
#ifndef HAL_HAS_TOUCH
  #define HAL_HAS_TOUCH 0
#endif
#ifndef HAL_HAS_SD_CARD
  #define HAL_HAS_SD_CARD 0
#endif
#ifndef HAL_HAS_PMU
  #define HAL_HAS_PMU 0
#endif
#ifndef HAL_HAS_BATTERY_GAUGE
  #define HAL_HAS_BATTERY_GAUGE 0
#endif
#ifndef HAL_HAS_SPEAKER
  #define HAL_HAS_SPEAKER 0
#endif
#ifndef HAL_HAS_MIC
  #define HAL_HAS_MIC 0
#endif
#ifndef HAL_HAS_ANTENNA_SWITCH
  #define HAL_HAS_ANTENNA_SWITCH 0
#endif
#ifndef HAL_HAS_POWER_ENABLE
  #define HAL_HAS_POWER_ENABLE 0
#endif
#ifndef HAL_HAS_IR
  #define HAL_HAS_IR 0
#endif

// Display defaults
#ifndef HAL_DISPLAY_WIDTH
  #define HAL_DISPLAY_WIDTH 0
#endif
#ifndef HAL_DISPLAY_HEIGHT
  #define HAL_DISPLAY_HEIGHT 0
#endif
#ifndef HAL_DISPLAY_ROTATION
  #define HAL_DISPLAY_ROTATION 3
#endif

// SPI bus defaults (board must override if it has SPI peripherals)
#ifndef HAL_SPI_SCK
  #define HAL_SPI_SCK -1
#endif
#ifndef HAL_SPI_MISO
  #define HAL_SPI_MISO -1
#endif
#ifndef HAL_SPI_MOSI
  #define HAL_SPI_MOSI -1
#endif

// I2C defaults
#ifndef HAL_I2C_SDA
  #define HAL_I2C_SDA -1
#endif
#ifndef HAL_I2C_SCL
  #define HAL_I2C_SCL -1
#endif

// Infrared (IR) transceiver defaults. Boards without a built-in IR LED /
// receiver leave these at -1; the IR app treats a negative pin as "not wired"
// and the user can override them for their own hardware in user_config.h.
#ifndef HAL_PIN_IR_TX
  #define HAL_PIN_IR_TX -1
#endif
#ifndef HAL_PIN_IR_RX
  #define HAL_PIN_IR_RX -1
#endif
