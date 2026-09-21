#!/usr/bin/env bash
# Embed and sign CEF and its helper apps in the host Runner build phase.
set -euo pipefail

err() { echo "error: $* Run dart run webview_cef:setup_macos from your app directory." >&2; exit 1; }
PLUGIN_MACOS="${WEBVIEW_CEF_MACOS_DIR:?Include Flutter/webview_cef.xcconfig; run dart run webview_cef:setup_macos}"
CEF="${PLUGIN_MACOS}/third/cef"
case "${CONFIGURATION}" in
  Debug*) BUILD_TYPE=Debug ;;
  Release*|Profile*) BUILD_TYPE=Release ;;
  *) err "Unsupported build configuration: ${CONFIGURATION}." ;;
esac
HELPER_BIN="${CEF}/${BUILD_TYPE}/cef_helper"
ENT="${PLUGIN_MACOS}/helper/Helper.entitlements"
FRAMEWORK="Chromium Embedded Framework.framework"
[ -f "${HELPER_BIN}" ] || err "Missing ${BUILD_TYPE} CEF helper."
[ -f "${CEF}/${BUILD_TYPE}/libcef_dll_wrapper.a" ] || err "Missing ${BUILD_TYPE} CEF wrapper."
[ -f "${CEF}/${FRAMEWORK}/Chromium Embedded Framework" ] || err "Missing CEF framework."
CEF_VERSION="$(sed -n 's/^[[:space:]]*set(CEF_VERSION[[:space:]]*"\(.*\)").*/\1/p' "${PLUGIN_MACOS}/../third/download.cmake" | head -1)"
case "${ARCHS}" in
  arm64) CEF_ARCH=macosarm64 ;;
  x86_64) CEF_ARCH=macosx64 ;;
  *) err "CEF requires a single architecture, got ARCHS=${ARCHS}." ;;
esac
[ -f "${CEF}/version.txt" ] && [ "$(cat "${CEF}/version.txt")" = "${CEF_VERSION}_${CEF_ARCH}_swiftpm_v1" ] || err "CEF artifacts are stale or for another architecture."
for binary in "${HELPER_BIN}" "${CEF}/${FRAMEWORK}/Chromium Embedded Framework" "${CEF}/${BUILD_TYPE}/libcef_dll_wrapper.a"; do
  lipo "${binary}" -verify_arch "${ARCHS}" || err "CEF architecture mismatch: ${binary}."
done

BASE="${EXECUTABLE_NAME} Helper"
DEST="${TARGET_BUILD_DIR}/${FRAMEWORKS_FOLDER_PATH}"
IDENTITY="${EXPANDED_CODE_SIGN_IDENTITY:--}"
SIGN_FLAGS=(--force --sign "${IDENTITY}")
if [ "${IDENTITY}" = - ]; then
  SIGN_FLAGS+=(--timestamp=none)
else
  SIGN_FLAGS+=(--options runtime --timestamp)
fi
mkdir -p "${DEST}"
rm -rf "${DEST}/${FRAMEWORK}"
# ditto preserves the versioned framework symlinks.
/usr/bin/ditto "${CEF}/${FRAMEWORK}" "${DEST}/${FRAMEWORK}"
while IFS= read -r -d '' library; do
  /usr/bin/codesign "${SIGN_FLAGS[@]}" "${library}"
done < <(find "${DEST}/${FRAMEWORK}/Versions/A/Libraries" -type f -name '*.dylib' -print0)
/usr/bin/codesign "${SIGN_FLAGS[@]}" "${DEST}/${FRAMEWORK}"


# The helpers are nested executables the notary service checks on their own, and
# they are not Xcode targets, so the host app's ENABLE_HARDENED_RUNTIME never
# reaches them: the hardened runtime flag must be set right here, on every
# signature, or notarization rejects every one of them. "Every signature"
# includes ad-hoc ("-"): an archive built with CODE_SIGN_IDENTITY="-" signs the
# helpers ad-hoc, and `xcodebuild -exportArchive` re-signs nested bundles for
# Developer ID *preserving* the existing signature's flags and entitlements —
# it never adds the runtime flag itself, so a flag missing here is missing in
# the notarized artifact. An ad-hoc signature carries the flag fine; only a
# secure timestamp is impossible without a real identity (and the export
# re-sign supplies one anyway).
SIGN_FLAGS=(--force --sign "${IDENTITY}" --options runtime)
if [ "${IDENTITY}" = "-" ]; then
  SIGN_FLAGS+=(--timestamp=none)
else
  SIGN_FLAGS+=(--timestamp)
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
