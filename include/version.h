#pragma once

// ============================================================================
// ZX-OS firmware version
// ============================================================================
// The release pipeline injects the real version by passing
//   -DZX_OS_VERSION=\"vX.Y.Z\"
// through PLATFORMIO_BUILD_FLAGS (see .github/workflows/*.yml). Local builds
// that do not set it fall back to "dev" so the OTA screen always has
// something to show.
// ============================================================================

#ifndef ZX_OS_VERSION
#define ZX_OS_VERSION "dev"
#endif

// Board-matched release asset name (e.g. openclaw-t-embed-cc1101-latest.bin).
// Defined per environment in platformio.ini so the OTA app can automatically
// select the binary that matches the board it is running on.
#ifndef ZX_OS_BOARD_ASSET
#define ZX_OS_BOARD_ASSET ""
#endif
