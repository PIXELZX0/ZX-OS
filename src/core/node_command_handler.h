#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class GatewayClient;

class NodeCommandHandler {
 public:
  void setGatewayClient(GatewayClient *gateway);

  void handleInvoke(const String &invokeId,
                    const String &nodeId,
                    const String &command,
                    JsonObjectConst params);

 private:
  GatewayClient *gateway_ = nullptr;

  bool handleSystemWhich(const String &invokeId,
                         const String &nodeId,
                         JsonObjectConst params);

  bool handleSystemRun(const String &invokeId,
                       const String &nodeId,
                       JsonObjectConst params);

  bool handleCc1101Command(const String &invokeId,
                           const String &nodeId,
                           const String &command,
                           JsonObjectConst params);

  bool handleJsCommand(const String &invokeId,
                       const String &nodeId,
                       const String &command,
                       JsonObjectConst params);
};
