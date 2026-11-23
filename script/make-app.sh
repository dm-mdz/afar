#!/usr/bin/env bash
# Afar_C macOS Afar.app / DMG packager, codesigner, and notarizer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

OUT_DIR="${ROOT_DIR}/build/dist"
BIN_PATH=""
VERSION=""
SIGN_IDENTITY=""
NOTARIZE_PROFILE=""
NO_DMG=0

usage() {
  cat <<'EOF'
Usage: script/make-app.sh [options]

Package Afar.app (and optionally Afar-<version>.dmg) for macOS.

Options:
  --out-dir <dir>              Output directory (default: <repo>/build/dist)
  --bin <path>                 Prebuilt afar-agent binary (default: builds release via build.py)
  --version <ver>              Bundle version (default: extracted from src/af_modules.h AF_VERSION)
  --sign <identity>            Developer ID / signing identity for codesign (default: ad-hoc '-')
  --notarize <keychain-profile>
                               Submit DMG to notarytool and staple (requires --sign, without --no-dmg)
  --no-dmg                     Only build Afar.app, skip DMG creation
  -h, --help                   Show this help message
EOF
}

say() {
  printf '[make-app] %s\n' "$*"
}

die() {
  printf '[make-app] ERROR: %s\n' "$*" >&2
  exit 1
}

require_arg() {
  if [[ $# -lt 2 || -z "${2}" ]]; then
    die "option '$1' requires a non-empty argument"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir)
      require_arg "$@"
      OUT_DIR="$2"
      shift 2
      ;;
    --bin)
      require_arg "$@"
      BIN_PATH="$2"
      shift 2
      ;;
    --version)
      require_arg "$@"
      VERSION="$2"
      shift 2
      ;;
    --sign)
      require_arg "$@"
      SIGN_IDENTITY="$2"
      shift 2
      ;;
    --notarize)
      require_arg "$@"
      NOTARIZE_PROFILE="$2"
      shift 2
      ;;
    --no-dmg)
      NO_DMG=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1 (use --help for usage)"
      ;;
  esac
done

if [[ -n "${NOTARIZE_PROFILE}" ]]; then
  if [[ -z "${SIGN_IDENTITY}" || "${SIGN_IDENTITY}" == "-" ]]; then
    die "--notarize requires a real --sign <identity> (not ad-hoc '-')"
  fi
  if [[ "${NO_DMG}" -eq 1 ]]; then
    die "--notarize cannot be combined with --no-dmg"
  fi
fi

# 1. Resolve version from src/af_modules.h if not explicitly provided
if [[ -z "${VERSION}" ]]; then
  MOD_HDR="${ROOT_DIR}/src/af_modules.h"
  [[ -f "${MOD_HDR}" ]] || die "missing ${MOD_HDR}"
  VERSION="$(sed -nE 's/^[[:space:]]*#[[:space:]]*define[[:space:]]+AF_VERSION[[:space:]]+"([^"]+)".*/\1/p' "${MOD_HDR}" | head -n 1)"
  [[ -n "${VERSION}" ]] || die "failed to extract AF_VERSION from ${MOD_HDR}"
fi
if [[ ! "${VERSION}" =~ ^[0-9A-Za-z._+-]+$ || "${VERSION}" == *".."* ]]; then
  die "invalid --version '${VERSION}' (allowed: [0-9A-Za-z._+-], no '..')"
fi

# 2. Resolve or build afar-agent binary
if [[ -n "${BIN_PATH}" ]]; then
  if [[ ! -f "${BIN_PATH}" ]]; then
    die "specified binary not found: ${BIN_PATH}"
  fi
  BIN_PATH="$(cd "$(dirname "${BIN_PATH}")" && pwd)/$(basename "${BIN_PATH}")"
else
  say "building release binary via build.py release..."
  python3 "${ROOT_DIR}/build.py" release
  BIN_PATH="${ROOT_DIR}/build/darwin-release/bin/afar-agent"
  [[ -f "${BIN_PATH}" ]] || die "expected release binary not found: ${BIN_PATH}"
fi

PLIST_TMPL="${ROOT_DIR}/shell/Info.mac.plist"
SETUP_HTML_SRC="${ROOT_DIR}/shell/ui/setup.html"
APP_ICON_SRC="${ROOT_DIR}/shell/AppIcon.icns"
ENTITLEMENTS_SRC="${ROOT_DIR}/shell/Afar.entitlements"
[[ -f "${PLIST_TMPL}" ]] || die "missing Info.plist template: ${PLIST_TMPL}"
[[ -f "${SETUP_HTML_SRC}" ]] || die "missing setup.html: ${SETUP_HTML_SRC}"
[[ -f "${APP_ICON_SRC}" ]] || die "missing AppIcon.icns: ${APP_ICON_SRC}"
[[ -f "${ENTITLEMENTS_SRC}" ]] || die "missing Afar.entitlements: ${ENTITLEMENTS_SRC}"

# 2b. Verify no local machine paths leaked into the shipped binary
for p in "${HOME}" "${ROOT_DIR}"; do
  if [[ -n "${p}" ]] && strings -a "${BIN_PATH}" | grep -qF "${p}"; then
    die "local path '${p}' found embedded in ${BIN_PATH}; rebuild cleanly with -ffile-prefix-map"
  fi
done

mkdir -p "${OUT_DIR}"
OUT_DIR="$(cd "${OUT_DIR}" && pwd)"

APP_DIR="${OUT_DIR}/Afar.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RES_DIR="${CONTENTS_DIR}/Resources"
INFO_PLIST="${CONTENTS_DIR}/Info.plist"
APP_BIN="${MACOS_DIR}/afar-agent"
APP_SETUP_HTML="${RES_DIR}/setup.html"
APP_ICON_DST="${RES_DIR}/AppIcon.icns"

# 3. Assemble Afar.app bundle
say "assembling ${APP_DIR} (version ${VERSION})..."
rm -rf "${APP_DIR}"
mkdir -p "${MACOS_DIR}" "${RES_DIR}"

cp "${BIN_PATH}" "${APP_BIN}"
chmod 0755 "${APP_BIN}"

cp "${SETUP_HTML_SRC}" "${APP_SETUP_HTML}"
chmod 0644 "${APP_SETUP_HTML}"

cp "${APP_ICON_SRC}" "${APP_ICON_DST}"
chmod 0644 "${APP_ICON_DST}"

cp "${PLIST_TMPL}" "${INFO_PLIST}"
chmod 0644 "${INFO_PLIST}"

# 4. Sync versions via PlistBuddy and enforce LSMinimumSystemVersion == 27.0
PLIST_BUDDY="/usr/libexec/PlistBuddy"
[[ -x "${PLIST_BUDDY}" ]] || die "PlistBuddy not found at ${PLIST_BUDDY}"

"${PLIST_BUDDY}" -c "Set :CFBundleShortVersionString ${VERSION}" "${INFO_PLIST}"
"${PLIST_BUDDY}" -c "Set :CFBundleVersion ${VERSION}" "${INFO_PLIST}"

MIN_SYS_VER="$("${PLIST_BUDDY}" -c "Print :LSMinimumSystemVersion" "${INFO_PLIST}")"
if [[ "${MIN_SYS_VER}" != "27.0" ]]; then
  die "LSMinimumSystemVersion must be 27.0, got '${MIN_SYS_VER}'"
fi

SHORT_VER="$("${PLIST_BUDDY}" -c "Print :CFBundleShortVersionString" "${INFO_PLIST}")"
BUNDLE_VER="$("${PLIST_BUDDY}" -c "Print :CFBundleVersion" "${INFO_PLIST}")"
if [[ "${SHORT_VER}" != "${VERSION}" || "${BUNDLE_VER}" != "${VERSION}" ]]; then
  die "version sync failed: CFBundleShortVersionString='${SHORT_VER}', CFBundleVersion='${BUNDLE_VER}'"
fi

# 5. Validate Info.plist and setup.html
say "validating Info.plist and resources..."
plutil -lint "${INFO_PLIST}" >/dev/null
if ! cmp -s "${SETUP_HTML_SRC}" "${APP_SETUP_HTML}"; then
  die "bundled setup.html does not match ${SETUP_HTML_SRC}"
fi
if ! cmp -s "${APP_ICON_SRC}" "${APP_ICON_DST}"; then
  die "bundled AppIcon.icns does not match ${APP_ICON_SRC}"
fi

# 6. Codesign Afar.app (Developer ID + Hardened Runtime + Apple Events entitlement, or ad-hoc)
if [[ -n "${SIGN_IDENTITY}" && "${SIGN_IDENTITY}" != "-" ]]; then
  say "signing Afar.app with identity '${SIGN_IDENTITY}'..."
  codesign --force --options runtime --timestamp --deep --entitlements "${ENTITLEMENTS_SRC}" --sign "${SIGN_IDENTITY}" "${APP_DIR}"
  codesign --verify --deep --strict --verbose=2 "${APP_DIR}"
else
  say "ad-hoc signing Afar.app..."
  codesign --force --sign - "${APP_DIR}"
  codesign --verify --deep --strict "${APP_DIR}"
fi

# 7. Verify bundled binary runs cleanly
BIN_VER_OUT="$("${APP_BIN}" --version)"
[[ -n "${BIN_VER_OUT}" ]] || die "bundled binary --version produced empty output"
say "bundled binary check: ${BIN_VER_OUT}"

# 8. Build drag-install DMG unless --no-dmg
DMG_PATH=""
if [[ "${NO_DMG}" -eq 0 ]]; then
  DMG_PATH="${OUT_DIR}/Afar-${VERSION}.dmg"
  STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/afar-dmg-stage.XXXXXX")"
  cleanup_stage() {
    rm -rf "${STAGE_DIR}"
  }
  trap cleanup_stage EXIT

  say "staging DMG contents..."
  ditto "${APP_DIR}" "${STAGE_DIR}/Afar.app"
  ln -s /Applications "${STAGE_DIR}/Applications"

  rm -f "${DMG_PATH}"
  say "creating ${DMG_PATH}..."
  hdiutil create \
    -volname "Afar" \
    -srcfolder "${STAGE_DIR}" \
    -ov \
    -format UDZO \
    "${DMG_PATH}" >/dev/null

  rm -rf "${STAGE_DIR}"
  trap - EXIT

  if [[ -n "${SIGN_IDENTITY}" && "${SIGN_IDENTITY}" != "-" ]]; then
    say "signing DMG with identity '${SIGN_IDENTITY}'..."
    codesign --force --timestamp --sign "${SIGN_IDENTITY}" "${DMG_PATH}"
    codesign --verify --verbose=2 "${DMG_PATH}"
  fi
fi

# 9. Optional notarization & stapling
if [[ -n "${NOTARIZE_PROFILE}" ]]; then
  say "submitting ${DMG_PATH} for notarization (profile: ${NOTARIZE_PROFILE})..."
  NOTARY_LOG="${OUT_DIR}/notary.log"
  xcrun notarytool submit "${DMG_PATH}" --keychain-profile "${NOTARIZE_PROFILE}" --wait | tee "${NOTARY_LOG}"
  grep -q "status: Accepted" "${NOTARY_LOG}" || die "notarization did not succeed (see ${NOTARY_LOG})"
  say "stapling notarization ticket..."
  xcrun stapler staple "${DMG_PATH}"
  xcrun stapler validate "${DMG_PATH}"
  spctl -a -t open --context context:primary-signature -v "${DMG_PATH}"
fi

say "packaging PASSED"
say "  App:     ${APP_DIR}"
if [[ -n "${DMG_PATH}" ]]; then
  say "  DMG:     ${DMG_PATH}"
fi
say "  Version: ${VERSION} (LSMinimumSystemVersion=${MIN_SYS_VER})"
