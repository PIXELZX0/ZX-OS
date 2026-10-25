#include <Arduino.h>

#include <string.h>

#include "../board_pins.h"
#include "../hal/board_config.h"
#include "js_internal.h"

// gpio.* lets scripts drive spare pins directly. Pins the firmware itself
// depends on (display/SD/CC1101 SPI, encoder, buttons, I2C, IR, NFC/RFID/
// NRF24) are blocked so a script cannot brick the shared SPI bus or UI input.
namespace zxjs {

namespace {

bool isReserved(int pin) {
  // -1 entries mean "pin not present on this board" and must not be treated
  // as a sentinel: the array is scanned to its end by sizeof, not by value.
  static const int reserved[] = {
#if defined(HAL_PIN_TFT_CS)
      HAL_PIN_TFT_CS,
#endif
#if defined(HAL_PIN_TFT_DC)
      HAL_PIN_TFT_DC,
#endif
#if defined(HAL_PIN_TFT_RST)
      HAL_PIN_TFT_RST,
#endif
#if defined(HAL_PIN_TFT_BACKLIGHT)
      HAL_PIN_TFT_BACKLIGHT,
#endif
#if HAL_HAS_SD_CARD
      HAL_PIN_SD_CS,
#endif
#if HAL_HAS_CC1101
      HAL_PIN_CC1101_CS, HAL_PIN_CC1101_GDO0,
#endif
#if defined(HAL_SPI_SCK) && HAL_SPI_SCK >= 0
      HAL_SPI_SCK, HAL_SPI_MISO, HAL_SPI_MOSI,
#endif
#if HAL_HAS_ENCODER
      HAL_PIN_ENCODER_A, HAL_PIN_ENCODER_B,
#endif
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
      HAL_PIN_BTN_OK,
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
      HAL_PIN_BTN_BACK,
#endif
#if defined(HAL_I2C_SDA) && HAL_I2C_SDA >= 0
      HAL_I2C_SDA, HAL_I2C_SCL,
#endif
#if HAL_HAS_IR
      HAL_PIN_IR_TX, HAL_PIN_IR_RX,
#endif
#if defined(HAL_HAS_POWER_ENABLE) && HAL_HAS_POWER_ENABLE
      HAL_PIN_POWER_ENABLE,
#endif
      -1,  // harmless padding so the array is never empty; checkPin() already
           // rejects pin < 0 before this runs.
  };
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    if (reserved[i] == pin) {
      return true;
    }
  }
  return false;
}

bool checkPin(duk_context *ctx, int pin) {
  if (pin < 0 || pin > 48) {
    duk_error(ctx, DUK_ERR_RANGE_ERROR, "pin must be 0..48");
    return false;
  }
  if (isReserved(pin)) {
    duk_error(ctx, DUK_ERR_ERROR,
              "GPIO %d is reserved by the firmware (display/SD/radio/input)",
              pin);
    return false;
  }
  return true;
}

duk_ret_t gpioMode(duk_context *ctx) {
  const int pin = duk_require_int(ctx, 0);
  const char *mode = duk_require_string(ctx, 1);
  if (!checkPin(ctx, pin)) {
    return 0;
  }
  if (strcmp(mode, "input") == 0) {
    pinMode(pin, INPUT);
  } else if (strcmp(mode, "input_pullup") == 0) {
    pinMode(pin, INPUT_PULLUP);
  } else if (strcmp(mode, "input_pulldown") == 0) {
    pinMode(pin, INPUT_PULLDOWN);
  } else if (strcmp(mode, "output") == 0) {
    pinMode(pin, OUTPUT);
  } else {
    return duk_error(ctx, DUK_ERR_ERROR,
                     "mode must be input, input_pullup, input_pulldown or "
                     "output");
  }
  return 0;
}

duk_ret_t gpioWrite(duk_context *ctx) {
  const int pin = duk_require_int(ctx, 0);
  const int value = duk_require_int(ctx, 1);
  if (!checkPin(ctx, pin)) {
    return 0;
  }
  digitalWrite(pin, value ? HIGH : LOW);
  return 0;
}

duk_ret_t gpioRead(duk_context *ctx) {
  const int pin = duk_require_int(ctx, 0);
  if (!checkPin(ctx, pin)) {
    return 0;
  }
  duk_push_int(ctx, digitalRead(pin));
  return 1;
}

duk_ret_t gpioAnalogRead(duk_context *ctx) {
  const int pin = duk_require_int(ctx, 0);
  if (!checkPin(ctx, pin)) {
    return 0;
  }
  duk_push_int(ctx, analogRead(pin));
  return 1;
}

duk_ret_t gpioPwm(duk_context *ctx) {
  const int pin = duk_require_int(ctx, 0);
  const double duty01 = duk_require_number(ctx, 1);
  const int hz = duk_opt_int(ctx, 2, 1000);
  if (!checkPin(ctx, pin)) {
    return 0;
  }
  if (hz < 1 || hz > 40000000) {
    return duk_error(ctx, DUK_ERR_RANGE_ERROR, "frequency out of range");
  }
  if (!ledcAttach(pin, hz, 10)) {
    return duk_error(ctx, DUK_ERR_ERROR, "ledcAttach failed");
  }
  double duty = duty01;
  if (duty < 0.0) duty = 0.0;
  if (duty > 1.0) duty = 1.0;
  ledcWrite(pin, static_cast<uint32_t>(duty * 1023.0));
  return 0;
}

}  // namespace

void registerGpioModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "mode", gpioMode, 2);
  putFunction(ctx, "write", gpioWrite, 2);
  putFunction(ctx, "read", gpioRead, 1);
  putFunction(ctx, "analogRead", gpioAnalogRead, 1);
  putFunction(ctx, "pwm", gpioPwm, 3);
  duk_put_prop_string(ctx, -2, "gpio");
  duk_pop(ctx);
}

}  // namespace zxjs
