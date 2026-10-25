#include "ir_app.h"

#include <Arduino.h>

#include <vector>

#include "../ui/ui_runtime.h"
#include "user_config.h"

// IRremoteESP8266 is an optional dependency. Guard the whole implementation so
// the firmware still builds (and the app degrades gracefully) when the library
// is unavailable for a given board/toolchain, mirroring the NFC/RFID apps.
#if __has_include(<IRremoteESP8266.h>)
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#define IR_LIB_AVAILABLE 1
#else
#define IR_LIB_AVAILABLE 0
#endif

namespace {

#if IR_LIB_AVAILABLE

constexpr int8_t kIrTxPin = static_cast<int8_t>(USER_IR_TX_PIN);
constexpr int8_t kIrRxPin = static_cast<int8_t>(USER_IR_RX_PIN);

// Receiver capture buffer. 1024 half-cycles handles long AC/raw frames while
// staying well within the ESP32's RAM budget.
constexpr uint16_t kCaptureBufferSize = 1024;
constexpr uint8_t kCaptureTimeoutMs = 50;      // end-of-frame gap
constexpr unsigned long kLearnWindowMs = 15000;  // give up after 15 s

bool gTxReady = false;

// A single captured/known signal held in RAM (the "last" slot, like Flipper's
// clipboard). Known protocols replay from (type,value,bits); everything else
// (including AC state frames) replays from the raw timing buffer.
struct CapturedSignal {
  bool valid = false;
  bool isRaw = false;
  decode_type_t type = decode_type_t::UNKNOWN;
  uint64_t value = 0;
  uint16_t bits = 0;
  std::vector<uint16_t> raw;  // microsecond timings, used when isRaw
  String label;
};

CapturedSignal gLast;

// Universal-remote presets. These are widely published NEC/SIRC codes for the
// most common consumer brands; sending the batch in sequence mimics the Flipper
// "universal remote" / TV-B-Gone behaviour.
struct IrPreset {
  const char *label;
  decode_type_t type;
  uint64_t value;
  uint16_t bits;
};

const IrPreset kTvPowerPresets[] = {
    {"Samsung", decode_type_t::SAMSUNG, 0xE0E040BFULL, 32},
    {"LG", decode_type_t::NEC, 0x20DF10EFULL, 32},
    {"Sony", decode_type_t::SONY, 0xA90ULL, 12},
    {"Panasonic", decode_type_t::PANASONIC, 0x40040100BCBDULL, 48},
    {"NEC generic", decode_type_t::NEC, 0x00FF00FFULL, 32},
};

const IrPreset kTvVolUpPresets[] = {
    {"Samsung", decode_type_t::SAMSUNG, 0xE0E0E01FULL, 32},
    {"LG", decode_type_t::NEC, 0x20DF40BFULL, 32},
    {"Sony", decode_type_t::SONY, 0x490ULL, 12},
};

const IrPreset kTvVolDownPresets[] = {
    {"Samsung", decode_type_t::SAMSUNG, 0xE0E0D02FULL, 32},
    {"LG", decode_type_t::NEC, 0x20DFC03FULL, 32},
    {"Sony", decode_type_t::SONY, 0xC90ULL, 12},
};

const IrPreset kTvMutePresets[] = {
    {"Samsung", decode_type_t::SAMSUNG, 0xE0E0F00FULL, 32},
    {"LG", decode_type_t::NEC, 0x20DF906FULL, 32},
    {"Sony", decode_type_t::SONY, 0x290ULL, 12},
};

IRsend *gSender = nullptr;

bool irTxConfigured() {
  return kIrTxPin >= 0;
}

bool irRxConfigured() {
  return kIrRxPin >= 0;
}

bool ensureTxReady() {
  if (!irTxConfigured()) {
    return false;
  }
  if (gTxReady) {
    return true;
  }
  if (!gSender) {
    gSender = new IRsend(static_cast<uint16_t>(kIrTxPin));
  }
  gSender->begin();
  gTxReady = true;
  return true;
}

String protocolName(decode_type_t type) {
  return typeToString(type, false);
}

// Send one preset; returns true if the protocol is supported by the library.
bool sendPreset(const IrPreset &preset) {
  if (!ensureTxReady()) {
    return false;
  }
  return gSender->send(preset.type, preset.value, preset.bits);
}

void sendUniversalBatch(AppContext &ctx,
                        const char *title,
                        const IrPreset *presets,
                        size_t count,
                        const std::function<void()> &backgroundTick) {
  if (!ensureTxReady()) {
    ctx.uiRuntime->showToast("IR", "IR TX pin not wired", 1700, backgroundTick);
    return;
  }

  size_t sent = 0;
  for (size_t i = 0; i < count; ++i) {
    const int percent = static_cast<int>(((i + 1) * 100ULL) / count);
    ctx.uiRuntime->showProgressOverlay(
        String(title),
        String("Sending ") + presets[i].label + " (" +
            protocolName(presets[i].type) + ")",
        percent);
    if (sendPreset(presets[i])) {
      ++sent;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    delay(120);
  }
  ctx.uiRuntime->hideProgressOverlay();
  ctx.uiRuntime->showToast(
      "IR", "Sent " + String(static_cast<unsigned>(sent)) + "/" +
                String(static_cast<unsigned>(count)) + " codes",
      1400, backgroundTick);
}

void showModuleInfo(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("Driver: IRremoteESP8266");
  lines.push_back(String("TX pin: ") +
                  (irTxConfigured() ? String(static_cast<int>(kIrTxPin)) : String("(not wired)")));
  lines.push_back(String("RX pin: ") +
                  (irRxConfigured() ? String(static_cast<int>(kIrRxPin)) : String("(not wired)")));
  lines.push_back("Carrier: 38 kHz");
  if (gLast.valid) {
    lines.push_back("Last: " + gLast.label);
  } else {
    lines.push_back("Last: (none)");
  }
  ctx.uiRuntime->showInfo("IR Module", lines, backgroundTick, "OK/BACK Exit");
}

// Blocking capture loop: enables the receiver, shows a progress overlay, and
// polls for a decoded frame or a BACK press to cancel.
void learnSignal(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  if (!irRxConfigured()) {
    ctx.uiRuntime->showToast("IR Learn", "IR RX pin not wired", 1800, backgroundTick);
    return;
  }

  IRrecv receiver(static_cast<uint16_t>(kIrRxPin),
                  kCaptureBufferSize,
                  kCaptureTimeoutMs,
                  true);
  receiver.enableIRIn();

  ctx.uiRuntime->resetInputState();
  ctx.uiRuntime->showProgressOverlay("IR Learn",
                                     "Point remote at receiver, press a button",
                                     -1);

  decode_results results;
  const unsigned long started = millis();
  bool captured = false;

  while (millis() - started < kLearnWindowMs) {
    if (receiver.decode(&results)) {
      captured = true;
      break;
    }

    const UiEvent event = ctx.uiRuntime->pollInput();
    if (event.back) {
      break;
    }

    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }

  receiver.disableIRIn();
  ctx.uiRuntime->hideProgressOverlay();

  if (!captured) {
    ctx.uiRuntime->showToast("IR Learn", "No signal captured", 1500, backgroundTick);
    return;
  }

  gLast = CapturedSignal();
  gLast.valid = true;
  gLast.type = results.decode_type;

  const bool knownSendable =
      results.decode_type != decode_type_t::UNKNOWN &&
      !hasACState(results.decode_type) &&
      results.bits > 0;

  if (knownSendable) {
    gLast.isRaw = false;
    gLast.value = results.value;
    gLast.bits = results.bits;
    gLast.label = protocolName(results.decode_type) + " 0x" +
                  String(static_cast<uint32_t>(results.value), HEX);
  } else {
    // Store the corrected raw timing array for faithful replay.
    gLast.isRaw = true;
    const uint16_t rawLen = getCorrectedRawLength(&results);
    uint16_t *rawArray = resultToRawArray(&results);
    if (rawArray && rawLen > 0) {
      gLast.raw.assign(rawArray, rawArray + rawLen);
    }
    delete[] rawArray;
    gLast.label = String("RAW ") + String(rawLen) + " marks";
    if (gLast.raw.empty()) {
      gLast.valid = false;
      ctx.uiRuntime->showToast("IR Learn", "Capture too large", 1600, backgroundTick);
      return;
    }
  }

  std::vector<String> lines;
  lines.push_back("Captured OK");
  lines.push_back("Protocol: " + protocolName(results.decode_type));
  if (!gLast.isRaw) {
    lines.push_back("Value: 0x" +
                    String(static_cast<uint32_t>(results.value), HEX));
    lines.push_back("Bits: " + String(results.bits));
  } else {
    lines.push_back("Raw marks: " + String(static_cast<unsigned>(gLast.raw.size())));
  }
  lines.push_back("Use 'Replay Last' to send");
  ctx.uiRuntime->showInfo("IR Learn", lines, backgroundTick, "OK/BACK Exit");
}

void replayLast(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  if (!gLast.valid) {
    ctx.uiRuntime->showToast("IR Replay", "Nothing captured yet", 1500, backgroundTick);
    return;
  }
  if (!ensureTxReady()) {
    ctx.uiRuntime->showToast("IR Replay", "IR TX pin not wired", 1700, backgroundTick);
    return;
  }

  bool ok = false;
  if (gLast.isRaw) {
    if (!gLast.raw.empty()) {
      gSender->sendRaw(gLast.raw.data(),
                       static_cast<uint16_t>(gLast.raw.size()),
                       38);
      ok = true;
    }
  } else {
    ok = gSender->send(gLast.type, gLast.value, gLast.bits);
  }

  ctx.uiRuntime->showToast("IR Replay",
                           ok ? ("Sent " + gLast.label) : String("Protocol unsupported"),
                           1400, backgroundTick);
}

void universalRemoteMenu(AppContext &ctx,
                         const std::function<void()> &backgroundTick) {
  int selected = 0;
  while (true) {
    std::vector<String> menu;
    menu.push_back("TV Power");
    menu.push_back("TV Volume +");
    menu.push_back("TV Volume -");
    menu.push_back("TV Mute");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("Universal Remote",
                                               menu,
                                               selected,
                                               backgroundTick,
                                               "OK Send  BACK Exit",
                                               "Multi-brand IR");
    if (choice < 0 || choice == 4) {
      return;
    }
    selected = choice;

    switch (choice) {
      case 0:
        sendUniversalBatch(ctx, "TV Power", kTvPowerPresets,
                           sizeof(kTvPowerPresets) / sizeof(kTvPowerPresets[0]),
                           backgroundTick);
        break;
      case 1:
        sendUniversalBatch(ctx, "TV Volume +", kTvVolUpPresets,
                           sizeof(kTvVolUpPresets) / sizeof(kTvVolUpPresets[0]),
                           backgroundTick);
        break;
      case 2:
        sendUniversalBatch(ctx, "TV Volume -", kTvVolDownPresets,
                           sizeof(kTvVolDownPresets) / sizeof(kTvVolDownPresets[0]),
                           backgroundTick);
        break;
      case 3:
        sendUniversalBatch(ctx, "TV Mute", kTvMutePresets,
                           sizeof(kTvMutePresets) / sizeof(kTvMutePresets[0]),
                           backgroundTick);
        break;
      default:
        break;
    }
  }
}

#endif  // IR_LIB_AVAILABLE

}  // namespace

void runIrApp(AppContext &ctx,
              const std::function<void()> &backgroundTick) {
#if IR_LIB_AVAILABLE
  int selected = 0;
  while (true) {
    std::vector<String> menu;
    menu.push_back("Module Info");
    menu.push_back("Universal Remote");
    menu.push_back("Learn Signal");
    menu.push_back("Replay Last");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("IR",
                                               menu,
                                               selected,
                                               backgroundTick,
                                               "OK Select  BACK Exit",
                                               "Flipper-style IR");
    if (choice < 0 || choice == 4) {
      return;
    }
    selected = choice;

    switch (choice) {
      case 0:
        showModuleInfo(ctx, backgroundTick);
        break;
      case 1:
        universalRemoteMenu(ctx, backgroundTick);
        break;
      case 2:
        learnSignal(ctx, backgroundTick);
        break;
      case 3:
        replayLast(ctx, backgroundTick);
        break;
      default:
        break;
    }
  }
#else
  (void)backgroundTick;
  ctx.uiRuntime->showToast("IR", "IRremoteESP8266 library missing", 1800, backgroundTick);
#endif
}
