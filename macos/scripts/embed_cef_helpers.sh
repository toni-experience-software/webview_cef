#!/usr/bin/env bash
#
# Embed the CEF helper sub-process apps into the host application bundle, so CEF
# runs multi-process on macOS. Invoked as a build phase of the host app's Runner
# target (installed via the Podfile post_install hook in macos/embed_cef_helpers.rb).
#
# It clones the prebuilt helper executable (macos/third/cef/cef_helper, produced
# by download_cef.sh) into the five CEF-named ".app" bundles inside the host
# app's Frameworks dir, writes a minimal Info.plist for each, and code-signs them.
#
# If the prebuilt helper is missing, it does nothing — the plugin then falls back
# to single-process at runtime, so the build still succeeds.
set -euo pipefail

PLUGIN_MACOS="${WEBVIEW_CEF_MACOS_DIR:-${SRCROOT}/Flutter/ephemeral/.symlinks/plugins/webview_cef/macos}"
HELPER_BIN="${PLUGIN_MACOS}/third/cef/cef_helper"
ENT="${PLUGIN_MACOS}/helper/Helper.entitlements"

if [ ! -f "${HELPER_BIN}" ]; then
  echo "warning: webview_cef helper not found at ${HELPER_BIN}; skipping embed (CEF will run single-process)."
  exit 0
fi

BASE="${EXECUTABLE_NAME} Helper"
DEST="${TARGET_BUILD_DIR}/${FRAMEWORKS_FOLDER_PATH}"
IDENTITY="${EXPANDED_CODE_SIGN_IDENTITY:--}"
mkdir -p "${DEST}"

# The helpers are nested executables the notary service checks on their own, and
# they are not Xcode targets, so the host app's ENABLE_HARDENED_RUNTIME never
# reaches them: enable the hardened runtime and request a secure timestamp here,
# or notarization rejects every one of them. An ad-hoc signature ("-", what a
# local debug build gets) can carry neither, so it keeps the old flags.
SIGN_FLAGS=(--force --sign "${IDENTITY}")
if [ "${IDENTITY}" = "-" ]; then
  SIGN_FLAGS+=(--timestamp=none)
else
  SIGN_FLAGS+=(--options runtime --timestamp)
fi
if [ -f "${ENT}" ]; then
  SIGN_FLAGS+=(--entitlements "${ENT}")
fi

# "<name suffix>:<bundle-id suffix>" — see CEF_HELPER_APP_SUFFIXES.
for spec in ":" " (GPU):.gpu" " (Plugin):.plugin" " (Renderer):.renderer" " (Alerts):.alerts"; do
  suffix="${spec%%:*}"
  idsuffix="${spec##*:}"
  name="${BASE}${suffix}"
  app="${DEST}/${name}.app"

  rm -rf "${app}"
  mkdir -p "${app}/Contents/MacOS"
  cp "${HELPER_BIN}" "${app}/Contents/MacOS/${name}"

  cat > "${app}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>${name}</string>
	<key>CFBundleExecutable</key><string>${name}</string>
	<key>CFBundleIdentifier</key><string>${PRODUCT_BUNDLE_IDENTIFIER}.helper${idsuffix}</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>${name}</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSUIElement</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

  /usr/bin/codesign "${SIGN_FLAGS[@]}" "${app}"
  echo "embedded ${name}.app"
done
