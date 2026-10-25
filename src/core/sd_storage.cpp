#include "sd_storage.h"

#include <SD.h>

#include "board_pins.h"
#include "shared_spi_bus.h"
#include "../hal/board_config.h"

namespace sdstorage {

bool ensureMounted(String *errorOut) {
#if HAL_HAS_SD_CARD
  if (SD.cardType() != CARD_NONE) {
    return true;
  }

#if HAL_HAS_DISPLAY && defined(HAL_PIN_TFT_CS)
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_CC1101
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = sharedspi::bus();
  if (!SD.begin(boardpins::kSdCs, *spiBus, 25000000, "/sd", 8, false)) {
    if (errorOut) {
      *errorOut = "SD mount failed";
    }
    return false;
  }
  return true;
#else
  if (errorOut) {
    *errorOut = "SD card not available on this board";
  }
  return false;
#endif
}

bool isSafePath(const String &path) {
  if (path.length() == 0 || path.length() > 200 || path[0] != '/') {
    return false;
  }
  if (path.indexOf("..") >= 0 || path.indexOf('\\') >= 0) {
    return false;
  }
  return true;
}

}  // namespace sdstorage
