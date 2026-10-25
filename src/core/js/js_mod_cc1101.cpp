#include <Arduino.h>

#include "../cc1101_radio.h"
#include "js_internal.h"

// cc1101.* — sub-GHz radio: register-level control, raw OOK capture/replay,
// the rc-switch style fixed-code transmitter, and the packet-mode (FSK/GFSK)
// send/receive already used by the gateway.
namespace zxjs {

namespace {

duk_ret_t cc1101Info(duk_context *ctx) {
  duk_push_object(ctx);
  duk_push_boolean(ctx, isCc1101Ready());
  duk_put_prop_string(ctx, -2, "ready");
  duk_push_number(ctx, getCc1101FrequencyMhz());
  duk_put_prop_string(ctx, -2, "frequencyMhz");
  duk_push_int(ctx, getCc1101TxPowerDbm());
  duk_put_prop_string(ctx, -2, "txPowerDbm");
  return 1;
}

duk_ret_t cc1101SetFreq(duk_context *ctx) {
  const double mhz = duk_require_number(ctx, 0);
  setCc1101FrequencyMhz(static_cast<float>(mhz));
  duk_push_number(ctx, getCc1101FrequencyMhz());
  return 1;
}

duk_ret_t cc1101SetPower(duk_context *ctx) {
  const int dbm = duk_require_int(ctx, 0);
  String err;
  if (!setCc1101TxPowerDbm(dbm, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101ReadRssi(duk_context *ctx) {
  String err;
  const int rssi = readCc1101RssiDbm(&err);
  if (err.length()) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  duk_push_int(ctx, rssi);
  return 1;
}

duk_ret_t cc1101ReadReg(duk_context *ctx) {
  const int addr = duk_require_int(ctx, 0);
  if (addr < 0 || addr > 0xFF) {
    return duk_error(ctx, DUK_ERR_RANGE_ERROR, "register out of range");
  }
  uint8_t value = 0;
  String err;
  if (!readCc1101Register(static_cast<uint8_t>(addr), value, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  duk_push_uint(ctx, value);
  return 1;
}

duk_ret_t cc1101WriteReg(duk_context *ctx) {
  const int addr = duk_require_int(ctx, 0);
  const int value = duk_require_int(ctx, 1);
  if (addr < 0 || addr > 0xFF || value < 0 || value > 0xFF) {
    return duk_error(ctx, DUK_ERR_RANGE_ERROR, "register/value out of range");
  }
  String err;
  if (!writeCc1101Register(static_cast<uint8_t>(addr),
                           static_cast<uint8_t>(value), err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101Strobe(duk_context *ctx) {
  const int strobe = duk_require_int(ctx, 0);
  String err;
  if (!strobeCc1101(static_cast<uint8_t>(strobe), err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101SetMode(duk_context *ctx) {
  const char *mode = duk_require_string(ctx, 0);
  String err;
  if (!setCc1101Mode(mode, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101Capture(duk_context *ctx) {
  const int timeoutMs = duk_opt_int(ctx, 0, 8000);
  const int minEdges = duk_opt_int(ctx, 1, 8);
  const int gapUs = duk_opt_int(ctx, 2, 4000);

  std::vector<uint16_t> timings;
  String err;
  const bool ok = captureCc1101Raw(
      timeoutMs > 0 ? timeoutMs : 8000, minEdges > 0 ? minEdges : 8,
      gapUs > 0 ? gapUs : 4000, timings, err,
      [ctx]() { return !yieldHost(); });
  if (yieldHost()) {
    return throwStop(ctx);
  }
  if (!ok) {
    if (err.length()) {
      return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
    }
    duk_push_null(ctx);
    return 1;
  }
  pushTimings(ctx, timings);
  return 1;
}

duk_ret_t cc1101Replay(duk_context *ctx) {
  std::vector<uint16_t> timings;
  String err;
  if (!getTimings(ctx, 0, timings, 4000, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  const int repeat = duk_opt_int(ctx, 1, 1);
  const int gapUs = duk_opt_int(ctx, 2, 10000);
  if (!replayCc1101Raw(timings.data(), timings.size(), repeat,
                       gapUs > 0 ? gapUs : 0, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101TxFixedCode(duk_context *ctx) {
  const double code = duk_require_number(ctx, 0);
  const int bits = duk_require_int(ctx, 1);
  const int pulseLength = duk_opt_int(ctx, 2, 350);
  const int protocol = duk_opt_int(ctx, 3, 1);
  const int repeat = duk_opt_int(ctx, 4, 10);

  String err;
  if (!transmitCc1101(static_cast<uint32_t>(code), bits, pulseLength,
                      protocol, repeat, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101PacketGet(duk_context *ctx) {
  const Cc1101PacketConfig &cfg = getCc1101PacketConfig();
  duk_push_object(ctx);
  duk_push_uint(ctx, cfg.modulation);
  duk_put_prop_string(ctx, -2, "modulation");
  duk_push_uint(ctx, cfg.channel);
  duk_put_prop_string(ctx, -2, "channel");
  duk_push_number(ctx, cfg.dataRateKbps);
  duk_put_prop_string(ctx, -2, "dataRateKbps");
  duk_push_number(ctx, cfg.deviationKHz);
  duk_put_prop_string(ctx, -2, "deviationKHz");
  duk_push_number(ctx, cfg.rxBandwidthKHz);
  duk_put_prop_string(ctx, -2, "rxBandwidthKHz");
  duk_push_uint(ctx, cfg.syncMode);
  duk_put_prop_string(ctx, -2, "syncMode");
  duk_push_uint(ctx, cfg.packetFormat);
  duk_put_prop_string(ctx, -2, "packetFormat");
  duk_push_boolean(ctx, cfg.crcEnabled);
  duk_put_prop_string(ctx, -2, "crcEnabled");
  duk_push_uint(ctx, cfg.lengthConfig);
  duk_put_prop_string(ctx, -2, "lengthConfig");
  duk_push_uint(ctx, cfg.packetLength);
  duk_put_prop_string(ctx, -2, "packetLength");
  duk_push_boolean(ctx, cfg.whitening);
  duk_put_prop_string(ctx, -2, "whitening");
  duk_push_boolean(ctx, cfg.manchester);
  duk_put_prop_string(ctx, -2, "manchester");
  return 1;
}

duk_ret_t cc1101PacketSet(duk_context *ctx) {
  duk_require_object(ctx, 0);
  Cc1101PacketConfig cfg = getCc1101PacketConfig();
  double num = 0;
  bool flag = false;
  if (getNumberProp(ctx, 0, "modulation", num)) cfg.modulation = static_cast<uint8_t>(num);
  if (getNumberProp(ctx, 0, "channel", num)) cfg.channel = static_cast<uint8_t>(num);
  if (getNumberProp(ctx, 0, "dataRateKbps", num)) cfg.dataRateKbps = static_cast<float>(num);
  if (getNumberProp(ctx, 0, "deviationKHz", num)) cfg.deviationKHz = static_cast<float>(num);
  if (getNumberProp(ctx, 0, "rxBandwidthKHz", num)) cfg.rxBandwidthKHz = static_cast<float>(num);
  if (getNumberProp(ctx, 0, "syncMode", num)) cfg.syncMode = static_cast<uint8_t>(num);
  if (getNumberProp(ctx, 0, "packetFormat", num)) cfg.packetFormat = static_cast<uint8_t>(num);
  if (getBoolProp(ctx, 0, "crcEnabled", flag)) cfg.crcEnabled = flag;
  if (getNumberProp(ctx, 0, "lengthConfig", num)) cfg.lengthConfig = static_cast<uint8_t>(num);
  if (getNumberProp(ctx, 0, "packetLength", num)) cfg.packetLength = static_cast<uint8_t>(num);
  if (getBoolProp(ctx, 0, "whitening", flag)) cfg.whitening = flag;
  if (getBoolProp(ctx, 0, "manchester", flag)) cfg.manchester = flag;

  String err;
  if (!configureCc1101Packet(cfg, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101PacketSend(duk_context *ctx) {
  std::vector<uint8_t> bytes;
  String err;
  if (!getBytes(ctx, 0, bytes, 61, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  const int txDelayMs = duk_opt_int(ctx, 1, 25);
  if (!sendCc1101Packet(bytes.data(), bytes.size(), txDelayMs, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t cc1101PacketReceive(duk_context *ctx) {
  const int timeoutMs = duk_opt_int(ctx, 0, 5000);
  std::vector<uint8_t> packet;
  int rssi = 0;
  String err;
  // The library call itself does not yield; approximate cooperative
  // scheduling by chunking the wait.
  const uint32_t startMs = millis();
  bool ok = false;
  do {
    ok = receiveCc1101Packet(packet, 100, &rssi, err);
    if (ok || err.length()) {
      break;
    }
    if (yieldHost()) {
      return throwStop(ctx);
    }
  } while (millis() - startMs < static_cast<uint32_t>(timeoutMs > 0 ? timeoutMs : 5000));

  if (!ok) {
    if (err.length()) {
      return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
    }
    duk_push_null(ctx);
    return 1;
  }
  duk_push_object(ctx);
  pushBytes(ctx, packet.data(), packet.size());
  duk_put_prop_string(ctx, -2, "data");
  duk_push_int(ctx, rssi);
  duk_put_prop_string(ctx, -2, "rssiDbm");
  return 1;
}

}  // namespace

void registerCc1101Module(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "info", cc1101Info, 0);
  putFunction(ctx, "setFreq", cc1101SetFreq, 1);
  putFunction(ctx, "setPower", cc1101SetPower, 1);
  putFunction(ctx, "readRssi", cc1101ReadRssi, 0);
  putFunction(ctx, "readReg", cc1101ReadReg, 1);
  putFunction(ctx, "writeReg", cc1101WriteReg, 2);
  putFunction(ctx, "strobe", cc1101Strobe, 1);
  putFunction(ctx, "setMode", cc1101SetMode, 1);
  putFunction(ctx, "capture", cc1101Capture, 3);
  putFunction(ctx, "replay", cc1101Replay, 3);
  putFunction(ctx, "txFixedCode", cc1101TxFixedCode, 5);
  putFunction(ctx, "packetGet", cc1101PacketGet, 0);
  putFunction(ctx, "packetSet", cc1101PacketSet, 1);
  putFunction(ctx, "send", cc1101PacketSend, 2);
  putFunction(ctx, "receive", cc1101PacketReceive, 1);
  duk_put_prop_string(ctx, -2, "cc1101");
  duk_pop(ctx);
}

}  // namespace zxjs
