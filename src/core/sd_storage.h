#pragma once

#include <Arduino.h>

namespace sdstorage {

// Mounts the shared-SPI SD card on first use. Safe to call repeatedly.
bool ensureMounted(String *errorOut = nullptr);

// Accepts absolute paths without ".." segments (e.g. "/scripts/a.js").
bool isSafePath(const String &path);

}  // namespace sdstorage
