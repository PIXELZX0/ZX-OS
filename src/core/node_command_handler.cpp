#include "node_command_handler.h"

#include <limits.h>
#include <WiFi.h>

#include "cc1101_radio.h"
#include "gateway_client.h"
#include "js/js_engine.h"

namespace {

struct ArgList {
  static constexpr size_t kMaxArgs = 8;
  String values[kMaxArgs];
  size_t count = 0;
};

bool parseArgArray(JsonVariantConst value, ArgList &outArgs) {
  if (!value.is<JsonArrayConst>()) {
    return false;
  }

  JsonArrayConst arr = value.as<JsonArrayConst>();
  outArgs.count = 0;

  for (JsonVariantConst item : arr) {
    if (outArgs.count >= ArgList::kMaxArgs) {
      break;
    }

    if (item.is<const char *>()) {
      outArgs.values[outArgs.count++] = String(item.as<const char *>());
    } else if (item.is<long>() || item.is<unsigned long>() || item.is<long long>() ||
               item.is<unsigned long long>() || item.is<float>() || item.is<double>()) {
      outArgs.values[outArgs.count++] = item.as<String>();
    } else {
      return false;
    }
  }

  return outArgs.count > 0;
}

bool parseUInt64Token(const String &token, uint64_t &out) {
  char *endPtr = nullptr;
  const unsigned long long value = strtoull(token.c_str(), &endPtr, 0);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<uint64_t>(value);
  return true;
}

bool parseIntToken(const String &token, int &out) {
  char *endPtr = nullptr;
  const long value = strtol(token.c_str(), &endPtr, 10);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool parseFloatToken(const String &token, float &out) {
  char *endPtr = nullptr;
  const float value = strtof(token.c_str(), &endPtr);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = value;
  return true;
}

bool readFloatFromJson(JsonVariantConst v, float &out) {
  if (v.is<float>()) {
    out = v.as<float>();
    return true;
  }
  if (v.is<double>()) {
    out = static_cast<float>(v.as<double>());
    return true;
  }
  if (v.is<const char *>()) {
    return parseFloatToken(String(v.as<const char *>()), out);
  }
  return false;
}

bool readIntFromJson(JsonVariantConst v, int &out) {
  if (v.is<int>()) {
    out = v.as<int>();
    return true;
  }
  if (v.is<long>()) {
    out = static_cast<int>(v.as<long>());
    return true;
  }
  if (v.is<unsigned long>()) {
    const unsigned long value = v.as<unsigned long>();
    if (value > static_cast<unsigned long>(INT_MAX)) {
      return false;
    }
    out = static_cast<int>(value);
    return true;
  }
  if (v.is<const char *>()) {
    return parseIntToken(String(v.as<const char *>()), out);
  }
  return false;
}

bool readBoolFromJson(JsonVariantConst v, bool &out) {
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }
  if (v.is<int>()) {
    out = v.as<int>() != 0;
    return true;
  }
  if (v.is<const char *>()) {
    String text = String(v.as<const char *>());
    text.trim();
    text.toLowerCase();
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
      out = true;
      return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
      out = false;
      return true;
    }
  }
  return false;
}

bool readUInt32FromJson(JsonVariantConst v, uint32_t &out) {
  if (v.is<uint32_t>()) {
    out = v.as<uint32_t>();
    return true;
  }
  if (v.is<int>()) {
    const int iv = v.as<int>();
    if (iv < 0) {
      return false;
    }
    out = static_cast<uint32_t>(iv);
    return true;
  }
  if (v.is<const char *>()) {
    const String text = String(v.as<const char *>());
    char *endPtr = nullptr;
    const unsigned long value = strtoul(text.c_str(), &endPtr, 0);
    if (endPtr == text.c_str() || *endPtr != '\0') {
      return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
  }
  return false;
}

bool readUInt64FromJson(JsonVariantConst v, uint64_t &out) {
  if (v.is<uint64_t>()) {
    out = v.as<uint64_t>();
    return true;
  }
  if (v.is<unsigned long long>()) {
    out = static_cast<uint64_t>(v.as<unsigned long long>());
    return true;
  }
  if (v.is<const char *>()) {
    return parseUInt64Token(String(v.as<const char *>()), out);
  }
  return false;
}

void appendPacketConfigPayload(JsonObject obj, const Cc1101PacketConfig &cfg) {
  obj["modulation"] = cfg.modulation;
  obj["channel"] = cfg.channel;
  obj["dataRateKbps"] = cfg.dataRateKbps;
  obj["deviationKHz"] = cfg.deviationKHz;
  obj["rxBandwidthKHz"] = cfg.rxBandwidthKHz;
  obj["syncMode"] = cfg.syncMode;
  obj["packetFormat"] = cfg.packetFormat;
  obj["crcEnabled"] = cfg.crcEnabled;
  obj["lengthConfig"] = cfg.lengthConfig;
  obj["packetLength"] = cfg.packetLength;
  obj["whitening"] = cfg.whitening;
  obj["manchester"] = cfg.manchester;
}

String bytesToHex(const std::vector<uint8_t> &bytes) {
  static const char kHex[] = "0123456789ABCDEF";
  String out;
  out.reserve(bytes.size() * 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    const uint8_t b = bytes[i];
    out += kHex[(b >> 4) & 0x0F];
    out += kHex[b & 0x0F];
  }
  return out;
}

String bytesToAscii(const std::vector<uint8_t> &bytes) {
  String out;
  out.reserve(bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    const uint8_t c = bytes[i];
    out += (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
  }
  return out;
}

bool isSupportedBin(const String &bin) {
  return bin == "system.which" ||
         bin == "system.run" ||
         bin == "cc1101.info" ||
         bin == "cc1101.set_freq" ||
         bin == "cc1101.tx" ||
         bin == "cc1101.read_rssi" ||
         bin == "cc1101.packet_get" ||
         bin == "cc1101.packet_set" ||
         bin == "cc1101.packet_tx_text" ||
         bin == "cc1101.packet_rx_once" ||
         bin == "js.eval" ||
         bin == "js.run_file";
}

void buildInfoPayload(JsonObject obj) {
  appendCc1101Info(obj);
  obj["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  obj["wifiRssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  obj["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  obj["uptimeMs"] = millis();
}

}  // namespace

void NodeCommandHandler::setGatewayClient(GatewayClient *gateway) {
  gateway_ = gateway;
}

void NodeCommandHandler::handleInvoke(const String &invokeId,
                                      const String &nodeId,
                                      const String &command,
                                      JsonObjectConst params) {
  if (!gateway_) {
    return;
  }

  if (command == "system.which") {
    if (handleSystemWhich(invokeId, nodeId, params)) {
      return;
    }
    gateway_->sendInvokeError(invokeId,
                              nodeId,
                              "INVALID_REQUEST",
                              "bins array required");
    return;
  }

  if (command == "system.run") {
    if (handleSystemRun(invokeId, nodeId, params)) {
      return;
    }
    return;
  }

  if (command.startsWith("cc1101.")) {
    if (handleCc1101Command(invokeId, nodeId, command, params)) {
      return;
    }
    return;
  }

  if (command.startsWith("js.")) {
    if (handleJsCommand(invokeId, nodeId, command, params)) {
      return;
    }
    return;
  }

  gateway_->sendInvokeError(invokeId,
                            nodeId,
                            "UNAVAILABLE",
                            "command not supported");
}

bool NodeCommandHandler::handleSystemWhich(const String &invokeId,
                                           const String &nodeId,
                                           JsonObjectConst params) {
  if (!params["bins"].is<JsonArrayConst>()) {
    return false;
  }

  DynamicJsonDocument payload(768);
  JsonObject binsOut = payload.createNestedObject("bins");
  JsonArrayConst bins = params["bins"].as<JsonArrayConst>();

  for (JsonVariantConst v : bins) {
    if (!v.is<const char *>()) {
      continue;
    }

    const String bin = v.as<const char *>();
    if (isSupportedBin(bin)) {
      binsOut[bin] = "builtin://t-embed-cc1101";
    }
  }

  gateway_->sendInvokeOk(invokeId, nodeId, payload);
  return true;
}

bool NodeCommandHandler::handleSystemRun(const String &invokeId,
                                         const String &nodeId,
                                         JsonObjectConst params) {
  ArgList args;
  if (!parseArgArray(params["command"], args)) {
    gateway_->sendInvokeError(invokeId,
                              nodeId,
                              "INVALID_REQUEST",
                              "command array required");
    return true;
  }

  String stdoutText;
  String stderrText;
  int exitCode = 0;
  bool success = false;

  DynamicJsonDocument resultPayload(1024);
  JsonObject result = resultPayload.to<JsonObject>();

  const String cmd = args.values[0];

  if (cmd == "cc1101.info") {
    buildInfoPayload(result);
    serializeJson(resultPayload, stdoutText);
    success = true;
  } else if (cmd == "cc1101.set_freq") {
    if (args.count < 2) {
      exitCode = 2;
      stderrText = "usage: cc1101.set_freq <mhz>";
    } else {
      float mhz = 0.0f;
      if (!parseFloatToken(args.values[1], mhz)) {
        exitCode = 2;
        stderrText = "invalid frequency";
      } else {
        setCc1101FrequencyMhz(mhz);
        result["frequencyMhz"] = getCc1101FrequencyMhz();
        result["applied"] = true;
        serializeJson(resultPayload, stdoutText);
        success = true;
      }
    }
  } else if (cmd == "cc1101.tx") {
    if (args.count < 3) {
      exitCode = 2;
      stderrText = "usage: cc1101.tx <code> <bits> [pulseLength] [protocol] [repeat]";
    } else {
      uint64_t code64 = 0;
      int bits = 0;
      int pulseLength = 350;
      int protocol = 1;
      int repeat = 10;

      if (!parseUInt64Token(args.values[1], code64) || code64 > 0xFFFFFFFFULL) {
        exitCode = 2;
        stderrText = "code must be uint32";
      } else if (!parseIntToken(args.values[2], bits)) {
        exitCode = 2;
        stderrText = "invalid bits";
      } else if (args.count >= 4 && !parseIntToken(args.values[3], pulseLength)) {
        exitCode = 2;
        stderrText = "invalid pulseLength";
      } else if (args.count >= 5 && !parseIntToken(args.values[4], protocol)) {
        exitCode = 2;
        stderrText = "invalid protocol";
      } else if (args.count >= 6 && !parseIntToken(args.values[5], repeat)) {
        exitCode = 2;
        stderrText = "invalid repeat";
      } else {
        String txErr;
        if (!transmitCc1101(static_cast<uint32_t>(code64),
                            bits,
                            pulseLength,
                            protocol,
                            repeat,
                            txErr)) {
          exitCode = 1;
          stderrText = txErr;
        } else {
          result["sent"] = true;
          result["code"] = static_cast<uint32_t>(code64);
          result["bits"] = bits;
          result["pulseLength"] = pulseLength;
          result["protocol"] = protocol;
          result["repeat"] = repeat;
          result["frequencyMhz"] = getCc1101FrequencyMhz();
          serializeJson(resultPayload, stdoutText);
          success = true;
        }
      }
    }
  } else if (cmd == "cc1101.read_rssi") {
    String rssiErr;
    const int rssi = readCc1101RssiDbm(&rssiErr);
    if (!rssiErr.isEmpty()) {
      exitCode = 1;
      stderrText = rssiErr;
    } else {
      result["rssiDbm"] = rssi;
      serializeJson(resultPayload, stdoutText);
      success = true;
    }
  } else if (cmd == "cc1101.packet_get") {
    appendPacketConfigPayload(result, getCc1101PacketConfig());
    serializeJson(resultPayload, stdoutText);
    success = true;
  } else if (cmd == "cc1101.packet_tx_text") {
    if (args.count < 2) {
      exitCode = 2;
      stderrText = "usage: cc1101.packet_tx_text <text> [txDelayMs]";
    } else {
      int txDelayMs = 25;
      if (args.count >= 3 && !parseIntToken(args.values[2], txDelayMs)) {
        exitCode = 2;
        stderrText = "invalid txDelayMs";
      } else {
        String txErr;
        if (!sendCc1101PacketText(args.values[1], txDelayMs, txErr)) {
          exitCode = 1;
          stderrText = txErr;
        } else {
          result["sent"] = true;
          result["bytes"] = args.values[1].length();
          result["txDelayMs"] = txDelayMs;
          serializeJson(resultPayload, stdoutText);
          success = true;
        }
      }
    }
  } else if (cmd == "cc1101.packet_rx_once") {
    int timeoutMs = 5000;
    if (args.count >= 2 && !parseIntToken(args.values[1], timeoutMs)) {
      exitCode = 2;
      stderrText = "invalid timeoutMs";
    } else {
      std::vector<uint8_t> packet;
      int rssi = 0;
      String rxErr;
      if (!receiveCc1101Packet(packet, timeoutMs, &rssi, rxErr)) {
        exitCode = 1;
        stderrText = rxErr;
      } else {
        result["size"] = static_cast<uint32_t>(packet.size());
        result["rssiDbm"] = rssi;
        result["hex"] = bytesToHex(packet);
        result["ascii"] = bytesToAscii(packet);
        serializeJson(resultPayload, stdoutText);
        success = true;
      }
    }
  } else {
    exitCode = 127;
    stderrText = "unsupported command: " + cmd;
  }

  DynamicJsonDocument payload(3072);
  payload["exitCode"] = exitCode;
  payload["timedOut"] = false;
  payload["success"] = success;
  payload["stdout"] = stdoutText;
  payload["stderr"] = stderrText;
  if (success) {
    payload["error"] = nullptr;
  } else {
    payload["error"] = stderrText;
  }
  payload["truncated"] = false;
  if (resultPayload.size() > 0) {
    payload["result"] = resultPayload.as<JsonVariantConst>();
  }

  gateway_->sendInvokeOk(invokeId, nodeId, payload);
  return true;
}

bool NodeCommandHandler::handleCc1101Command(const String &invokeId,
                                             const String &nodeId,
                                             const String &command,
                                             JsonObjectConst params) {
  DynamicJsonDocument payload(1024);

  if (command == "cc1101.info") {
    buildInfoPayload(payload.to<JsonObject>());
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.set_freq") {
    float mhz = 0.0f;
    if (!readFloatFromJson(params["mhz"], mhz)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "mhz is required");
      return true;
    }

    setCc1101FrequencyMhz(mhz);
    payload["frequencyMhz"] = getCc1101FrequencyMhz();
    payload["applied"] = true;
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.tx") {
    uint64_t code64 = 0;
    uint32_t bits = 0;
    uint32_t pulseLength = 350;
    uint32_t protocol = 1;
    uint32_t repeat = 10;

    if (!readUInt64FromJson(params["code"], code64) || code64 > 0xFFFFFFFFULL) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "code must be uint32");
      return true;
    }
    if (!readUInt32FromJson(params["bits"], bits)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "bits is required");
      return true;
    }
    if (!params["pulseLength"].isNull() &&
        !readUInt32FromJson(params["pulseLength"], pulseLength)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid pulseLength");
      return true;
    }
    if (!params["protocol"].isNull() &&
        !readUInt32FromJson(params["protocol"], protocol)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid protocol");
      return true;
    }
    if (!params["repeat"].isNull() &&
        !readUInt32FromJson(params["repeat"], repeat)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid repeat");
      return true;
    }

    String txErr;
    if (!transmitCc1101(static_cast<uint32_t>(code64),
                        static_cast<int>(bits),
                        static_cast<int>(pulseLength),
                        static_cast<int>(protocol),
                        static_cast<int>(repeat),
                        txErr)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "UNAVAILABLE",
                                txErr);
      return true;
    }

    payload["sent"] = true;
    payload["code"] = static_cast<uint32_t>(code64);
    payload["bits"] = bits;
    payload["pulseLength"] = pulseLength;
    payload["protocol"] = protocol;
    payload["repeat"] = repeat;
    payload["frequencyMhz"] = getCc1101FrequencyMhz();
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.read_rssi") {
    String rssiErr;
    const int rssi = readCc1101RssiDbm(&rssiErr);
    if (!rssiErr.isEmpty()) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "UNAVAILABLE",
                                rssiErr);
      return true;
    }

    payload["rssiDbm"] = rssi;
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.packet_get") {
    appendPacketConfigPayload(payload.to<JsonObject>(), getCc1101PacketConfig());
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.packet_set") {
    Cc1101PacketConfig cfg = getCc1101PacketConfig();
    uint32_t u32 = 0;
    float fval = 0.0f;
    bool bval = false;

    if (!params["modulation"].isNull()) {
      if (!readUInt32FromJson(params["modulation"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid modulation");
        return true;
      }
      cfg.modulation = static_cast<uint8_t>(u32);
    }
    if (!params["channel"].isNull()) {
      if (!readUInt32FromJson(params["channel"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid channel");
        return true;
      }
      cfg.channel = static_cast<uint8_t>(u32);
    }
    if (!params["dataRateKbps"].isNull()) {
      if (!readFloatFromJson(params["dataRateKbps"], fval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid dataRateKbps");
        return true;
      }
      cfg.dataRateKbps = fval;
    }
    if (!params["deviationKHz"].isNull()) {
      if (!readFloatFromJson(params["deviationKHz"], fval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid deviationKHz");
        return true;
      }
      cfg.deviationKHz = fval;
    }
    if (!params["rxBandwidthKHz"].isNull()) {
      if (!readFloatFromJson(params["rxBandwidthKHz"], fval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid rxBandwidthKHz");
        return true;
      }
      cfg.rxBandwidthKHz = fval;
    }
    if (!params["syncMode"].isNull()) {
      if (!readUInt32FromJson(params["syncMode"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid syncMode");
        return true;
      }
      cfg.syncMode = static_cast<uint8_t>(u32);
    }
    if (!params["packetFormat"].isNull()) {
      if (!readUInt32FromJson(params["packetFormat"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid packetFormat");
        return true;
      }
      cfg.packetFormat = static_cast<uint8_t>(u32);
    }
    if (!params["crcEnabled"].isNull()) {
      if (!readBoolFromJson(params["crcEnabled"], bval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid crcEnabled");
        return true;
      }
      cfg.crcEnabled = bval;
    }
    if (!params["lengthConfig"].isNull()) {
      if (!readUInt32FromJson(params["lengthConfig"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid lengthConfig");
        return true;
      }
      cfg.lengthConfig = static_cast<uint8_t>(u32);
    }
    if (!params["packetLength"].isNull()) {
      if (!readUInt32FromJson(params["packetLength"], u32) || u32 > 255) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid packetLength");
        return true;
      }
      cfg.packetLength = static_cast<uint8_t>(u32);
    }
    if (!params["whitening"].isNull()) {
      if (!readBoolFromJson(params["whitening"], bval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid whitening");
        return true;
      }
      cfg.whitening = bval;
    }
    if (!params["manchester"].isNull()) {
      if (!readBoolFromJson(params["manchester"], bval)) {
        gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid manchester");
        return true;
      }
      cfg.manchester = bval;
    }

    String applyErr;
    if (!configureCc1101Packet(cfg, applyErr)) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", applyErr);
      return true;
    }

    payload["applied"] = true;
    appendPacketConfigPayload(payload.as<JsonObject>(), getCc1101PacketConfig());
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.packet_tx_text") {
    const String text = params["text"].as<String>();
    if (text.isEmpty()) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "text is required");
      return true;
    }

    int txDelayMs = 25;
    if (!params["txDelayMs"].isNull() && !readIntFromJson(params["txDelayMs"], txDelayMs)) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid txDelayMs");
      return true;
    }

    String txErr;
    if (!sendCc1101PacketText(text, txDelayMs, txErr)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "UNAVAILABLE",
                                txErr);
      return true;
    }

    payload["sent"] = true;
    payload["bytes"] = text.length();
    payload["txDelayMs"] = txDelayMs;
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.packet_rx_once") {
    int timeoutMs = 5000;
    if (!params["timeoutMs"].isNull() && !readIntFromJson(params["timeoutMs"], timeoutMs)) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid timeoutMs");
      return true;
    }

    std::vector<uint8_t> packet;
    int rssi = 0;
    String rxErr;
    if (!receiveCc1101Packet(packet, timeoutMs, &rssi, rxErr)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "UNAVAILABLE",
                                rxErr);
      return true;
    }

    payload["size"] = static_cast<uint32_t>(packet.size());
    payload["rssiDbm"] = rssi;
    payload["hex"] = bytesToHex(packet);
    payload["ascii"] = bytesToAscii(packet);
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  gateway_->sendInvokeError(invokeId,
                            nodeId,
                            "UNAVAILABLE",
                            "unsupported cc1101 command");
  return true;
}

bool NodeCommandHandler::handleJsCommand(const String &invokeId,
                                         const String &nodeId,
                                         const String &command,
                                         JsonObjectConst params) {
  DynamicJsonDocument payload(2048);

  if (command != "js.eval" && command != "js.run_file") {
    gateway_->sendInvokeError(invokeId, nodeId, "UNAVAILABLE",
                              "unsupported js command");
    return true;
  }

  if (zxjs::isBusy()) {
    gateway_->sendInvokeError(invokeId, nodeId, "UNAVAILABLE",
                              "another script is already running");
    return true;
  }

  int timeoutMs = 10000;
  if (!params["timeoutMs"].isNull() &&
      !readIntFromJson(params["timeoutMs"], timeoutMs)) {
    gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST",
                              "invalid timeoutMs");
    return true;
  }
  if (timeoutMs < 1 || timeoutMs > 30000) {
    timeoutMs = 10000;
  }

  String log;
  zxjs::Host host;
  host.print = [&log](const String &line) {
    if (log.length() < 4000) {
      log += line;
      log += '\n';
    }
  };

  zxjs::Result result;
  if (command == "js.eval") {
    const String source = params["source"].as<String>();
    if (source.isEmpty()) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST",
                                "source is required");
      return true;
    }
    result = zxjs::runSource(source, "gateway.js", host,
                             static_cast<uint32_t>(timeoutMs));
  } else {
    const String path = params["path"].as<String>();
    if (path.isEmpty()) {
      gateway_->sendInvokeError(invokeId, nodeId, "INVALID_REQUEST",
                                "path is required");
      return true;
    }
    result = zxjs::runFile(path, host, static_cast<uint32_t>(timeoutMs));
  }

  payload["ok"] = result.ok;
  payload["cancelled"] = result.cancelled;
  payload["timedOut"] = result.timedOut;
  payload["log"] = log;
  if (result.ok) {
    payload["value"] = result.value;
  } else {
    payload["error"] = result.error;
  }
  gateway_->sendInvokeOk(invokeId, nodeId, payload);
  return true;
}
