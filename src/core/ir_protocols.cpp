#include "ir_protocols.h"

#include <string.h>
#include <strings.h>

namespace irproto {

namespace {

constexpr uint16_t kNecHeaderMark = 9000;
constexpr uint16_t kNecHeaderSpace = 4500;
constexpr uint16_t kNecRepeatSpace = 2250;
constexpr uint16_t kNecBitMark = 560;
constexpr uint16_t kNecZeroSpace = 560;
constexpr uint16_t kNecOneSpace = 1690;

constexpr uint16_t kSamsungHeader = 4500;

constexpr uint16_t kSonyHeaderMark = 2400;
constexpr uint16_t kSonyUnit = 600;

constexpr uint16_t kRc5Half = 889;

bool near(uint32_t value, uint32_t target) {
  const uint32_t tol = target / 4 + 100;
  return value + tol >= target && value <= target + tol;
}

void setErr(const char **err, const char *msg) {
  if (err) {
    *err = msg;
  }
}

void pushNecBits(std::vector<uint16_t> &out, uint32_t value) {
  for (int i = 0; i < 32; ++i) {
    out.push_back(kNecBitMark);
    out.push_back(((value >> i) & 1U) ? kNecOneSpace : kNecZeroSpace);
  }
  out.push_back(kNecBitMark);
}

bool readNecBits(const uint16_t *t, size_t n, size_t first, uint32_t &value) {
  // 32 mark/space pairs + stop mark.
  if (n < first + 65) {
    return false;
  }
  value = 0;
  for (int i = 0; i < 32; ++i) {
    const uint16_t mark = t[first + 2 * i];
    const uint16_t space = t[first + 2 * i + 1];
    if (!near(mark, kNecBitMark)) {
      return false;
    }
    if (space > (kNecZeroSpace + kNecOneSpace) / 2) {
      if (!near(space, kNecOneSpace)) {
        return false;
      }
      value |= (1UL << i);
    } else if (!near(space, kNecZeroSpace)) {
      return false;
    }
  }
  return near(t[first + 64], kNecBitMark);
}

bool decodeNec(const uint16_t *t, size_t n, Frame &out) {
  if (n < 67 || !near(t[0], kNecHeaderMark) || !near(t[1], kNecHeaderSpace)) {
    return false;
  }
  uint32_t v = 0;
  if (!readNecBits(t, n, 2, v)) {
    return false;
  }
  const uint8_t b0 = v & 0xFF;
  const uint8_t b1 = (v >> 8) & 0xFF;
  const uint8_t b2 = (v >> 16) & 0xFF;
  const uint8_t b3 = (v >> 24) & 0xFF;
  if (static_cast<uint8_t>(~b0) == b1 && static_cast<uint8_t>(~b2) == b3) {
    out.protocol = Protocol::Nec;
    out.address = b0;
    out.command = b2;
  } else {
    out.protocol = Protocol::NecExt;
    out.address = v & 0xFFFF;
    out.command = v >> 16;
  }
  return true;
}

bool decodeSamsung(const uint16_t *t, size_t n, Frame &out) {
  if (n < 67 || !near(t[0], kSamsungHeader) || !near(t[1], kSamsungHeader)) {
    return false;
  }
  uint32_t v = 0;
  if (!readNecBits(t, n, 2, v)) {
    return false;
  }
  const uint8_t b0 = v & 0xFF;
  const uint8_t b1 = (v >> 8) & 0xFF;
  const uint8_t b2 = (v >> 16) & 0xFF;
  const uint8_t b3 = (v >> 24) & 0xFF;
  if (b0 != b1 || static_cast<uint8_t>(~b2) != b3) {
    return false;
  }
  out.protocol = Protocol::Samsung32;
  out.address = b0;
  out.command = b2;
  return true;
}

bool decodeSony(const uint16_t *t, size_t n, Frame &out) {
  if (n < 2 || !near(t[0], kSonyHeaderMark) || !near(t[1], kSonyUnit)) {
    return false;
  }
  uint32_t value = 0;
  int bits = 0;
  for (size_t i = 2; i < n && bits < 20; i += 2) {
    const uint16_t mark = t[i];
    if (mark > (kSonyUnit + 2 * kSonyUnit) / 2) {
      if (!near(mark, 2 * kSonyUnit)) {
        return false;
      }
      value |= (1UL << bits);
    } else if (!near(mark, kSonyUnit)) {
      return false;
    }
    ++bits;
    // The space after a mark is 600us; the last one may be missing/long.
    if (i + 1 < n && bits < 20 && !near(t[i + 1], kSonyUnit)) {
      break;
    }
  }
  if (bits != 12 && bits != 15 && bits != 20) {
    return false;
  }
  out.protocol = bits == 12 ? Protocol::Sony12
                            : (bits == 15 ? Protocol::Sony15 : Protocol::Sony20);
  out.command = value & 0x7F;
  out.address = value >> 7;
  return true;
}

bool decodeRc5(const uint16_t *t, size_t n, Frame &out) {
  if (n < 10 || n > 40) {
    return false;
  }
  // The very first half bit of RC5 is an idle (space) that the receiver
  // never reports, so start with a virtual space slot.
  bool slots[40];
  size_t count = 0;
  slots[count++] = false;
  for (size_t i = 0; i < n; ++i) {
    const bool mark = (i % 2) == 0;
    uint32_t units = (static_cast<uint32_t>(t[i]) + kRc5Half / 2) / kRc5Half;
    if (units < 1 || units > 2 || !near(t[i], units * kRc5Half)) {
      // Allow a long trailing gap after the final space.
      if (i == n - 1 && !mark && t[i] > 2 * kRc5Half) {
        units = 2;
      } else {
        return false;
      }
    }
    for (uint32_t u = 0; u < units && count < 40; ++u) {
      slots[count++] = mark;
    }
  }
  while (count < 28) {
    slots[count++] = false;  // trailing idle
  }
  uint16_t bits = 0;
  for (int i = 0; i < 14; ++i) {
    const bool a = slots[2 * i];
    const bool b = slots[2 * i + 1];
    if (a == b) {
      return false;
    }
    bits = static_cast<uint16_t>((bits << 1) | (b ? 1 : 0));  // space->mark = 1
  }
  if (!((bits >> 13) & 1U)) {
    return false;  // start bit
  }
  const bool field = (bits >> 12) & 1U;
  out.protocol = Protocol::Rc5;
  out.toggle = (bits >> 11) & 1U;
  out.address = (bits >> 6) & 0x1F;
  out.command = (bits & 0x3F) | (field ? 0 : 0x40);
  return true;
}

void encodeSony(uint32_t address,
                uint32_t command,
                int bits,
                std::vector<uint16_t> &out) {
  const uint32_t value = (command & 0x7F) | (address << 7);
  out.push_back(kSonyHeaderMark);
  out.push_back(kSonyUnit);
  for (int i = 0; i < bits; ++i) {
    out.push_back(((value >> i) & 1U) ? 2 * kSonyUnit : kSonyUnit);
    if (i != bits - 1) {
      out.push_back(kSonyUnit);
    }
  }
}

void encodeRc5(uint32_t address,
               uint32_t command,
               bool toggle,
               std::vector<uint16_t> &out) {
  const bool field = (command & 0x40) == 0;  // field bit is inverted cmd bit 6
  uint16_t bits = 0;
  bits |= 1U << 13;
  bits |= (field ? 1U : 0U) << 12;
  bits |= (toggle ? 1U : 0U) << 11;
  bits |= (address & 0x1F) << 6;
  bits |= command & 0x3F;

  // Manchester: 1 = space,mark ; 0 = mark,space. Merge equal neighbours.
  bool levels[28];
  for (int i = 0; i < 14; ++i) {
    const bool one = (bits >> (13 - i)) & 1U;
    levels[2 * i] = !one;
    levels[2 * i + 1] = one;
  }
  // Drop the leading idle half bit (first bit is always 1 => space first).
  size_t i = 1;
  bool level = levels[i];
  uint32_t len = kRc5Half;
  for (++i; i < 28; ++i) {
    if (levels[i] == level) {
      len += kRc5Half;
    } else {
      out.push_back(static_cast<uint16_t>(len));
      level = levels[i];
      len = kRc5Half;
    }
  }
  out.push_back(static_cast<uint16_t>(len));
  if (!level) {
    out.pop_back();  // do not end with a trailing space
  }
}

}  // namespace

const char *name(Protocol p) {
  switch (p) {
    case Protocol::Nec:
      return "NEC";
    case Protocol::NecExt:
      return "NECext";
    case Protocol::Samsung32:
      return "Samsung32";
    case Protocol::Sony12:
      return "SIRC";
    case Protocol::Sony15:
      return "SIRC15";
    case Protocol::Sony20:
      return "SIRC20";
    case Protocol::Rc5:
      return "RC5";
  }
  return "?";
}

bool parseName(const char *text, Protocol &out) {
  if (!text) {
    return false;
  }
  static const Protocol all[] = {Protocol::Nec,    Protocol::NecExt,
                                 Protocol::Samsung32, Protocol::Sony12,
                                 Protocol::Sony15, Protocol::Sony20,
                                 Protocol::Rc5};
  for (Protocol p : all) {
    if (strcasecmp(text, name(p)) == 0) {
      out = p;
      return true;
    }
  }
  if (strcasecmp(text, "Sony") == 0 || strcasecmp(text, "SIRC12") == 0) {
    out = Protocol::Sony12;
    return true;
  }
  return false;
}

uint32_t carrierHz(Protocol p) {
  switch (p) {
    case Protocol::Sony12:
    case Protocol::Sony15:
    case Protocol::Sony20:
      return 40000;
    case Protocol::Rc5:
      return 36000;
    default:
      return 38000;
  }
}

uint32_t framePeriodUs(Protocol p) {
  switch (p) {
    case Protocol::Sony12:
    case Protocol::Sony15:
    case Protocol::Sony20:
      return 45000;
    case Protocol::Rc5:
      return 114000;
    default:
      return 108000;
  }
}

bool encode(Protocol p,
            uint32_t address,
            uint32_t command,
            uint8_t flags,
            std::vector<uint16_t> &out,
            const char **err) {
  out.clear();
  switch (p) {
    case Protocol::Nec: {
      if (flags & kFlagRepeat) {
        out = {kNecHeaderMark, kNecRepeatSpace, kNecBitMark};
        return true;
      }
      if (address > 0xFF || command > 0xFF) {
        setErr(err, "NEC address/command must be 0..255");
        return false;
      }
      const uint32_t v = address | ((~address & 0xFFU) << 8) |
                         (command << 16) | ((~command & 0xFFU) << 24);
      out.push_back(kNecHeaderMark);
      out.push_back(kNecHeaderSpace);
      pushNecBits(out, v);
      return true;
    }
    case Protocol::NecExt: {
      if (flags & kFlagRepeat) {
        out = {kNecHeaderMark, kNecRepeatSpace, kNecBitMark};
        return true;
      }
      if (address > 0xFFFF || command > 0xFFFF) {
        setErr(err, "NECext address/command must be 0..65535");
        return false;
      }
      out.push_back(kNecHeaderMark);
      out.push_back(kNecHeaderSpace);
      pushNecBits(out, address | (command << 16));
      return true;
    }
    case Protocol::Samsung32: {
      if (address > 0xFF || command > 0xFF) {
        setErr(err, "Samsung32 address/command must be 0..255");
        return false;
      }
      const uint32_t v = address | (address << 8) | (command << 16) |
                         ((~command & 0xFFU) << 24);
      out.push_back(kSamsungHeader);
      out.push_back(kSamsungHeader);
      pushNecBits(out, v);
      return true;
    }
    case Protocol::Sony12:
    case Protocol::Sony15:
    case Protocol::Sony20: {
      const int bits = p == Protocol::Sony12 ? 12 : (p == Protocol::Sony15 ? 15 : 20);
      const uint32_t maxAddr = (1UL << (bits - 7)) - 1;
      if (command > 0x7F || address > maxAddr) {
        setErr(err, "SIRC command must be 0..127 and address within protocol width");
        return false;
      }
      encodeSony(address, command, bits, out);
      return true;
    }
    case Protocol::Rc5: {
      if (address > 0x1F || command > 0x7F) {
        setErr(err, "RC5 address must be 0..31 and command 0..127");
        return false;
      }
      encodeRc5(address, command, (flags & kFlagToggle) != 0, out);
      return true;
    }
  }
  setErr(err, "unknown protocol");
  return false;
}

bool decode(const uint16_t *timings, size_t count, Frame &out) {
  if (!timings || count < 3) {
    return false;
  }
  return decodeNec(timings, count, out) || decodeSamsung(timings, count, out) ||
         decodeSony(timings, count, out) || decodeRc5(timings, count, out);
}

}  // namespace irproto
