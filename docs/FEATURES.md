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
- Install/remove the Korean font pack. The font is not built into the firmware:
  it lives on the SD card at `/fonts/korean_ui_14.zxf` and is read glyph by
  glyph at runtime. Install downloads `zx-font-korean-ui-14.zxf` from the latest
  release (SHA-256 verified), or uses a copy already placed on the SD card.
  The pack is generated from an `lv_font_conv` C font with
  `scripts/lvgl_font_to_zxf.py`; the committed source is `assets/fonts/`.
- Adjust display brightness percentage.
- Set timezone string manually.
- Sync timezone by network/IP (requires Wi-Fi).
- Factory reset runtime config.

#### Firmware Update

- Show firmware update status (running version, board env, OTA slot size, SD package).
- Check for update: compares the running version with the latest GitHub release
  and offers to install when a newer one exists.
- Update now (Wi-Fi): streams this board's image (`openclaw-<env>-<tag>.bin`)
  straight into the OTA partition, so no SD card is required.
- Download to SD (`/firmware/latest.bin`) and install from SD (SD boards only).
- Every image is checked against the release's `.sha256` asset; the new firmware
  is only committed when the hash matches, otherwise the current one keeps booting.
- The running version and target repository are injected at build time by
  `scripts/firmware_version.py` (`ZX_FW_VERSION`, `ZX_FW_REPO` env vars; CI sets them).

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
- **RF app** (`rf_app.cpp`)
  - CC1101 radio info.
  - Frequency set.
  - Packet profile tuning.
  - Packet TX/RX.
  - RSSI read.
  - OOK TX via RCSwitch-style signaling.
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

