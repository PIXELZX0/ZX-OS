#pragma once

#include <Arduino.h>

#include <functional>

#include "app_context.h"

void runFirmwareUpdateApp(AppContext &ctx,
                          const std::function<void()> &backgroundTick);

// Downloads the Korean font pack from the latest release onto the SD card
// (sdfont::kKoreanFontPath), verifying its published SHA-256.
bool installKoreanFontPack(AppContext &ctx,
                           const std::function<void()> &backgroundTick,
                           String *error);
