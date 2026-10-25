#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

// Thin, script-friendly layer over the PN532 (I2C) used by the NFC app.
namespace nfcreader {

using KeepWaiting = std::function<bool()>;

// True when the Adafruit_PN532 library is part of the build.
bool compiledIn();

// Initialises the I2C bus and PN532 once; later calls return the cached state.
bool begin(String &errorOut);
bool isReady();
bool firmware(uint8_t &ic, uint8_t &version, uint8_t &revision);

// ISO14443A (106 kbps) poll. Returns false with empty errorOut on timeout.
bool scan(uint32_t timeoutMs,
          std::vector<uint8_t> &uid,
          String &errorOut,
          const KeepWaiting &keepWaiting = KeepWaiting());

// MIFARE Classic (the tag must have been selected by scan() first).
bool mifareAuth(const std::vector<uint8_t> &uid,
                uint32_t block,
                bool useKeyB,
                const uint8_t key[6],
                String &errorOut);
bool mifareRead(uint8_t block, uint8_t out[16], String &errorOut);
bool mifareWrite(uint8_t block, const uint8_t data[16], String &errorOut);

// MIFARE Ultralight / NTAG: 4-byte pages.
bool pageRead(uint8_t page, uint8_t out[4], String &errorOut);
bool pageWrite(uint8_t page, const uint8_t data[4], String &errorOut);

// Raw InDataExchange to the selected target (APDU, NTAG commands, ...).
bool transceive(const uint8_t *data,
                size_t size,
                std::vector<uint8_t> &response,
                String &errorOut);

}  // namespace nfcreader
