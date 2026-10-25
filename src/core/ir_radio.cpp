#include "ir_radio.h"

#include <esp_timer.h>

#include "user_config.h"

namespace irradio {

namespace {

constexpr size_t kMaxEdges = 800;
constexpr uint32_t kFrameGapUs = 20000;  // silence that terminates a frame
constexpr uint8_t kLedcBits = 8;
constexpr uint32_t kLedcDutyOn = 85;      // ~33% duty
constexpr size_t kMaxTxTimings = 2000;
constexpr uint32_t kMinCarrierHz = 20000;
constexpr uint32_t kMaxCarrierHz = 60000;

int gTxPin = USER_IR_TX_PIN;
int gRxPin = USER_IR_RX_PIN;
bool gTxAttached = false;
bool gRxAttached = false;

volatile uint32_t gEdgeUs[kMaxEdges];
volatile size_t gEdgeCount = 0;

void IRAM_ATTR onIrEdge() {
  const size_t n = gEdgeCount;
  if (n < kMaxEdges) {
    gEdgeUs[n] = static_cast<uint32_t>(esp_timer_get_time());
    gEdgeCount = n + 1;
  }
}

void waitUntilUs(int64_t deadlineUs) {
  while (esp_timer_get_time() < deadlineUs) {
  }
}

void detachTx() {
  if (gTxAttached) {
    ledcDetach(gTxPin);
    pinMode(gTxPin, OUTPUT);
    digitalWrite(gTxPin, LOW);
    gTxAttached = false;
  }
}

void detachRx() {
  if (gRxAttached) {
    detachInterrupt(digitalPinToInterrupt(gRxPin));
    gRxAttached = false;
  }
}

}  // namespace

bool available() { return gTxPin >= 0 || gRxPin >= 0; }
int txPin() { return gTxPin; }
int rxPin() { return gRxPin; }

bool setPins(int tx, int rx, String &errorOut) {
  if (tx < -1 || rx < -1 || tx > 48 || rx > 48) {
    errorOut = "invalid GPIO";
    return false;
  }
  end();
  if (tx >= 0) {
    gTxPin = tx;
  }
  if (rx >= 0) {
    gRxPin = rx;
  }
  return true;
}

bool sendRaw(const uint16_t *timings,
             size_t count,
             uint32_t carrierHz,
             String &errorOut) {
  if (gTxPin < 0) {
    errorOut = "IR transmitter not available on this board";
    return false;
  }
  if (!timings || count == 0 || count > kMaxTxTimings) {
    errorOut = "timings must hold 1.." + String(kMaxTxTimings) + " entries";
    return false;
  }
  if (carrierHz < kMinCarrierHz || carrierHz > kMaxCarrierHz) {
    errorOut = "carrier must be 20000..60000 Hz";
    return false;
  }

  detachRx();  // our own LED light can leak into the receiver
  detachTx();
  if (!ledcAttach(gTxPin, carrierHz, kLedcBits)) {
    errorOut = "LEDC attach failed";
    return false;
  }
  gTxAttached = true;
  ledcWrite(gTxPin, 0);

  int64_t next = esp_timer_get_time();
  for (size_t i = 0; i < count; ++i) {
    ledcWrite(gTxPin, (i % 2 == 0) ? kLedcDutyOn : 0);
    next += timings[i];
    waitUntilUs(next);
  }
  ledcWrite(gTxPin, 0);
  detachTx();
  return true;
}

bool send(irproto::Protocol protocol,
          uint32_t address,
          uint32_t command,
          int repeat,
          uint8_t flags,
          String &errorOut) {
  if (repeat < 1 || repeat > 50) {
    errorOut = "repeat must be 1..50";
    return false;
  }
  std::vector<uint16_t> first;
  const char *err = nullptr;
  if (!irproto::encode(protocol, address, command, flags, first, &err)) {
    errorOut = err ? err : "encode failed";
    return false;
  }
  std::vector<uint16_t> again;
  const bool necFamily = protocol == irproto::Protocol::Nec ||
                         protocol == irproto::Protocol::NecExt;
  if (necFamily) {
    irproto::encode(protocol, address, command,
                    flags | irproto::kFlagRepeat, again, nullptr);
  } else {
    again = first;
  }

  const uint32_t carrier = irproto::carrierHz(protocol);
  const uint32_t period = irproto::framePeriodUs(protocol);
  for (int i = 0; i < repeat; ++i) {
    const std::vector<uint16_t> &frame = (i == 0) ? first : again;
    const int64_t start = esp_timer_get_time();
    if (!sendRaw(frame.data(), frame.size(), carrier, errorOut)) {
      return false;
    }
    if (i + 1 < repeat) {
      const int64_t wait = static_cast<int64_t>(period) -
                           (esp_timer_get_time() - start);
      if (wait > 0) {
        delay(static_cast<uint32_t>(wait / 1000));
      }
    }
  }
  return true;
}

bool receiveRaw(uint32_t timeoutMs,
                std::vector<uint16_t> &out,
                String &errorOut,
                const KeepWaiting &keepWaiting) {
  out.clear();
  if (gRxPin < 0) {
    errorOut = "IR receiver not available on this board";
    return false;
  }
  detachRx();
  pinMode(gRxPin, INPUT);
  gEdgeCount = 0;
  attachInterrupt(digitalPinToInterrupt(gRxPin), onIrEdge, CHANGE);
  gRxAttached = true;

  const uint32_t startMs = millis();
  bool gotFrame = false;
  while (millis() - startMs < timeoutMs) {
    if (keepWaiting && !keepWaiting()) {
      break;
    }
    const size_t n = gEdgeCount;
    if (n >= 3) {
      const uint32_t last = gEdgeUs[n - 1];
      if (static_cast<uint32_t>(esp_timer_get_time()) - last > kFrameGapUs ||
          n >= kMaxEdges) {
        gotFrame = true;
        break;
      }
    }
    delay(1);
  }
  detachRx();

  if (!gotFrame) {
    return false;
  }

  // Receivers are active-low: the first edge of a frame is falling = mark.
  const size_t n = gEdgeCount;
  // The line idles high, so an even edge count must end high. If it does not,
  // the capture started on a stray rising edge: skip that edge.
  const bool endsHigh = digitalRead(gRxPin) == HIGH;
  const size_t first = (endsHigh == (n % 2 == 0)) ? 0 : 1;
  if (n - first < 3) {
    return false;
  }
  // m edges give m-1 durations. Frames must end with a mark, so an even
  // duration count means a trailing space that we drop.
  size_t count = n - first - 1;
  if (count % 2 == 0) {
    --count;
  }
  for (size_t i = 0; i < count; ++i) {
    uint32_t d = gEdgeUs[first + i + 1] - gEdgeUs[first + i];
    if (d > 0xFFFF) {
      d = 0xFFFF;
    }
    out.push_back(static_cast<uint16_t>(d));
  }
  return !out.empty();
}

void end() {
  detachRx();
  detachTx();
}

}  // namespace irradio
