#pragma once

#include <Arduino.h>

#include "../core/runtime_config.h"

class WifiManager;
class GatewayClient;
class BleManager;
class UiRuntime;
class UiNavigator;

enum class AppId : uint8_t {
  OpenClaw = 0,
  Settings = 1,
  FileExplorer = 2,
  AppMarket = 3,
  Rf = 4,
  Nfc = 5,
  Rfid = 6,
  Nrf24 = 7,
  Ir = 8,
};

struct AppContext {
  RuntimeConfig config;
  WifiManager *wifi = nullptr;
  GatewayClient *gateway = nullptr;
  BleManager *ble = nullptr;
  UiRuntime *uiRuntime = nullptr;
  UiNavigator *uiNav = nullptr;
  bool configDirty = false;
};
