#!/usr/bin/env bash
set -euo pipefail

# Absolute path — do not derive from $0 (breaks when piped through sed|bash).
PROJECT="${AURORA_PROJECT:-/mnt/c/Projetos/moonlight/lg/moonlight-tv}"
cd "$PROJECT"

# Ignore Windows PATH (npm ares-package shim needs node.exe).
# Also fix HOME: `wsl ... bash -lc` often inherits Windows HOME like C:Users...
export PATH=/tmp/ares-prefix/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
if [ ! -d "${HOME:-}" ] || [[ "${HOME:-}" == *:* ]]; then
  export HOME="$(getent passwd "$(id -un)" | cut -d: -f6)"
fi
export HOME="${HOME:-/home/$(id -un)}"

export CI=1
export CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR:-/tmp/aurora-webos-build}"
export CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

SDK_VERSION=webos-a38c582
SDK_ARCHIVE=arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz
SDK_URL="https://github.com/openlgtv/buildroot-nc4/releases/download/${SDK_VERSION}/${SDK_ARCHIVE}"
# Persist outside /tmp so WSL reboots / tmp cleanups do not force a full re-download.
SDK_CACHE="${AURORA_SDK_CACHE:-$HOME/.cache/aurora-sdk}"
SDK_DIR="${WEBOS_SDK_DIR:-$SDK_CACHE/arm-webos-linux-gnueabi_sdk-buildroot}"

echo "=== Aurora WSL build ==="
echo "Project: $PROJECT"
echo "Build dir: $CMAKE_BINARY_DIR"
echo "SDK: $SDK_VERSION"
echo "SDK dir: $SDK_DIR"

if ! command -v cmake >/dev/null || ! command -v gawk >/dev/null || ! command -v curl >/dev/null; then
  echo "Installing build packages..."
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cmake gawk curl git build-essential ca-certificates wget file
fi

if [ ! -x /tmp/ares-prefix/usr/bin/ares-package ]; then
  echo "Installing ares-package..."
  cd /tmp
  if [ ! -f ares-package.deb ]; then
    curl -L --fail -o ares-package.deb \
      https://github.com/webosbrew/ares-cli-rs/releases/download/20241111-d97ba96/ares-package_0.1.4-1_amd64.deb
  fi
  rm -rf /tmp/ares-prefix
  mkdir -p /tmp/ares-prefix
  dpkg-deb -x ares-package.deb /tmp/ares-prefix
fi
which ares-package

mkdir -p "$SDK_CACHE"
if [ ! -f "$SDK_DIR/share/buildroot/toolchainfile.cmake" ]; then
  echo "Downloading SDK $SDK_VERSION..."
  cd "$SDK_CACHE"
  # Prefer a complete Windows-side cache if present (avoids re-fetch across WSL).
  WIN_SDK="/mnt/c/Projetos/moonlight/lg/moonlight-tv/.cache/$SDK_ARCHIVE"
  if [ -f "$WIN_SDK" ] && [ "$(stat -c%s "$WIN_SDK" 2>/dev/null || echo 0)" -ge 300000000 ]; then
    echo "Using Windows SDK cache: $WIN_SDK"
    cp -f "$WIN_SDK" "$SDK_ARCHIVE"
  fi
  if [ ! -f "$SDK_ARCHIVE" ] || [ "$(stat -c%s "$SDK_ARCHIVE" 2>/dev/null || echo 0)" -lt 300000000 ]; then
    curl -L --fail -C - --retry 10 --retry-delay 2 --retry-all-errors \
      -o "$SDK_ARCHIVE" "$SDK_URL"
  fi
  echo "Extracting..."
  rm -rf arm-webos-linux-gnueabi_sdk-buildroot
  tar -xzf "$SDK_ARCHIVE"
  ./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
  SDK_DIR="$SDK_CACHE/arm-webos-linux-gnueabi_sdk-buildroot"
fi

export TOOLCHAIN_FILE="$SDK_DIR/share/buildroot/toolchainfile.cmake"
if [ ! -f "$TOOLCHAIN_FILE" ]; then
  echo "Error: toolchain not found: $TOOLCHAIN_FILE"
  exit 1
fi
if [ ! -x "$SDK_DIR/bin/arm-webos-linux-gnueabi-gcc" ]; then
  echo "Error: webOS compiler not found"
  exit 1
fi

cd "$PROJECT"
# Incremental rebuild is fine; set DOCKER_CLEAN_BUILD=1 to wipe.
if [ "${DOCKER_CLEAN_BUILD:-0}" = "1" ]; then
  rm -rf "$CMAKE_BINARY_DIR"
fi

sed 's/\r$//' ./scripts/webos/easy_build.sh | bash -s -- -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" || {
  # easy_build fails if CPack cannot mkdir under Windows-mounted dist/. Binary may still be OK.
  if [ ! -x "$CMAKE_BINARY_DIR/aurora" ]; then
    exit 1
  fi
  echo "CPack via project dist failed; packaging to /tmp..."
}

# Always package from /tmp to avoid NTFS mkdir races under dist/_CPack_Packages.
CPACK_OUT=/tmp/aurora-cpack-out
rm -rf "$CPACK_OUT"
mkdir -p "$CPACK_OUT"
(cd "$CMAKE_BINARY_DIR" && cpack -D "CPACK_PACKAGE_DIRECTORY=$CPACK_OUT")

mkdir -p "$PROJECT/dist"
copied=0
produced=0
for ipk in "$CPACK_OUT"/*.ipk; do
  [ -f "$ipk" ] || continue
  produced=1
  base="$(basename "$ipk")"
  if cp -f "$ipk" "$PROJECT/dist/$base" 2>/dev/null; then
    copied=1
  else
    alt="$PROJECT/dist/${base%.ipk}.panel-phase.ipk"
    stamp="$PROJECT/dist/${base%.ipk}.$(date +%Y%m%d-%H%M%S).ipk"
    if cp -f "$ipk" "$alt" 2>/dev/null; then
      echo "Warning: $base locked; wrote $alt"
      copied=1
    elif cp -f "$ipk" "$stamp" 2>/dev/null; then
      echo "Warning: $base locked; wrote $stamp"
      copied=1
    else
      echo "Warning: could not copy $base to dist (Windows mount locked?)"
      echo "IPK remains at $ipk"
      echo "Copy from Windows: \\\\wsl$\\Ubuntu${ipk}"
    fi
  fi
done
echo "=== IPKs ==="
ls -la "$PROJECT/dist"/*.ipk 2>/dev/null || true
ls -la "$CPACK_OUT"/*.ipk 2>/dev/null || true
if [ "$produced" -eq 0 ]; then
  echo "Error: no IPK produced"
  exit 1
fi
if [ "$copied" -eq 0 ]; then
  echo "Warning: IPK not copied to dist/, but available under $CPACK_OUT"
fi
