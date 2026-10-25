#include "nfc_reader.h"

#include <Wire.h>

#include "user_config.h"

#if __has_include(<Adafruit_PN532.h>)
#include <Adafruit_PN532.h>
#define NFCREADER_PN532 1
#else
#define NFCREADER_PN532 0
#endif

namespace nfcreader {

#if NFCREADER_PN532

namespace {

constexpr uint32_t kPollSliceMs = 120;
constexpr size_t kMaxTransceive = 250;

Adafruit_PN532 gPn532(USER_NFC_IRQ_PIN, USER_NFC_RESET_PIN, &Wire);
bool gInited = false;
bool gPresent = false;
uint32_t gFirmware = 0;

}  // namespace

bool compiledIn() { return true; }

bool begin(String &errorOut) {
  if (gInited) {
    if (!gPresent) {
      errorOut = "PN532 not detected";
    }
    return gPresent;
  }
  if (USER_NFC_I2C_SDA < 0 || USER_NFC_I2C_SCL < 0) {
    errorOut = "NFC I2C pins not configured for this board";
    return false;
  }
  Wire.begin(USER_NFC_I2C_SDA, USER_NFC_I2C_SCL);
  gPn532.begin();
  gFirmware = gPn532.getFirmwareVersion();
  gPresent = gFirmware != 0;
  gInited = true;
  if (!gPresent) {
    errorOut = "PN532 not detected";
    return false;
  }
  gPn532.SAMConfig();
  return true;
}

bool isReady() { return gInited && gPresent; }

bool firmware(uint8_t &ic, uint8_t &version, uint8_t &revision) {
  if (!isReady()) {
    return false;
  }
  ic = (gFirmware >> 24) & 0xFF;
  version = (gFirmware >> 16) & 0xFF;
  revision = (gFirmware >> 8) & 0xFF;
  return true;
}

bool scan(uint32_t timeoutMs,
          std::vector<uint8_t> &uid,
          String &errorOut,
          const KeepWaiting &keepWaiting) {
  uid.clear();
  if (!begin(errorOut)) {
    return false;
  }
  const uint32_t startMs = millis();
  do {
    if (keepWaiting && !keepWaiting()) {
      return false;
    }
    uint8_t buf[10] = {0};
    uint8_t len = 0;
    if (gPn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &len,
                                   kPollSliceMs) &&
        len > 0) {
      uid.assign(buf, buf + len);
      return true;
    }
  } while (millis() - startMs < timeoutMs);
  return false;
}

bool mifareAuth(const std::vector<uint8_t> &uid,
                uint32_t block,
                bool useKeyB,
                const uint8_t key[6],
                String &errorOut) {
  if (!begin(errorOut)) {
    return false;
  }
  if (uid.empty() || uid.size() > 7) {
    errorOut = "invalid uid";
    return false;
  }
  uint8_t uidCopy[7] = {0};
  memcpy(uidCopy, uid.data(), uid.size());
  uint8_t keyCopy[6];
  memcpy(keyCopy, key, 6);
  if (!gPn532.mifareclassic_AuthenticateBlock(uidCopy, uid.size(), block,
                                              useKeyB ? 1 : 0, keyCopy)) {
    errorOut = "authentication failed";
    return false;
  }
  return true;
}

bool mifareRead(uint8_t block, uint8_t out[16], String &errorOut) {
  if (!begin(errorOut)) {
    return false;
  }
  if (!gPn532.mifareclassic_ReadDataBlock(block, out)) {
    errorOut = "read failed (authenticate the sector first)";
    return false;
  }
  return true;
}

bool mifareWrite(uint8_t block, const uint8_t data[16], String &errorOut) {
  if (!begin(errorOut)) {
    return false;
  }
  uint8_t copy[16];
  memcpy(copy, data, 16);
  if (!gPn532.mifareclassic_WriteDataBlock(block, copy)) {
    errorOut = "write failed (authenticate the sector first)";
    return false;
  }
  return true;
}

bool pageRead(uint8_t page, uint8_t out[4], String &errorOut) {
  if (!begin(errorOut)) {
    return false;
  }
  // The library reads a 4-byte page into a buffer of at least 4 bytes.
  if (!gPn532.mifareultralight_ReadPage(page, out)) {
    errorOut = "page read failed";
    return false;
  }
  return true;
}

bool pageWrite(uint8_t page, const uint8_t data[4], String &errorOut) {
  if (!begin(errorOut)) {
    return false;
  }
  uint8_t copy[4];
  memcpy(copy, data, 4);
  if (!gPn532.mifareultralight_WritePage(page, copy)) {
    errorOut = "page write failed";
    return false;
  }
  return true;
}

bool transceive(const uint8_t *data,
                size_t size,
                std::vector<uint8_t> &response,
                String &errorOut) {
  response.clear();
  if (!begin(errorOut)) {
    return false;
  }
  if (!data || size == 0 || size > kMaxTransceive) {
    errorOut = "payload must be 1.." + String(kMaxTransceive) + " bytes";
    return false;
  }
  uint8_t out[kMaxTransceive];
  memcpy(out, data, size);
  uint8_t resp[kMaxTransceive];
  uint8_t respLen = sizeof(resp);
  if (!gPn532.inDataExchange(out, static_cast<uint8_t>(size), resp,
                             &respLen)) {
    errorOut = "no response from tag";
    return false;
  }
  response.assign(resp, resp + respLen);
  return true;
}

#else  // !NFCREADER_PN532

bool compiledIn() { return false; }
bool begin(String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool isReady() { return false; }
bool firmware(uint8_t &, uint8_t &, uint8_t &) { return false; }
bool scan(uint32_t, std::vector<uint8_t> &, String &errorOut,
          const KeepWaiting &) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool mifareAuth(const std::vector<uint8_t> &, uint32_t, bool, const uint8_t *,
                String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool mifareRead(uint8_t, uint8_t *, String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool mifareWrite(uint8_t, const uint8_t *, String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool pageRead(uint8_t, uint8_t *, String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool pageWrite(uint8_t, const uint8_t *, String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}
bool transceive(const uint8_t *, size_t, std::vector<uint8_t> &,
                String &errorOut) {
  errorOut = "Adafruit_PN532 not available in this build";
  return false;
}

#endif

}  // namespace nfcreader
