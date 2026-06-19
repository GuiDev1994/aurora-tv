#!/usr/bin/env bash
# Build Aurora for LG webOS (compatible with LG C1 and other webOS TVs)
# Requires: Linux or WSL2 with Ubuntu

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SDK_VERSION="webos-b17b4cc"
SDK_ARCHIVE="arm-webos-linux-gnueabi_sdk-buildroot.tar.gz"
SDK_URL="https://github.com/openlgtv/buildroot-nc4/releases/download/${SDK_VERSION}/${SDK_ARCHIVE}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
SDK_DIR="${WEBOS_SDK_DIR:-/tmp/arm-webos-linux-gnueabi_sdk-buildroot}"

cd "${PROJECT_ROOT}"

echo "=== Aurora - Build for LG webOS ==="
echo "Project: ${PROJECT_ROOT}"
echo "Build type: ${BUILD_TYPE}"
echo ""

# Check we are in the project root
if [ ! -f "scripts/webos/easy_build.sh" ]; then
    echo "Error: Run this script from the project root"
    exit 1
fi

# Check dependencies
for cmd in cmake awk gawk; do
    if ! command -v $cmd &>/dev/null; then
        echo "Error: $cmd not found. Install with: sudo apt-get install cmake gawk"
        exit 1
    fi
done

# Initialize submodules
echo "Updating submodules..."
git submodule update --init --recursive

# Download and configure webOS SDK if not present
if [ ! -f "${SDK_DIR}/share/buildroot/toolchainfile.cmake" ]; then
    echo ""
    echo "WebOS SDK not found at ${SDK_DIR}"
    echo "Downloading SDK (buildroot-nc4 ${SDK_VERSION})..."
    
    mkdir -p /tmp
    cd /tmp
    
    if [ ! -f "${SDK_ARCHIVE}" ]; then
        if command -v curl &>/dev/null; then
            curl -L -O "${SDK_URL}"
        elif command -v wget &>/dev/null; then
            wget "${SDK_URL}"
        else
            echo "Error: curl or wget required to download the SDK"
            exit 1
        fi
    fi
    
    echo "Extracting SDK..."
    tar -xzf "${SDK_ARCHIVE}"
    
    if [ -d "arm-webos-linux-gnueabi_sdk-buildroot" ]; then
        echo "Relocating SDK..."
        ./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
        SDK_DIR="/tmp/arm-webos-linux-gnueabi_sdk-buildroot"
    else
        echo "Error: Unexpected SDK structure after extraction"
        exit 1
    fi
    
    cd "${PROJECT_ROOT}"
else
    echo "WebOS SDK found at ${SDK_DIR}"
fi

TOOLCHAIN_FILE="${SDK_DIR}/share/buildroot/toolchainfile.cmake"

if [ ! -f "${TOOLCHAIN_FILE}" ]; then
    echo "Error: Toolchain not found at ${TOOLCHAIN_FILE}"
    exit 1
fi

echo ""
echo "Running build..."
export TOOLCHAIN_FILE
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo ""
echo "=== Build complete! ==="
echo "IPK package generated at: ${PROJECT_ROOT}/dist/"
ls -la "${PROJECT_ROOT}/dist/"/*.ipk 2>/dev/null || true
echo ""
echo "To install on your TV:"
echo "  1. Install webosbrew and dev-manager on the TV"
echo "  2. Use: ares-install dist/com.aurora.gamestream_*_arm.ipk -d <TV_IP>"
echo "  Or use webOS Dev Manager for easy installation"
echo ""
