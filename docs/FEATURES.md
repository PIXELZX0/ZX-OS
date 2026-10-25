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
- Adjust display brightness percentage.
- Set timezone string manually.
- Sync timezone by network/IP (requires Wi-Fi).
- Factory reset runtime config.

#### Firmware Update

- Show firmware update status.
- Check latest firmware metadata.
- Download latest firmware package to SD (`/firmware/latest.bin`).
- Install downloaded package.
- Update now (network-driven flow, then reboot).

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


## 7. JS scripting framework (`src/core/js/*`, `JS Console` app)

ZX-OS embeds [Duktape](https://duktape.org/) (ES5.1 + typed arrays, MIT
licensed, vendored under `lib/duktape/`) so IR, NFC and CC1101 can be driven
from plain JavaScript instead of recompiling firmware.

- **Where scripts live**: `/scripts/*.js` on the SD card. The `JS Console`
  launcher entry lists them, runs the selected one, and shows `print()`
  output plus the final value or error. A `sample.js` is created on first
  use.
- **Remote execution**: the OpenClaw gateway accepts `js.eval` (inline
  `source`) and `js.run_file` (SD `path`), both bounded by `timeoutMs`
  (≤30s) — see `NodeCommandHandler::handleJsCommand`.
- **Safety**: one script runs at a time; each run gets a wall-clock timeout
  (aborted via Duktape's interrupt-counter hook, so even `while(true){}`
  is killed), a heap allocator capped to a slice of PSRAM/internal RAM, and
  BACK cancels a script running from the launcher. `fs.*` is sandboxed to
  absolute paths with no `..`; `gpio.*` refuses pins the firmware itself
  depends on (display/SD/CC1101 SPI, encoder, buttons, I2C, IR, NFC/RFID/
  NRF24).

### Global API (see `src/core/js/js_mod_*.cpp` for the native side)

- `print(...)`, `console.log/info/warn/error`, `delay(ms)`, `millis()`,
  `random()`, `toHex(bytes, sep)`, `fromHex(str)`.
- `sys.board()`, `sys.heapFree()`, `sys.uptimeMs()`.
- `gpio.mode(pin, "input"|"input_pullup"|"input_pulldown"|"output")`,
  `gpio.write(pin, 0|1)`, `gpio.read(pin)`, `gpio.analogRead(pin)`,
  `gpio.pwm(pin, duty0to1, hz)`.
- `fs.readText(path)`, `fs.readBytes(path)`, `fs.write(path, data, append)`,
  `fs.remove/mkdir/exists/list(path)`.
- `ui.toast(title, msg, ms)`, `ui.confirm(title, msg)`, `ui.prompt(title,
  default)`, `ui.progress(title, msg, percent)`, `ui.hideProgress()`,
  `ui.status(line)` (only available when a `UiRuntime` is attached, i.e.
  when run from the launcher).
- `ir.send(protocol, address, command, repeat)` — `protocol` is one of
  `"NEC"`, `"NECext"`, `"Samsung32"`, `"SIRC"`/`"SIRC15"`/`"SIRC20"`, `"RC5"`.
  `ir.sendRaw(timings, carrierHz)`, `ir.receive(timeoutMs)` (returns
  `{raw, protocol, address, command, toggle}` or `null`),
  `ir.decode(timings)`, `ir.encode(protocol, address, command)`.
- `nfc.scan(timeoutMs)` → `{uid, uidHex}` or `null`; `nfc.info()`;
  `nfc.mifare.auth(uid, block, key6, useKeyB)`, `.read(block)`,
  `.write(block, data16)`, `.findKey(block, keys, useKeyB)` (dictionary
  attack helper); `nfc.ntag.readPage(page)` / `.writePage(page, data4)`;
  `nfc.transceive(apdu)` for raw `InDataExchange`.
- `cc1101.info()`, `.setFreq(mhz)`, `.setPower(dbm)`, `.readRssi()`,
  `.readReg(addr)` / `.writeReg(addr, value)` / `.strobe(strobe)` /
  `.setMode("idle"|"rx"|"tx"|"sleep"|"reset")` for direct register access,
  `.capture(timeoutMs, minEdges, gapUs)` / `.replay(timings, repeat, gapUs)`
  for raw OOK/ASK pulse trains (sub-GHz remotes), `.txFixedCode(code, bits,
  pulseLength, protocol, repeat)` for rc-switch style fixed codes, and
  `.packetGet()` / `.packetSet(cfg)` / `.send(bytes)` / `.receive(timeoutMs)`
  for the existing FSK/GFSK packet mode.
