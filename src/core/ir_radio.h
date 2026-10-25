#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "ir_protocols.h"

// IR transmit (LEDC carrier + busy-wait timing) and receive (GPIO edge ISR).
// Timings are microseconds, alternating mark/space, starting with a mark.
namespace irradio {

// Returns true while the caller still wants to wait (false aborts a receive).
using KeepWaiting = std::function<bool()>;

bool available();
int txPin();
int rxPin();

// Override the compile-time pins (-1 keeps the current value).
bool setPins(int tx, int rx, String &errorOut);

bool sendRaw(const uint16_t *timings,
             size_t count,
             uint32_t carrierHz,
             String &errorOut);

bool send(irproto::Protocol protocol,
          uint32_t address,
          uint32_t command,
          int repeat,
          uint8_t flags,
          String &errorOut);

// Waits for one IR frame. Returns false with empty errorOut on plain timeout.
bool receiveRaw(uint32_t timeoutMs,
                std::vector<uint16_t> &out,
                String &errorOut,
                const KeepWaiting &keepWaiting = KeepWaiting());

void end();

}  // namespace irradio
