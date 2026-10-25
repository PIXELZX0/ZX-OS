#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Pure (hardware independent) IR protocol encoders/decoders.
// Timings are microseconds, alternating mark/space and always starting with a
// mark (carrier on).
namespace irproto {

enum class Protocol : uint8_t {
  Nec = 0,     // 8-bit address / 8-bit command (both sent with inverse)
  NecExt,      // 16-bit address / 16-bit command
  Samsung32,   // 8-bit address / 8-bit command
  Sony12,      // 7-bit command / 5-bit address
  Sony15,      // 7-bit command / 8-bit address
  Sony20,      // 7-bit command / 13-bit address
  Rc5,         // 5-bit address / 7-bit command
};

constexpr uint8_t kFlagRepeat = 1;  // NEC family: emit the short repeat code
constexpr uint8_t kFlagToggle = 2;  // RC5: toggle bit

struct Frame {
  Protocol protocol = Protocol::Nec;
  uint32_t address = 0;
  uint32_t command = 0;
  bool toggle = false;
};

const char *name(Protocol p);
bool parseName(const char *text, Protocol &out);

uint32_t carrierHz(Protocol p);
// Start-to-start period of consecutive frames, used when repeating a code.
uint32_t framePeriodUs(Protocol p);

// Returns false and sets *err (static string) when address/command do not fit.
bool encode(Protocol p,
            uint32_t address,
            uint32_t command,
            uint8_t flags,
            std::vector<uint16_t> &out,
            const char **err = nullptr);

// Tries every protocol. Returns false when the capture is not recognised.
bool decode(const uint16_t *timings, size_t count, Frame &out);

}  // namespace irproto
