#!/usr/bin/env bash
#
# Download the official CEF "Standard Distribution" for macOS and prepare
# macos/third/cef for the SwiftPM build. This is the macOS counterpart of
# the CMake `prepare_prebuilt_files` path used on Windows/Linux
# (see third/download.cmake): nothing under macos/third/cef is tracked in git;
# it is fetched/built on demand.
#
# It performs three steps that previously had to be done by hand (see README):
#   1. download + extract the CEF distribution (provides include/ headers)
#   2. lay the framework out as a versioned macOS bundle (Versions/A + symlinks)
#   3. build libcef_dll_wrapper.a from the distribution's sources
#
# The CEF version is read from third/download.cmake so there is a single source
# of truth shared with Windows/Linux. Re-running is cheap: if the destination is
# already populated for the pinned version it exits immediately.
#
# Both Debug and Release wrappers/helpers are prepared together.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"          # .../macos
REPO_ROOT="$(cd "${MACOS_DIR}/.." && pwd)"           # repo root
DEST="${MACOS_DIR}/third/cef"
DOWNLOAD_CMAKE="${REPO_ROOT}/third/download.cmake"
CDN="https://cef-builds.spotifycdn.com"

err() { echo "error: $*" >&2; exit 1; }

# --- resolve version (single source of truth: third/download.cmake) ----------
[ -f "${DOWNLOAD_CMAKE}" ] || err "cannot find ${DOWNLOAD_CMAKE}"
CEF_VERSION="$(sed -n 's/^[[:space:]]*set(CEF_VERSION[[:space:]]*"\(.*\)").*/\1/p' "${DOWNLOAD_CMAKE}" | head -1)"
[ -n "${CEF_VERSION}" ] || err "could not parse CEF_VERSION from ${DOWNLOAD_CMAKE}"

case "$(uname -m)" in
  arm64)  CEF_ARCH="macosarm64"; PROJECT_ARCH="arm64" ;;
  x86_64) CEF_ARCH="macosx64";   PROJECT_ARCH="x86_64" ;;
  *)      err "unsupported arch: $(uname -m)" ;;
esac

PKG="cef_binary_${CEF_VERSION}_${CEF_ARCH}"
STAMP="${DEST}/version.txt"
WANT="${CEF_VERSION}_${CEF_ARCH}_swiftpm_v1"

# --- skip if already prepared for this exact version/arch/type ---------------
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}" 2>/dev/null)" = "${WANT}" ] \
   && [ -f "${DEST}/Debug/libcef_dll_wrapper.a" ] \
   && [ -f "${DEST}/Release/libcef_dll_wrapper.a" ] \
   && [ -f "${DEST}/Debug/cef_helper" ] \
   && [ -f "${DEST}/Release/cef_helper" ] \
   && [ -e "${DEST}/Chromium Embedded Framework.framework/Resources/Info.plist" ] \
   && [ -f "${DEST}/include/cef_version.h" ]; then
  echo "CEF ${WANT} already prepared in ${DEST} — nothing to do."
  exit 0
fi

command -v cmake >/dev/null || err "cmake not found (brew install cmake) — needed to build libcef_dll_wrapper"
if command -v ninja >/dev/null && ninja --version >/dev/null 2>&1; then
  GENERATOR="Ninja"; BUILD_TOOL=(ninja libcef_dll_wrapper)
else
  GENERATOR="Unix Makefiles"; BUILD_TOOL=(make -j"$(sysctl -n hw.ncpu)" libcef_dll_wrapper)
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cef_dl.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
TARBALL="${WORK}/${PKG}.tar.bz2"
# CDN requires '+' percent-encoded as %2B.
URL="${CDN}/$(printf '%s' "${PKG}.tar.bz2" | sed 's/+/%2B/g')"

echo "==> Downloading ${PKG}.tar.bz2"
curl -L --fail --connect-timeout 30 -o "${TARBALL}" "${URL}"

echo "==> Extracting"
tar -xjf "${TARBALL}" -C "${WORK}"
SRC="${WORK}/${PKG}"
[ -d "${SRC}" ] || err "extracted dir ${SRC} not found"

echo "==> Installing into ${DEST}"
# Invalidate the stamp first, so interrupted preparation cannot look complete.
rm -f "${STAMP}"
rm -rf "${DEST}/include" "${DEST}/Chromium Embedded Framework.framework" \
       "${DEST}/Debug" "${DEST}/Release"
mkdir -p "${DEST}"
cp -R "${SRC}/include" "${DEST}/include"

# Lay the framework out as a versioned macOS bundle (Xcode embed/sign requires
# Versions/Current/Resources/Info.plist; CEF ships a flat bundle).
FW_SRC="${SRC}/Release/Chromium Embedded Framework.framework"
FW_DST="${DEST}/Chromium Embedded Framework.framework"
mkdir -p "${FW_DST}/Versions/A"
cp -R "${FW_SRC}/Chromium Embedded Framework" "${FW_DST}/Versions/A/"
cp -R "${FW_SRC}/Libraries" "${FW_DST}/Versions/A/"
cp -R "${FW_SRC}/Resources" "${FW_DST}/Versions/A/"
ln -sfn A "${FW_DST}/Versions/Current"
ln -sfn "Versions/Current/Chromium Embedded Framework" "${FW_DST}/Chromium Embedded Framework"
ln -sfn Versions/Current/Libraries "${FW_DST}/Libraries"
ln -sfn Versions/Current/Resources "${FW_DST}/Resources"

# CEF's Release framework is used in both modes. The wrapper ABI still needs
# to match the plugin's NDEBUG setting (DCHECK_IS_ON changes inline definitions).
for BUILD_TYPE in Debug Release; do
  OUT="${DEST}/${BUILD_TYPE}"
  mkdir -p "${OUT}"
  echo "==> Building libcef_dll_wrapper (${BUILD_TYPE}, ${PROJECT_ARCH})"
  cmake -S "${SRC}" -B "${SRC}/build-${BUILD_TYPE}" -G "${GENERATOR}" \
    -DPROJECT_ARCH="${PROJECT_ARCH}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 >/dev/null
  ( cd "${SRC}/build-${BUILD_TYPE}" && "${BUILD_TOOL[@]}" >/dev/null )
  cp "${SRC}/build-${BUILD_TYPE}/libcef_dll_wrapper/libcef_dll_wrapper.a" "${OUT}/"

  FLAGS=(-g)
  if [ "${BUILD_TYPE}" = Release ]; then FLAGS=(-O2 -DNDEBUG); fi
  echo "==> Building CEF helper (${BUILD_TYPE})"
  clang++ -std=c++20 -stdlib=libc++ -mmacosx-version-min=12.0 -arch "${PROJECT_ARCH}" -w \
    "${FLAGS[@]}" -I "${DEST}" -I "${REPO_ROOT}/common" \
    "${MACOS_DIR}/helper/cef_helper_main.mm" "${OUT}/libcef_dll_wrapper.a" \
    -framework Foundation -framework AppKit -Wl,-ObjC -o "${OUT}/cef_helper"
done

echo "${WANT}" > "${STAMP}"
echo "==> Done: CEF ${CEF_VERSION} (${CEF_ARCH}, Debug + Release) ready in ${DEST}"
