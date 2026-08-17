#!/usr/bin/env bash
set -euo pipefail
export HOME=/home/guilherme
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

CACHE=/home/guilherme/.cache/aurora-sdk
ARCHIVE=arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz
WIN_CACHE="/mnt/c/Projetos/moonlight/lg/moonlight-tv/.cache/$ARCHIVE"

mkdir -p "$CACHE"
cd "$CACHE"

wsz=$(stat -c%s "$WIN_CACHE")
echo "Windows archive: $wsz bytes"
if [ "$wsz" -lt 300000000 ]; then
  echo "Error: Windows archive incomplete"
  exit 1
fi
cp -f "$WIN_CACHE" "$ARCHIVE"

if [ ! -f "$CACHE/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake" ]; then
  echo "Extracting SDK..."
  rm -rf arm-webos-linux-gnueabi_sdk-buildroot
  tar -xzf "$ARCHIVE"
  ./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
fi

test -x "$CACHE/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc"
echo "SDK ready at $CACHE/arm-webos-linux-gnueabi_sdk-buildroot"
