#pragma once

// Build identity injected by scripts/firmware_version.py. The fallbacks only
// apply to builds that bypass PlatformIO's extra script.
#ifndef ZX_FW_VERSION
#define ZX_FW_VERSION "dev"
#endif
#ifndef ZX_BUILD_ENV
#define ZX_BUILD_ENV "t-embed-cc1101"
#endif
#ifndef ZX_FW_REPO
#define ZX_FW_REPO "PIXELZX0/ZX-OS"
#endif

// Release assets for this board are named "openclaw-<env>-<tag|latest>.bin".
#define ZX_FW_ASSET_PREFIX "openclaw-" ZX_BUILD_ENV "-"
