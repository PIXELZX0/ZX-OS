#include "gateway_client.h"

#include <Ed25519.h>
#include <SHA256.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <ctype.h>
#include <time.h>

#include "user_config.h"

namespace {

constexpr const char *OPENCLAW_CLIENT_ID = "node-host";
constexpr const char *OPENCLAW_CLIENT_MODE = "node";
constexpr const char *OPENCLAW_CLIENT_VERSION = "0.3.0";
constexpr int OPENCLAW_PROTOCOL_MIN = 1;
constexpr int OPENCLAW_PROTOCOL_MAX = 3;

constexpr unsigned long kReconnectRetryMs = 2000UL;
constexpr unsigned long kConnectDelayMs = 750UL;
constexpr unsigned long kLowMemRetryMs = 8000UL;
constexpr unsigned long kTlsFailMaxBackoffMs = 30000UL;
constexpr unsigned long kConnectAttemptTimeoutMs = 9000UL;
constexpr uint32_t kTlsMinInternalFreeBytes = 36000U;
constexpr uint32_t kTlsMinInternalLargestBytes = 18000U;

constexpr size_t kDevicePrivateKeyLen = 32;
constexpr size_t kDevicePublicKeyLen = 32;
constexpr size_t kDeviceSignatureLen = 64;
constexpr size_t kGatewayFrameDocCapacity = 8192;
constexpr size_t kGatewayFrameFilterCapacity = 1024;
constexpr size_t kMaxGatewayFrameBytes = 131072;
constexpr size_t kMaxGatewaySendFrameBytes = 6144;
constexpr size_t kMaxMsgIdLen = 96;
constexpr size_t kMaxMsgMetaLen = 64;
constexpr size_t kMaxMsgTextLen = 768;
constexpr size_t kMaxMsgFileNameLen = 128;

bool isMarkupTagNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool isControlChatTag(const String &tagName) {
  return tagName.equalsIgnoreCase("analysis") ||
         tagName.equalsIgnoreCase("commentary") ||
         tagName.equalsIgnoreCase("final");
}

String stripControlChatTags(const String &text) {
  if (text.isEmpty() || text.indexOf('<') < 0) {
    return text;
  }

  String cleaned;
  cleaned.reserve(text.length());

  const size_t len = text.length();
  size_t i = 0;
  while (i < len) {
    if (text[i] != '<') {
      cleaned += text[i];
      ++i;
      continue;
    }

    size_t cursor = i + 1;
    while (cursor < len &&
           isspace(static_cast<unsigned char>(text[cursor]))) {
      ++cursor;
    }
    if (cursor < len && text[cursor] == '/') {
      ++cursor;
    }
    while (cursor < len &&
           isspace(static_cast<unsigned char>(text[cursor]))) {
      ++cursor;
    }

    const size_t nameStart = cursor;
    while (cursor < len && isMarkupTagNameChar(text[cursor])) {
      ++cursor;
    }

    if (nameStart == cursor) {
      cleaned += text[i];
      ++i;
      continue;
    }

    const String tagName = text.substring(nameStart, cursor);
    if (!isControlChatTag(tagName)) {
      cleaned += text[i];
      ++i;
      continue;
    }

    // Handle both complete tags ("</final>") and split tail chunks ("</final").
    while (cursor < len && text[cursor] != '>') {
      ++cursor;
    }
    i = cursor < len ? (cursor + 1) : cursor;
  }

  return cleaned;
}

bool startsWithErrorMarker(const String &text) {
  if (text.isEmpty()) {
    return false;
  }

  String normalized = text;
  normalized.trim();
  return normalized.startsWith("[error]");
}

void clampString(String &value, size_t maxLen) {
  if (maxLen == 0) {
    value = "";
    return;
  }
  if (value.length() <= maxLen) {
    return;
  }
  if (maxLen <= 3) {
    value = value.substring(0, maxLen);
    return;
  }
  value = value.substring(0, maxLen - 3) + "...";
}

void clampInboxMessage(GatewayInboxMessage &message) {
  clampString(message.id, kMaxMsgIdLen);
  clampString(message.event, kMaxMsgMetaLen);
  clampString(message.type, kMaxMsgMetaLen);
  clampString(message.from, kMaxMsgMetaLen);
  clampString(message.to, kMaxMsgMetaLen);
  clampString(message.fileName, kMaxMsgFileNameLen);
  clampString(message.contentType, kMaxMsgMetaLen);
  clampString(message.text, kMaxMsgTextLen);
}

bool hasTlsHeapHeadroom() {
  const uint32_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return freeBytes >= kTlsMinInternalFreeBytes &&
         largest >= kTlsMinInternalLargestBytes;
}

String wsReasonText(const uint8_t *payload, size_t length) {
  if (!payload || length == 0) {
    return "";
  }

  String reason;
  reason.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(payload[i]);
    if ((c < 32 || c > 126) && c != '\t') {
      return "";
    }
    reason += c;
  }
  reason.trim();
  if (reason.length() > 96) {
    reason = reason.substring(0, 96);
  }
  return reason;
}

}  // namespace

void GatewayClient::begin() {
  if (initialized_) {
    return;
  }
  ws_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
    onWsEvent(type, payload, length);
  });
  ws_.setReconnectInterval(kReconnectRetryMs);
  ws_.enableHeartbeat(15000, 3000, 2);

  initialized_ = true;
}

void GatewayClient::setInvokeRequestHandler(InvokeRequestHandler handler) {
  invokeHandler_ = handler;
}

void GatewayClient::setTelemetryBuilder(TelemetryBuilder builder) {
  telemetryBuilder_ = builder;
}

void GatewayClient::configure(const RuntimeConfig &config) {
  config_ = config;
}

void GatewayClient::connectNow() {
  shouldConnect_ = true;
  if (!wsStarted_) {
    startWebSocket();
  }
}

void GatewayClient::disconnectNow() {
  shouldConnect_ = false;
  gatewayReady_ = false;
  wsConnected_ = false;
  connectRequestId_ = "";
  connectNonce_ = "";
  connectChallengeTsMs_ = 0;
  connectQueuedAtMs_ = 0;
  connectSent_ = false;
  connectAttemptStartedMs_ = 0;
  connectUsedDeviceToken_ = false;
  connectCanFallbackToShared_ = false;
  tlsFailStreak_ = 0;

  if (wsStarted_) {
    ws_.disconnect();
    wsStarted_ = false;
  }
}

void GatewayClient::reconnectNow() {
  disconnectNow();
  shouldConnect_ = true;
  startWebSocket();
}

void GatewayClient::tick() {
  if (!initialized_) {
    return;
  }

  if (wsStarted_) {
    ws_.loop();
  }

  if (wsStarted_ && !wsConnected_ && connectAttemptStartedMs_ > 0) {
    const unsigned long now = millis();
    if (now - connectAttemptStartedMs_ >= kConnectAttemptTimeoutMs) {
      ws_.disconnect();
      wsStarted_ = false;
      gatewayReady_ = false;
      connectSent_ = false;
      connectQueuedAtMs_ = 0;
      connectAttemptStartedMs_ = 0;
      if (tlsFailStreak_ < 0xFFU) {
        ++tlsFailStreak_;
      }
      lastConnectAttemptMs_ = now;
      if (config_.gatewayUrl.startsWith("wss://")) {
        lastError_ = "TLS connect timeout; retrying";
      } else {
        lastError_ = "Gateway connect timeout";
      }
    }
  }

  if (shouldConnect_ && !wsStarted_) {
    const unsigned long now = millis();
    unsigned long retryMs = hasTlsHeapHeadroom() ? kReconnectRetryMs : kLowMemRetryMs;
    if (tlsFailStreak_ > 0) {
      const uint8_t shift = tlsFailStreak_ > 4 ? 4 : tlsFailStreak_;
      const unsigned long backoff = kReconnectRetryMs << shift;
      if (backoff > retryMs) {
        retryMs = backoff;
      }
      if (retryMs > kTlsFailMaxBackoffMs) {
        retryMs = kTlsFailMaxBackoffMs;
      }
    }
    if (now - lastConnectAttemptMs_ >= retryMs) {
      startWebSocket();
    }
  }

  if (wsConnected_ && !connectSent_) {
    const unsigned long now = millis();
    if (now - connectQueuedAtMs_ >= kConnectDelayMs) {
      sendConnectRequest();
    }
  }

  if (gatewayReady_ && telemetryBuilder_) {
    const unsigned long now = millis();
    if (now - lastTelemetryMs_ >= USER_TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs_ = now;
      DynamicJsonDocument payload(1024);
      JsonObject obj = payload.to<JsonObject>();
      telemetryBuilder_(obj);
      sendNodeEvent("cc1101.telemetry", payload);
    }
  }
}

bool GatewayClient::isReady() const {
  return gatewayReady_;
}

String GatewayClient::lastError() const {
  return lastError_;
}

GatewayStatus GatewayClient::status() const {
  GatewayStatus s;
  s.shouldConnect = shouldConnect_;
  s.wsConnected = wsConnected_;
  s.gatewayReady = gatewayReady_;
  s.lastError = lastError_;
  s.lastConnectAttemptMs = lastConnectAttemptMs_;
  s.lastConnectOkMs = lastConnectOkMs_;
  return s;
}

bool GatewayClient::sendNodeEvent(const char *eventName, JsonDocument &payloadDoc) {
  if (!gatewayReady_) {
    return false;
  }
  size_t payloadLen = measureJson(payloadDoc);
  if (payloadLen == 0) {
    payloadLen = 2;  // "{}"
  }

  size_t paramsCapacity = payloadLen + 384U;
  if (paramsCapacity < 768U) {
    paramsCapacity = 768U;
  }
  if (paramsCapacity > kMaxGatewaySendFrameBytes) {
    paramsCapacity = kMaxGatewaySendFrameBytes;
  }

  DynamicJsonDocument params(paramsCapacity);
  params["event"] = eventName;
  params["payload"] = payloadDoc.as<JsonVariantConst>();
  if (params.overflowed()) {
    lastError_ = "Gateway event payload too large";
    return false;
  }
  return sendRequest("node.event", params, nullptr);
}

bool GatewayClient::sendInvokeOk(const String &invokeId,
                                 const String &nodeId,
                                 JsonDocument &payloadDoc) {
  StaticJsonDocument<4096> params;
  params["id"] = invokeId;
  params["nodeId"] = nodeId;
  params["ok"] = true;
  params["payload"] = payloadDoc.as<JsonVariantConst>();
  return sendRequest("node.invoke.result", params, nullptr);
}

bool GatewayClient::sendInvokeError(const String &invokeId,
                                    const String &nodeId,
                                    const char *code,
                                    const String &message) {
  StaticJsonDocument<1024> params;
  params["id"] = invokeId;
  params["nodeId"] = nodeId;
  params["ok"] = false;
  JsonObject error = params.createNestedObject("error");
  error["code"] = code;
  error["message"] = message;
  return sendRequest("node.invoke.result", params, nullptr);
}

size_t GatewayClient::inboxCount() const {
  return inboxCount_;
}

bool GatewayClient::inboxMessage(size_t index, GatewayInboxMessage &out) const {
  if (index >= inboxCount_) {
    return false;
  }

  const size_t pos = (inboxStart_ + index) % kInboxCapacity;
  out = inbox_[pos];
  return true;
}

void GatewayClient::clearInbox() {
  inboxStart_ = 0;
  inboxCount_ = 0;
}

void GatewayClient::onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      {
      const String wsReason = wsReasonText(payload, length);
      wsConnected_ = false;
      gatewayReady_ = false;
      connectRequestId_ = "";
      connectNonce_ = "";
      connectChallengeTsMs_ = 0;
      connectQueuedAtMs_ = 0;
      connectSent_ = false;
      connectAttemptStartedMs_ = 0;
      connectUsedDeviceToken_ = false;
      connectCanFallbackToShared_ = false;
      wsStarted_ = false;
      if (shouldConnect_ && !gatewayReady_ && tlsFailStreak_ < 0xFFU) {
        ++tlsFailStreak_;
      }
      if (shouldConnect_) {
        if (wsReason.length()) {
          lastError_ = "Gateway disconnected: " + wsReason;
        } else if (!lastError_.length()) {
          lastError_ = "Gateway disconnected";
        }
      }
      break;
      }

    case WStype_CONNECTED:
      wsConnected_ = true;
      gatewayReady_ = false;
      lastError_ = "";
      connectRequestId_ = "";
      connectNonce_ = "";
      connectChallengeTsMs_ = 0;
      connectQueuedAtMs_ = millis();
      connectSent_ = false;
      connectAttemptStartedMs_ = 0;
      connectUsedDeviceToken_ = false;
      connectCanFallbackToShared_ = false;
      tlsFailStreak_ = 0;
      break;

    case WStype_TEXT:
      handleGatewayFrame(reinterpret_cast<const char *>(payload), length);
      break;

    case WStype_ERROR:
      {
      const String wsReason = wsReasonText(payload, length);
      lastError_ = wsReason.length() ? ("WebSocket error: " + wsReason) : String("WebSocket error");
      if (tlsFailStreak_ < 0xFFU) {
        ++tlsFailStreak_;
      }
      ws_.disconnect();
      wsStarted_ = false;
      wsConnected_ = false;
      gatewayReady_ = false;
      connectSent_ = false;
      connectQueuedAtMs_ = 0;
      connectAttemptStartedMs_ = 0;
      lastConnectAttemptMs_ = millis();
      break;
      }

    default:
      break;
  }
}

void GatewayClient::startWebSocket() {
  if (!hasTlsHeapHeadroom()) {
    lastError_ = "Gateway retry deferred (low heap)";
    lastConnectAttemptMs_ = millis();
    return;
  }

  String reason;
  if (!canStartConnection(&reason)) {
    lastError_ = reason;
    return;
  }

  GatewayEndpoint endpoint;
  if (!parseGatewayUrl(config_.gatewayUrl, endpoint)) {
    lastError_ = "Invalid gateway URL";
    return;
  }

  if (wsStarted_) {
    ws_.disconnect();
  }

  if (endpoint.secure) {
    // Empty fingerprint intentionally keeps compatibility with self-managed gateways.
    ws_.beginSSL(endpoint.host.c_str(), endpoint.port, endpoint.path.c_str(), "");
  } else {
    ws_.begin(endpoint.host.c_str(), endpoint.port, endpoint.path.c_str());
  }

  wsStarted_ = true;
  wsConnected_ = false;
  gatewayReady_ = false;
  connectRequestId_ = "";
  connectNonce_ = "";
  connectChallengeTsMs_ = 0;
  connectQueuedAtMs_ = 0;
  connectSent_ = false;
  connectAttemptStartedMs_ = millis();
  connectUsedDeviceToken_ = false;
  connectCanFallbackToShared_ = false;
  lastConnectAttemptMs_ = millis();
}

bool GatewayClient::canStartConnection(String *reason) const {
  if (config_.gatewayUrl.isEmpty()) {
    if (reason) {
      *reason = "Gateway URL is empty";
    }
    return false;
  }

  if (!hasGatewayCredentials(config_)) {
    if (reason) {
      *reason = "Gateway credential is missing";
    }
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (reason) {
      *reason = "Wi-Fi is not connected";
    }
    return false;
  }

  return true;
}

bool GatewayClient::sendRequest(const char *method,
                                JsonDocument &paramsDoc,
                                String *requestIdOut) {
  if (!wsConnected_) {
    return false;
  }

  size_t paramsLen = measureJson(paramsDoc);
  if (paramsLen == 0) {
    paramsLen = 2;  // "{}"
  }

  size_t frameCapacity = paramsLen + 768U;
  if (frameCapacity < 1024U) {
    frameCapacity = 1024U;
  }
  if (frameCapacity > kMaxGatewaySendFrameBytes + 1024U) {
    frameCapacity = kMaxGatewaySendFrameBytes + 1024U;
  }

  DynamicJsonDocument frame(frameCapacity);
  const String reqId = nextReqId("req");

  frame["type"] = "req";
  frame["id"] = reqId;
  frame["method"] = method;
  frame["params"] = paramsDoc.as<JsonVariantConst>();
  if (frame.overflowed()) {
    lastError_ = "Gateway send payload too large";
    return false;
  }

  const size_t bodyLen = measureJson(frame);
  if (bodyLen == 0 || bodyLen >= kMaxGatewaySendFrameBytes) {
    lastError_ = "Gateway send frame too large";
    return false;
  }

  String body;
  body.reserve(bodyLen + 1U);
  const size_t written = serializeJson(frame, body);
  if (written != bodyLen || body.length() != bodyLen) {
    lastError_ = "Gateway send serialize failed";
    return false;
  }

  const bool sent = ws_.sendTXT(body);
  if (sent && requestIdOut) {
    *requestIdOut = reqId;
  }
  return sent;
}

void GatewayClient::sendConnectRequest() {
  if (!wsConnected_ || connectSent_) {
    return;
  }

  String identityErr;
  if (!ensureDeviceIdentity(&identityErr)) {
    lastError_ = identityErr.isEmpty() ? String("Device identity unavailable") : identityErr;
    connectSent_ = true;
    return;
  }

  uint8_t privateKey[kDevicePrivateKeyLen] = {0};
  uint8_t publicKey[kDevicePublicKeyLen] = {0};
  if (!decodeBase64Url(config_.gatewayDevicePrivateKey, privateKey, sizeof(privateKey)) ||
      !decodeBase64Url(config_.gatewayDevicePublicKey, publicKey, sizeof(publicKey))) {
    lastError_ = "Device identity decode failed";
    connectSent_ = true;
    return;
  }

  String authToken;
  bool usePassword = false;
  connectUsedDeviceToken_ = false;
  connectCanFallbackToShared_ = false;

  if (!config_.gatewayDeviceToken.isEmpty()) {
    authToken = config_.gatewayDeviceToken;
    connectUsedDeviceToken_ = true;
    connectCanFallbackToShared_ = hasSharedCredential();
  } else if (config_.gatewayAuthMode == GatewayAuthMode::Password) {
    usePassword = true;
  } else {
    authToken = config_.gatewayToken;
  }

  const uint64_t signedAtMs =
      connectChallengeTsMs_ > 0 ? connectChallengeTsMs_ : currentUnixMs();
  const String tokenForSignature = usePassword ? String("") : authToken;
  const String authPayload = buildDeviceAuthPayload(signedAtMs, tokenForSignature);

  uint8_t signatureBytes[kDeviceSignatureLen] = {0};
  Ed25519::sign(signatureBytes,
                privateKey,
                publicKey,
                authPayload.c_str(),
                authPayload.length());
  const String signatureB64 = encodeBase64Url(signatureBytes, sizeof(signatureBytes));
  if (signatureB64.isEmpty()) {
    lastError_ = "Device signature encode failed";
    connectSent_ = true;
    return;
  }

  DynamicJsonDocument params(4096);
  params["minProtocol"] = OPENCLAW_PROTOCOL_MIN;
  params["maxProtocol"] = OPENCLAW_PROTOCOL_MAX;

  JsonObject client = params.createNestedObject("client");
  client["id"] = OPENCLAW_CLIENT_ID;
  client["displayName"] = effectiveDeviceName(config_);
  client["version"] = OPENCLAW_CLIENT_VERSION;
  client["platform"] = "esp32s3";
  client["deviceFamily"] = "lilygo-t-embed-cc1101";
  client["modelIdentifier"] = "T_EMBED_1101";
  client["mode"] = OPENCLAW_CLIENT_MODE;
  client["instanceId"] = USER_OPENCLAW_INSTANCE_ID;

  params["role"] = "node";
  params.createNestedArray("scopes");

  JsonArray caps = params.createNestedArray("caps");
  caps.add("rf");
  caps.add("cc1101");
  caps.add("js");

  JsonArray commands = params.createNestedArray("commands");
  commands.add("system.which");
  commands.add("system.run");
  commands.add("cc1101.info");
  commands.add("cc1101.set_freq");
  commands.add("cc1101.tx");
  commands.add("cc1101.read_rssi");
  commands.add("cc1101.packet_get");
  commands.add("cc1101.packet_set");
  commands.add("cc1101.packet_tx_text");
  commands.add("cc1101.packet_rx_once");
  commands.add("js.eval");
  commands.add("js.run_file");

  JsonObject auth = params.createNestedObject("auth");
  if (usePassword) {
    auth["password"] = config_.gatewayPassword;
  } else {
    auth["token"] = authToken;
  }

  JsonObject device = params.createNestedObject("device");
  device["id"] = config_.gatewayDeviceId;
  device["publicKey"] = config_.gatewayDevicePublicKey;
  device["signature"] = signatureB64;
  device["signedAt"] = static_cast<uint64_t>(signedAtMs);
  if (!connectNonce_.isEmpty()) {
    device["nonce"] = connectNonce_;
  }

  if (!sendRequest("connect", params, &connectRequestId_)) {
    lastError_ = "Failed to send connect request";
    connectSent_ = false;
    return;
  }

  connectSent_ = true;
}

void GatewayClient::handleGatewayFrame(const char *text, size_t len) {
  if (!text || len == 0) {
    return;
  }

  if (len > kMaxGatewayFrameBytes) {
    lastError_ = "Gateway frame too large (" + String(static_cast<unsigned long>(len)) +
                 " bytes)";
    return;
  }

  size_t start = 0;
  while (start < len && isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  if (start >= len) {
    return;
  }

  // Ignore non-JSON control frames that can be sent by some intermediaries.
  if (text[start] != '{') {
    return;
  }

  const size_t parseLen = len - start;
  DynamicJsonDocument filter(kGatewayFrameFilterCapacity);
  filter["type"] = true;
  filter["id"] = true;
  filter["ok"] = true;
  filter["event"] = true;

  JsonObject filterError = filter.createNestedObject("error");
  filterError["message"] = true;

  JsonObject filterPayload = filter.createNestedObject("payload");
  filterPayload["nonce"] = true;
  filterPayload["ts"] = true;
  filterPayload["id"] = true;
  filterPayload["nodeId"] = true;
  filterPayload["command"] = true;
  filterPayload["paramsJSON"] = true;
  filterPayload["messageId"] = true;
  filterPayload["msgId"] = true;
  filterPayload["runId"] = true;
  filterPayload["sessionKey"] = true;
  filterPayload["state"] = true;
  filterPayload["errorMessage"] = true;
  filterPayload["stopReason"] = true;
  filterPayload["seq"] = true;
  filterPayload["type"] = true;
  filterPayload["kind"] = true;
  filterPayload["from"] = true;
  filterPayload["sender"] = true;
  filterPayload["source"] = true;
  filterPayload["to"] = true;
  filterPayload["target"] = true;
  filterPayload["recipient"] = true;
  filterPayload["text"] = true;
  filterPayload["message"] = true;
  filterPayload["body"] = true;
  filterPayload["fileName"] = true;
  filterPayload["name"] = true;
  filterPayload["file"] = true;
  filterPayload["contentType"] = true;
  filterPayload["mime"] = true;
  filterPayload["mimeType"] = true;
  filterPayload["size"] = true;
  filterPayload["bytes"] = true;

  JsonObject filterAuth = filterPayload.createNestedObject("auth");
  filterAuth["deviceToken"] = true;

  DynamicJsonDocument doc(kGatewayFrameDocCapacity);
  const auto err = deserializeJson(doc,
                                   text + start,
                                   parseLen,
                                   DeserializationOption::Filter(filter));
  if (err) {
    if (err == DeserializationError::NoMemory) {
      lastError_ = "Gateway frame too large (" +
                   String(static_cast<unsigned long>(parseLen)) + " bytes)";
    } else {
      lastError_ = "Invalid gateway frame";
    }
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "res") == 0) {
    handleGatewayResponse(doc.as<JsonObjectConst>());
  } else if (strcmp(type, "event") == 0) {
    handleGatewayEvent(doc.as<JsonObjectConst>());
  }
}

void GatewayClient::handleGatewayResponse(JsonObjectConst frame) {
  const String id = frame["id"].as<String>();
  if (id != connectRequestId_) {
    return;
  }

  const bool ok = frame["ok"] | false;
  if (!ok) {
    gatewayReady_ = false;
    const String message = String(static_cast<const char *>(frame["error"]["message"] |
                                                              "Gateway connect rejected"));

    if (connectUsedDeviceToken_ && connectCanFallbackToShared_) {
      config_.gatewayDeviceToken = "";
      persistGatewayConfigBestEffort();
      connectUsedDeviceToken_ = false;
      connectCanFallbackToShared_ = false;
      lastError_ = message + " / retrying with shared auth";
      reconnectNow();
      return;
    }

    lastError_ = message;
    return;
  }

  gatewayReady_ = true;
  lastError_ = "";
  lastConnectOkMs_ = millis();

  if (frame["payload"].is<JsonObjectConst>()) {
    const JsonObjectConst payload = frame["payload"].as<JsonObjectConst>();
    if (payload["auth"].is<JsonObjectConst>()) {
      const JsonObjectConst auth = payload["auth"].as<JsonObjectConst>();
      const String deviceToken = String(static_cast<const char *>(auth["deviceToken"] | ""));
      if (!deviceToken.isEmpty() && deviceToken != config_.gatewayDeviceToken) {
        config_.gatewayDeviceToken = deviceToken;
        persistGatewayConfigBestEffort();
      }
    }
  }

  if (telemetryBuilder_) {
    DynamicJsonDocument payload(1024);
    JsonObject obj = payload.to<JsonObject>();
    telemetryBuilder_(obj);
    sendNodeEvent("cc1101.telemetry", payload);
    lastTelemetryMs_ = millis();
  }
}

void GatewayClient::handleGatewayEvent(JsonObjectConst frame) {
  const String eventName = frame["event"].as<String>();
  const bool hasPayload = frame["payload"].is<JsonObjectConst>();

  if (eventName == "connect.challenge") {
    if (hasPayload) {
      const JsonObjectConst payload = frame["payload"].as<JsonObjectConst>();
      connectNonce_ = String(static_cast<const char *>(payload["nonce"] | ""));
      connectChallengeTsMs_ = payload["ts"] | static_cast<uint64_t>(0);
      if (!connectSent_ && !connectNonce_.isEmpty()) {
        // Defer connect request to tick() to avoid deep call stacks inside WS callback.
        connectQueuedAtMs_ = millis();
      }
    }
    return;
  }

  if (hasPayload) {
    const JsonObjectConst payload = frame["payload"].as<JsonObjectConst>();
    if (captureMessageEvent(eventName, payload)) {
      return;
    }
  }

  if (eventName == "shutdown") {
    gatewayReady_ = false;
    lastError_ = "Gateway shutdown";
    return;
  }

  if (eventName != "node.invoke.request") {
    return;
  }

  if (!frame["payload"].is<JsonObjectConst>()) {
    return;
  }

  const JsonObjectConst payload = frame["payload"].as<JsonObjectConst>();
  const String invokeId = payload["id"].as<String>();
  const String nodeId = payload["nodeId"].as<String>();
  const String command = payload["command"].as<String>();
  const char *paramsJson = payload["paramsJSON"] | nullptr;

  DynamicJsonDocument paramsDoc(2048);
  if (!paramsJson || !paramsJson[0] || strcmp(paramsJson, "null") == 0) {
    paramsDoc.to<JsonObject>();
  } else {
    const auto parseErr = deserializeJson(paramsDoc, paramsJson);
    if (parseErr || !paramsDoc.is<JsonObject>()) {
      sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid paramsJSON");
      return;
    }
  }

  if (!invokeHandler_) {
    sendInvokeError(invokeId, nodeId, "UNAVAILABLE", "invoke handler is not configured");
    return;
  }

  invokeHandler_(invokeId,
                 nodeId,
                 command,
                 paramsDoc.as<JsonObjectConst>());
}

bool GatewayClient::parseGatewayUrl(const String &rawUrl,
                                    GatewayEndpoint &out) const {
  String url = rawUrl;
  url.trim();
  if (url.length() == 0) {
    return false;
  }

  bool secure = false;
  String rest;
  if (url.startsWith("wss://")) {
    secure = true;
    rest = url.substring(6);
  } else if (url.startsWith("ws://")) {
    secure = false;
    rest = url.substring(5);
  } else {
    return false;
  }

  const int slashIndex = rest.indexOf('/');
  const String hostPort = slashIndex >= 0 ? rest.substring(0, slashIndex) : rest;
  const String path = slashIndex >= 0 ? rest.substring(slashIndex) : "/";

  if (hostPort.length() == 0) {
    return false;
  }

  String host = hostPort;
  uint16_t port = secure ? 443 : 80;

  const int colon = hostPort.lastIndexOf(':');
  if (colon > 0 && hostPort.indexOf(']') < 0) {
    host = hostPort.substring(0, colon);
    const String portStr = hostPort.substring(colon + 1);
    const int parsedPort = portStr.toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      return false;
    }
    port = static_cast<uint16_t>(parsedPort);
  }

  out.valid = true;
  out.secure = secure;
  out.host = host;
  out.port = port;
  out.path = path;
  return true;
}

String GatewayClient::nextReqId(const char *prefix) {
  ++reqCounter_;
  String id(prefix);
  id += "-";
  id += String(reqCounter_);
  return id;
}

void GatewayClient::persistGatewayConfigBestEffort() {
  String saveErr;
  if (!saveConfig(config_, &saveErr) && !saveErr.isEmpty()) {
    Serial.println("[gateway] config save warning: " + saveErr);
  }
}

bool GatewayClient::ensureDeviceIdentity(String *error) {
  uint8_t privateKey[kDevicePrivateKeyLen] = {0};
  uint8_t publicKey[kDevicePublicKeyLen] = {0};

  bool hasPrivate = decodeBase64Url(config_.gatewayDevicePrivateKey,
                                    privateKey,
                                    sizeof(privateKey));
  bool hasPublic = decodeBase64Url(config_.gatewayDevicePublicKey,
                                   publicKey,
                                   sizeof(publicKey));

  bool changed = false;

  if (hasPrivate && !hasPublic) {
    Ed25519::derivePublicKey(publicKey, privateKey);
    config_.gatewayDevicePublicKey = encodeBase64Url(publicKey, sizeof(publicKey));
    hasPublic = !config_.gatewayDevicePublicKey.isEmpty();
    changed = true;
  }

  if (!hasPrivate || !hasPublic) {
    esp_fill_random(privateKey, sizeof(privateKey));
    Ed25519::derivePublicKey(publicKey, privateKey);
    config_.gatewayDevicePrivateKey = encodeBase64Url(privateKey, sizeof(privateKey));
    config_.gatewayDevicePublicKey = encodeBase64Url(publicKey, sizeof(publicKey));
    config_.gatewayDeviceToken = "";
    changed = true;
  }

  if (config_.gatewayDevicePrivateKey.isEmpty() || config_.gatewayDevicePublicKey.isEmpty()) {
    if (error) {
      *error = "Failed to generate device keypair";
    }
    return false;
  }

  const String derivedId = sha256Hex(publicKey, sizeof(publicKey));
  if (derivedId.isEmpty()) {
    if (error) {
      *error = "Failed to derive device id";
    }
    return false;
  }

  if (config_.gatewayDeviceId != derivedId) {
    config_.gatewayDeviceId = derivedId;
    changed = true;
  }

  if (changed) {
    persistGatewayConfigBestEffort();
  }

  return true;
}

bool GatewayClient::decodeBase64Url(const String &in, uint8_t *out, size_t outLen) const {
  if (!out || outLen == 0 || in.isEmpty()) {
    return false;
  }

  String normalized = in;
  normalized.replace('-', '+');
  normalized.replace('_', '/');
  while (normalized.length() % 4 != 0) {
    normalized += '=';
  }

  size_t decodedLen = 0;
  const int rc = mbedtls_base64_decode(out,
                                       outLen,
                                       &decodedLen,
                                       reinterpret_cast<const unsigned char *>(normalized.c_str()),
                                       normalized.length());
  return rc == 0 && decodedLen == outLen;
}

String GatewayClient::encodeBase64Url(const uint8_t *data, size_t len) const {
  if (!data || len == 0) {
    return "";
  }

  unsigned char encoded[192] = {0};
  size_t encodedLen = 0;
  const int rc = mbedtls_base64_encode(encoded, sizeof(encoded), &encodedLen, data, len);
  if (rc != 0 || encodedLen == 0 || encodedLen >= sizeof(encoded)) {
    return "";
  }

  encoded[encodedLen] = '\0';
  String out(reinterpret_cast<const char *>(encoded));
  out.replace('+', '-');
  out.replace('/', '_');
  while (out.endsWith("=")) {
    out.remove(out.length() - 1);
  }
  return out;
}

String GatewayClient::sha256Hex(const uint8_t *data, size_t len) const {
  if (!data || len == 0) {
    return "";
  }

  SHA256 hash;
  uint8_t digest[SHA256::HASH_SIZE] = {0};
  hash.reset();
  hash.update(data, len);
  hash.finalize(digest, sizeof(digest));

  static const char kHex[] = "0123456789abcdef";
  char out[(SHA256::HASH_SIZE * 2) + 1] = {0};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
    out[(i * 2) + 1] = kHex[digest[i] & 0x0F];
  }
  out[sizeof(out) - 1] = '\0';
  return String(out);
}

String GatewayClient::buildDeviceAuthPayload(uint64_t signedAtMs,
                                             const String &tokenForSignature) const {
  const String version = connectNonce_.isEmpty() ? "v1" : "v2";

  String payload;
  payload.reserve(256 + tokenForSignature.length() + connectNonce_.length());
  payload += version;
  payload += "|";
  payload += config_.gatewayDeviceId;
  payload += "|";
  payload += OPENCLAW_CLIENT_ID;
  payload += "|";
  payload += OPENCLAW_CLIENT_MODE;
  payload += "|";
  payload += "node";
  payload += "|";
  payload += "";  // scopes csv
  payload += "|";
  payload += String(static_cast<unsigned long long>(signedAtMs));
  payload += "|";
  payload += tokenForSignature;
  if (version == "v2") {
    payload += "|";
    payload += connectNonce_;
  }
  return payload;
}

bool GatewayClient::hasSharedCredential() const {
  if (config_.gatewayAuthMode == GatewayAuthMode::Password) {
    return !config_.gatewayPassword.isEmpty();
  }
  return !config_.gatewayToken.isEmpty();
}

uint64_t GatewayClient::currentUnixMs() const {
  const time_t nowSec = time(nullptr);
  if (nowSec <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(nowSec) * 1000ULL;
}

bool GatewayClient::captureMessageEvent(const String &eventName, JsonObjectConst payload) {
  const bool isChatEvent = eventName == "chat";
  const bool isMessageEvent = eventName.startsWith("msg.") ||
                              eventName.startsWith("message.") ||
                              eventName.startsWith("chat.") ||
                              isChatEvent;
  if (!isMessageEvent) {
    return false;
  }

  // Raw voice chunks are transport frames, not user-visible inbox entries.
  if (eventName.endsWith(".chunk")) {
    return true;
  }

  GatewayInboxMessage message;
  message.event = eventName;
  message.id = readMessageString(payload, "runId", "id", "messageId");
  if (message.id.isEmpty()) {
    message.id = readMessageString(payload, "msgId");
  }
  if (message.id.isEmpty()) {
    message.id = nextReqId("in");
  }

  message.type = readMessageString(payload, "type", "kind");
  if (message.type.isEmpty()) {
    message.type = eventName.indexOf("voice") >= 0 ? "voice" : "text";
  }

  message.from = readMessageString(payload, "from", "sender", "source");
  message.to = readMessageString(payload, "to", "target", "recipient");
  message.text = readMessageString(payload, "text", "message", "body");
  message.fileName = readMessageString(payload, "fileName", "name", "file");
  message.contentType = readMessageString(payload, "contentType", "mime", "mimeType");

  if (isChatEvent) {
    if (message.from.isEmpty()) {
      message.from = "assistant";
    }
    if (message.to.isEmpty()) {
      message.to = readMessageString(payload, "sessionKey");
    }

    if (message.text.isEmpty() && payload["message"].is<JsonObjectConst>()) {
      const JsonObjectConst messageObject = payload["message"].as<JsonObjectConst>();
      if (messageObject["content"].is<JsonArrayConst>()) {
        const JsonArrayConst content = messageObject["content"].as<JsonArrayConst>();
        for (JsonVariantConst item : content) {
          if (!item.is<JsonObjectConst>()) {
            continue;
          }
          const JsonObjectConst block = item.as<JsonObjectConst>();
          const char *blockType = block["type"] | "";
          if (strcmp(blockType, "text") != 0) {
            continue;
          }
          const char *blockText = block["text"] | "";
          if (blockText && blockText[0] != '\0') {
            message.text = String(blockText);
            break;
          }
        }
      }
    }

    if (!message.text.isEmpty()) {
      message.text = stripControlChatTags(message.text);
    }

    if (message.text.isEmpty()) {
      const String state = readMessageString(payload, "state");
      const String errorMessage = readMessageString(payload, "errorMessage");
      if (!errorMessage.isEmpty()) {
        message.text = "[error] " + errorMessage;
      } else if (state == "aborted") {
        message.text = "(aborted)";
      }
    }
  }

  auto readUInt32 = [&](const char *key) -> uint32_t {
    if (!key || !payload.containsKey(key)) {
      return 0;
    }
    const JsonVariantConst value = payload[key];
    if (value.is<uint32_t>()) {
      return value.as<uint32_t>();
    }
    if (value.is<unsigned long long>()) {
      return static_cast<uint32_t>(value.as<unsigned long long>());
    }
    if (value.is<int>()) {
      const int asInt = value.as<int>();
      return asInt > 0 ? static_cast<uint32_t>(asInt) : 0;
    }
    if (value.is<const char *>()) {
      const String asText = String(value.as<const char *>());
      if (asText.isEmpty()) {
        return 0;
      }
      char *endPtr = nullptr;
      const unsigned long parsed = strtoul(asText.c_str(), &endPtr, 10);
      if (endPtr == asText.c_str() || *endPtr != '\0') {
        return 0;
      }
      return static_cast<uint32_t>(parsed);
    }
    return 0;
  };

  message.voiceBytes = readUInt32("size");
  if (message.voiceBytes == 0) {
    message.voiceBytes = readUInt32("bytes");
  }

  uint64_t tsMs = 0;
  if (payload["ts"].is<uint64_t>()) {
    tsMs = payload["ts"].as<uint64_t>();
  } else if (payload["ts"].is<unsigned long long>()) {
    tsMs = static_cast<uint64_t>(payload["ts"].as<unsigned long long>());
  } else if (payload["ts"].is<const char *>()) {
    const String tsText = String(payload["ts"].as<const char *>());
    if (!tsText.isEmpty()) {
      char *endPtr = nullptr;
      const unsigned long long parsed = strtoull(tsText.c_str(), &endPtr, 10);
      if (endPtr != tsText.c_str() && *endPtr == '\0') {
        tsMs = static_cast<uint64_t>(parsed);
      }
    }
  }
  message.tsMs = tsMs > 0 ? tsMs : currentUnixMs();

  if (startsWithErrorMarker(message.text)) {
    return true;
  }

  clampInboxMessage(message);
  pushInboxMessage(message);
  return true;
}

void GatewayClient::pushInboxMessage(const GatewayInboxMessage &message) {
  if (kInboxCapacity == 0) {
    return;
  }

  if (!message.id.isEmpty()) {
    for (size_t i = 0; i < inboxCount_; ++i) {
      const size_t existingPos = (inboxStart_ + i) % kInboxCapacity;
      if (inbox_[existingPos].id == message.id) {
        GatewayInboxMessage merged = message;
        if (merged.text.isEmpty() && !inbox_[existingPos].text.isEmpty()) {
          merged.text = inbox_[existingPos].text;
        }
        inbox_[existingPos] = merged;
        return;
      }
    }
  }

  size_t pos = 0;
  if (inboxCount_ < kInboxCapacity) {
    pos = (inboxStart_ + inboxCount_) % kInboxCapacity;
    ++inboxCount_;
  } else {
    pos = inboxStart_;
    inboxStart_ = (inboxStart_ + 1) % kInboxCapacity;
  }

  inbox_[pos] = message;
}

String GatewayClient::readMessageString(JsonObjectConst payload,
                                        const char *key1,
                                        const char *key2,
                                        const char *key3) const {
  auto readOne = [&](const char *key) -> String {
    if (!key || !payload.containsKey(key)) {
      return "";
    }

    JsonVariantConst value = payload[key];
    if (value.is<const char *>()) {
      return String(value.as<const char *>());
    }
    if (value.is<String>()) {
      return value.as<String>();
    }
    if (value.is<bool>() || value.is<int>() || value.is<long>() ||
        value.is<unsigned long>() || value.is<long long>() ||
        value.is<unsigned long long>() || value.is<float>() || value.is<double>()) {
      return value.as<String>();
    }
    if (value.is<JsonObjectConst>()) {
      JsonObjectConst nested = value.as<JsonObjectConst>();
      const String nestedId = String(static_cast<const char *>(nested["id"] | ""));
      if (!nestedId.isEmpty()) {
        return nestedId;
      }
      return String(static_cast<const char *>(nested["name"] | ""));
    }
    return "";
  };

  const String first = readOne(key1);
  if (!first.isEmpty()) {
    return first;
  }

  const String second = readOne(key2);
  if (!second.isEmpty()) {
    return second;
  }

  return readOne(key3);
}
