# ZX-OS (T-Embed CC1101)

ZX-OS is embedded firmware for the LilyGO **T-Embed CC1101** board.
It combines a launcher-based LVGL UI with wireless tooling, OpenClaw gateway integration, SD-card workflows, and OTA-style firmware/app package management.

## What this project provides

- A touchless launcher UX optimized for the T-Embed encoder + buttons.
- Configuration persistence across reboot (SD + NVS fallback).
- OpenClaw gateway connectivity (`ws://` and `wss://`) with token/password auth.
- Wireless modules and utility apps (RF/CC1101, NFC, RFID, NRF24, BLE).
- A JavaScript (Duktape/ES5.1) scripting framework (`ir`/`nfc`/`cc1101`/`gpio`/`fs`/`ui`) for scripting IR, NFC and sub-GHz without recompiling — see `docs/FEATURES.md` §7.
- SD-card utilities including browsing, previewing media, and package-based updates.

> For full feature details and developer workflows, read:
>
> - `docs/FEATURES.md`
> - `docs/APP_DEVELOPMENT_GUIDE.md`

Third-party: the JS framework vendors [Duktape](https://duktape.org/) (MIT) under `lib/duktape/`.

## Web Flusher

브라우저에서 바로 릴리즈된 ZX-OS 펌웨어를 장치에 플래시할 수 있습니다.

**→ https://hiteyy.github.io/ZX-OS/**

- **요구사항**: Chrome / Edge (데스크톱) — Web Serial API 지원 브라우저
- 별도 설치 없이 GitHub 릴리즈 목록에서 버전을 선택하고 USB로 연결된 장치에 바로 플래시합니다.
- `main`/`master` 브랜치에 `web-flusher/` 변경이 푸시되면 GitHub Actions가 자동 배포합니다.

로컬에서 미리 보기:

```bash
python3 -m http.server --directory web-flusher
# → http://localhost:8000
```

## Quick start

### 1) Build

```bash
pio run -e t-embed-cc1101
```

### 2) Upload

```bash
pio run -e t-embed-cc1101 -t upload
```

### 3) Serial monitor

```bash
pio device monitor -b 115200
```

## Hardware/software stack

- **MCU/Board**: ESP32-S3 (LilyGO T-Embed CC1101)
- **Framework**: Arduino (PlatformIO)
- **UI**: LVGL + TFT_eSPI
- **Connectivity**: Wi-Fi, BLE, WebSocket gateway
- **Storage**: NVS + SD card

## Project structure

- `src/main.cpp`: boot, lifecycle, sleep/watchdog handling, service wiring.
- `src/core/*`: configuration, gateway, Wi-Fi, BLE, radio abstraction, shared buses.
- `src/ui/*`: LVGL runtime, input adapter, i18n, launcher/navigation.
- `src/apps/*`: app implementations (launcher apps + module apps).
- `docs/*`: architecture and feature documentation for faster app development.

## Documentation map

- **Feature reference**: `docs/FEATURES.md`
- **Developer onboarding / app extension guide**: `docs/APP_DEVELOPMENT_GUIDE.md`

## License

MIT License. See `LICENSE`.
