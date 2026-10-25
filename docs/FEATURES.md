# ZX-OS Feature Reference

This document explains what ZX-OS can do today and where each capability lives in code.
It is intended to help firmware/app developers quickly discover reusable features.

## 1. System overview

ZX-OS is structured as a service-driven runtime plus app modules:

- **Boot/runtime orchestration** (`src/main.cpp`)
  - Initializes power rails, UI, radio, storage-backed config, Wi-Fi, gateway, BLE.
  - Runs background ticks for networking, UI, RAM watchdog, and deep-sleep button handling.
- **Core services** (`src/core/*`)
  - Runtime configuration load/save/reset and validation.
  - Gateway WebSocket client + command handling.
  - Wi-Fi and BLE managers.
  - CC1101 radio control and shared SPI bus handling.
- **UI platform** (`src/ui/*`)
  - LVGL runtime setup and rendering.
  - Encoder/button input adaptation.
  - App launcher navigation and i18n.
- **Apps** (`src/apps/*`)
  - Launcher-exposed operational apps.
  - Hardware module apps and OpenClaw workflow apps.

## 2. Launcher-visible user apps

Current launcher menu entries are:

1. **APPMarket**
2. **Settings**
3. **File Explorer**
4. **RF** (CC1101 sub-GHz)
5. **IR** (Flipper-style infrared)
6. **NFC**
7. **RFID**

These are wired by `UiNavigator::runLauncher(...)`.

### 2.1 APPMarket

APPMarket provides GitHub release based package management and local fallback workflows:

- Show status (network, repo, latest/backup package paths, dirty config).
- Configure GitHub repository slug (`owner/repo`).
- Configure release asset preference (`.bin` target name filter).
- Check latest release metadata.
- Browse release assets interactively.
- Download latest/selected package to SD (`/appmarket/latest.bin`).
- Install latest package from SD (reboot after install).
- Backup currently running app to SD (`/appmarket/backup.bin`).
- Reinstall from backup package.
- Install from any SD `.bin` via file picker.
- Delete latest/backup packages.
- Save APPMarket config changes.

### 2.2 Settings

Settings is split into four major sections.

#### Wi-Fi

- Scan nearby SSIDs.
- Connect to scanned network.
- Connect to hidden SSID.
- Connect now using saved credentials.
- Disconnect.
- Clear stored Wi-Fi credentials.

#### BLE

- Scan and connect BLE device.
- Connect to saved BLE device.
- Disconnect current BLE session.
- Keyboard input view (debug/inspection style utility).
- Clear captured BLE keyboard input.
- Edit saved BLE device address.
- Toggle BLE auto-connect.
- Forget saved BLE device.

#### System

- Edit device name (with validation).
- Switch UI language (English/Korean).
- Adjust display brightness percentage.
- Set timezone string manually.
- Sync timezone by network/IP (requires Wi-Fi).
- Factory reset runtime config.

#### Firmware Update (OTA)

- Show firmware update status, including the **current running version** (`ZX_OS_VERSION`), active GitHub repo, and the board-matched asset.
- Check latest firmware metadata with **up-to-date detection** (compares the running version tag against the latest release tag).
- Download latest firmware package to SD (`/firmware/latest.bin`).
- **SHA-256 integrity verification**: when the release body publishes a checksum for the asset (`<!-- SHA256:<asset>=<hex> -->`), the download is hashed and rejected on mismatch before it can ever be flashed.
- Install downloaded package.
- Update now (network-driven flow, then reboot).

OTA source resolution:

- The GitHub repository comes from runtime config (`appMarketGithubRepo`), falling back to the compiled default.
- The release asset is selected automatically from the per-board build define `ZX_OS_BOARD_ASSET` (e.g. `openclaw-t-embed-cc1101-latest.bin`), so each board pulls its own binary. If that is empty it falls back to `appMarketReleaseAsset`, then to the first `.bin` asset.
- The release pipeline injects the real version via `-DZX_OS_VERSION` (release tag / commit) through `PLATFORMIO_BUILD_FLAGS`; local builds show `dev`.

### 2.3 File Explorer

SD-centered utility app with interactive browsing:

- SD card info (mount/space/health style metadata).
- Browse directories/files.
- File info view.
- File text preview.
- Image viewer path.
- Audio playback path.
- SD quick format.
- SD remount.

## 3. Additional module apps available in source

These are implemented and reusable, even if not currently launcher-wired in this branch:

- **OpenClaw app** (`openclaw_app.cpp`)
  - Gateway status dashboard (Wi-Fi, gateway, auth mode, BLE state, CC1101 status).
  - Gateway config editor (URL, auth mode, credentials, clear config).
  - Messenger flows:
    - text send,
    - voice record/send,
    - file attachment send,
    - session subscribe/unsubscribe and new session initialization.
  - Save & apply runtime config (Wi-Fi/Gateway/BLE reconfigure + reconnect logic).
- **RF app** (`rf_app.cpp`) — launcher-wired
  - CC1101 radio info.
  - Frequency set.
  - Packet profile tuning.
  - Packet TX/RX.
  - RSSI read.
  - OOK TX via RCSwitch-style signaling.
- **IR app** (`ir_app.cpp`) — launcher-wired, Flipper-style infrared
  - Module info (TX/RX pins, carrier, last captured signal).
  - Universal remote: multi-brand TV Power / Volume± / Mute batches (NEC/SIRC/Samsung/Panasonic presets sent in sequence).
  - Learn Signal: capture a decoded protocol (NEC, Sony, Samsung, …) or a raw timing frame from the IR receiver.
  - Replay Last: re-transmit the captured signal (protocol value or raw buffer).
  - Driver: `IRremoteESP8266`; pins `HAL_PIN_IR_TX` / `HAL_PIN_IR_RX` (optional external wiring, overridable via `USER_IR_TX_PIN` / `USER_IR_RX_PIN`). Gracefully degrades when the library or pins are absent.
- **NFC app** (`nfc_app.cpp`)
  - Module info and tag UID scanning.
- **RFID app** (`rfid_app.cpp`)
  - Module info and MIFARE UID scanning.
- **NRF24 app** (`nrf24_app.cpp`)
  - Module info.
  - Channel/rate/power/payload configuration.
  - Text send (32-byte packet constraints).
  - Single receive with timeout.

## 4. Runtime/platform capabilities

### 4.1 Power and sleep

- Rail enable + TFT backlight setup at boot.
- Long-press top button enters deep sleep.
- Wakeup source configured via EXT0 (top button).

### 4.2 Memory safety behavior

- Periodic RAM usage watchdog.
- Reboot fallback on extreme memory pressure to recover system health.
- Optional periodic memory trace logs (build-time flag controlled).

### 4.3 Connectivity and telemetry

- Wi-Fi manager tick-based lifecycle.
- Gateway client tick-based lifecycle with reconnect helpers.
- BLE manager tick-based lifecycle.
- Telemetry payload includes network and CC1101 status fields.

### 4.4 i18n

- UI language enum: English + Korean.
- Runtime language comes from config (`uiLanguage`), then mapped by helper.

## 5. Configuration and persistence model

`RuntimeConfig` supports:

- Device identity: name, gateway device identifiers/keys/tokens.
- Wi-Fi credentials.
- Gateway URL and auth mode (token/password).
- BLE target address and auto-connect toggle.
- APPMarket repo + asset preference.
- UI language.
- Timezone string.
- Display brightness percentage.

Storage/load behavior:

- Preferred SD config file (`/oc_cfg.json`) when available.
- NVS fallback persistence for reliability.
- Optional SD `.env` overrides for gateway fields.
- Validation gates before applying sensitive runtime changes.

## 6. Developer notes for faster app work

- Use `AppContext` as the shared dependency carrier (services + mutable config + dirty flag).
- Prefer `ctx.uiRuntime` helper loops (`menuLoop`, `showInfo`, `showToast`, input dialogs) to keep UX consistent.
- Use `backgroundTick` inside long operations to keep networking/UI responsive.
- Mark configuration edits with `ctx.configDirty = true` and persist via save/apply flows.
- For SPI peripherals, follow shared bus/CS discipline to avoid contention.

