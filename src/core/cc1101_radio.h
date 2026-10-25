#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>
#include <vector>

enum class Cc1101Modulation : uint8_t {
  Fsk2 = 0,
  Gfsk = 1,
  AskOok = 2,
  Fsk4 = 3,
  Msk = 4,
};

struct Cc1101PacketConfig {
  uint8_t modulation = static_cast<uint8_t>(Cc1101Modulation::AskOok);
  uint8_t channel = 0;
  float dataRateKbps = 4.8f;
  float deviationKHz = 5.0f;
  float rxBandwidthKHz = 256.0f;
  uint8_t syncMode = 2;
  uint8_t packetFormat = 0;
  bool crcEnabled = true;
  uint8_t lengthConfig = 1;
  uint8_t packetLength = 61;
  bool whitening = false;
  bool manchester = false;
};

bool initCc1101Radio();
bool isCc1101Ready();
float getCc1101FrequencyMhz();
void setCc1101FrequencyMhz(float mhz);

const Cc1101PacketConfig &getCc1101PacketConfig();
bool configureCc1101Packet(const Cc1101PacketConfig &config, String &errorOut);
int readCc1101RssiDbm(String *errorOut = nullptr);

bool sendCc1101Packet(const uint8_t *data,
                      size_t size,
                      int txDelayMs,
                      String &errorOut);
bool sendCc1101PacketText(const String &text,
                          int txDelayMs,
                          String &errorOut);
bool receiveCc1101Packet(std::vector<uint8_t> &outData,
                         int timeoutMs,
                         int *rssiOut,
                         String &errorOut);

bool transmitCc1101(uint32_t code,
                    int bits,
                    int pulseLength,
                    int protocol,
                    int repeat,
                    String &errorOut);

void appendCc1101Info(JsonObject obj);

// ---- Low-level / raw control (used by the JS framework) -------------------

// Config registers 0x00..0x2E and status registers 0x30..0x3D.
bool readCc1101Register(uint8_t addr, uint8_t &value, String &errorOut);
// Config registers 0x00..0x2E only.
bool writeCc1101Register(uint8_t addr, uint8_t value, String &errorOut);
// Command strobes 0x30..0x3D (SRES, SRX, STX, SIDLE, ...).
bool strobeCc1101(uint8_t strobe, String &errorOut);
// mode: "idle" | "rx" | "tx" | "sleep" | "reset"
bool setCc1101Mode(const String &mode, String &errorOut);

// dBm: -30, -20, -15, -10, -6, 0, 5, 7, 10, 11 or 12.
bool setCc1101TxPowerDbm(int dbm, String &errorOut);
int getCc1101TxPowerDbm();

// OOK/ASK raw pulse capture on GDO0. Durations are microseconds, alternating
// carrier-on / carrier-off, starting with carrier-on. A burst is accepted once
// it holds at least minEdges edges and the line then stays idle for gapUs.
bool captureCc1101Raw(uint32_t timeoutMs,
                      size_t minEdges,
                      uint32_t gapUs,
                      std::vector<uint16_t> &out,
                      String &errorOut,
                      const std::function<bool()> &keepWaiting =
                          std::function<bool()>());

// Replays OOK/ASK pulses (same layout as captureCc1101Raw) on GDO0.
bool replayCc1101Raw(const uint16_t *timings,
                     size_t count,
                     int repeat,
                     uint32_t interFrameGapUs,
                     String &errorOut);
