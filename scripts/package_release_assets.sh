#!/usr/bin/env bash
set -euo pipefail

: "${PIO_ENV:?PIO_ENV is required}"
: "${VERSION_TAG:?VERSION_TAG is required}"

DIST_DIR="${DIST_DIR:-dist}"
FIRMWARE_BIN_PATH="${FIRMWARE_BIN_PATH:-.pio/build/${PIO_ENV}/firmware.bin}"
REPO_SLUG="${REPO_SLUG:-${GITHUB_REPOSITORY:-unknown/unknown}}"
INCLUDE_LATEST_ALIAS="${INCLUDE_LATEST_ALIAS:-false}"
INCLUDE_APP_BUNDLE="${INCLUDE_APP_BUNDLE:-true}"
INCLUDE_FONT_PACKS="${INCLUDE_FONT_PACKS:-true}"

sha256_file() {
  local target="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${target}"
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${target}"
    return 0
  fi
  echo "No sha256 tool found (sha256sum/shasum)." >&2
  return 1
}

if [[ ! -f "${FIRMWARE_BIN_PATH}" ]]; then
  echo "Firmware binary not found: ${FIRMWARE_BIN_PATH}"
  exit 1
fi

if [[ ! "${VERSION_TAG}" =~ ^[0-9A-Za-z._-]+$ ]]; then
  echo "VERSION_TAG contains unsupported characters: ${VERSION_TAG}"
  exit 1
fi

mkdir -p "${DIST_DIR}"

VERSIONED_BIN="openclaw-${PIO_ENV}-${VERSION_TAG}.bin"
VERSIONED_BIN_PATH="${DIST_DIR}/${VERSIONED_BIN}"
cp "${FIRMWARE_BIN_PATH}" "${VERSIONED_BIN_PATH}"
sha256_file "${VERSIONED_BIN_PATH}" > "${VERSIONED_BIN_PATH}.sha256"

if [[ "${INCLUDE_LATEST_ALIAS}" == "true" ]]; then
  LATEST_BIN="openclaw-${PIO_ENV}-latest.bin"
  LATEST_BIN_PATH="${DIST_DIR}/${LATEST_BIN}"
  cp "${FIRMWARE_BIN_PATH}" "${LATEST_BIN_PATH}"
  sha256_file "${LATEST_BIN_PATH}" > "${LATEST_BIN_PATH}.sha256"
fi

if [[ "${INCLUDE_APP_BUNDLE}" == "true" ]]; then
  APP_MANIFEST="openclaw-app-${PIO_ENV}-${VERSION_TAG}.json"
  APP_BUNDLE="openclaw-app-${PIO_ENV}-${VERSION_TAG}.zip"
  APP_MANIFEST_PATH="${DIST_DIR}/${APP_MANIFEST}"
  APP_BUNDLE_PATH="${DIST_DIR}/${APP_BUNDLE}"

  VERSIONED_SHA256="$(awk '{print $1}' "${VERSIONED_BIN_PATH}.sha256")"
  VERSIONED_SIZE_BYTES="$(wc -c < "${VERSIONED_BIN_PATH}")"
  CREATED_AT_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  cat > "${APP_MANIFEST_PATH}" <<EOF
{
  "schemaVersion": 1,
  "name": "OpenClaw Firmware Package",
  "releaseTag": "${VERSION_TAG}",
  "buildEnv": "${PIO_ENV}",
  "repository": "${REPO_SLUG}",
  "firmware": {
    "asset": "${VERSIONED_BIN}",
    "sha256": "${VERSIONED_SHA256}",
    "sizeBytes": ${VERSIONED_SIZE_BYTES}
  },
  "createdAtUtc": "${CREATED_AT_UTC}"
}
EOF

  APP_TMP_DIR="$(mktemp -d)"
  cp "${VERSIONED_BIN_PATH}" "${APP_TMP_DIR}/firmware.bin"
  cp "${VERSIONED_BIN_PATH}.sha256" "${APP_TMP_DIR}/firmware.bin.sha256"
  cp "${APP_MANIFEST_PATH}" "${APP_TMP_DIR}/manifest.json"
  (
    cd "${APP_TMP_DIR}"
    zip -qr "${PWD}/${APP_BUNDLE}" .
  )
  mv "${APP_TMP_DIR}/${APP_BUNDLE}" "${APP_BUNDLE_PATH}"
  rm -rf "${APP_TMP_DIR}"
  sha256_file "${APP_BUNDLE_PATH}" > "${APP_BUNDLE_PATH}.sha256"
fi

if [[ "${INCLUDE_FONT_PACKS}" == "true" ]]; then
  # Board-independent SD font packs, installed on device from the latest release.
  FONT_PACK_PATH="${DIST_DIR}/zx-font-korean-ui-14.zxf"
  cp assets/fonts/korean_ui_14.zxf "${FONT_PACK_PATH}"
  sha256_file "${FONT_PACK_PATH}" > "${FONT_PACK_PATH}.sha256"
fi

echo "Packaged assets in ${DIST_DIR}:"
find "${DIST_DIR}" -maxdepth 1 -type f -print | sort
