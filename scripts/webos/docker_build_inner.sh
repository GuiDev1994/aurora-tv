#!/usr/bin/env bash
set -e

install_apt_packages() {
    # Docker Desktop on Windows often resolves Ubuntu mirrors to broken IPv6; force IPv4.
    apt-get -o Acquire::ForceIPv4=true update -qq && \
      apt-get -o Acquire::ForceIPv4=true install -y -qq cmake gawk curl git build-essential ca-certificates wget file
}

if ! install_apt_packages > /dev/null 2>&1; then
    echo "apt-get failed (often Docker DNS). Retrying with public nameservers..."
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
    if ! install_apt_packages > /dev/null; then
        echo "Error: could not install build packages. Check Docker Desktop network/DNS and retry."
        exit 1
    fi
fi

# Install ares-package (required to generate the webOS IPK)
echo "Installing ares-package..."
cd /tmp
wget -q https://github.com/webosbrew/ares-cli-rs/releases/download/20241111-d97ba96/ares-package_0.1.4-1_amd64.deb
apt-get install -y -qq ./ares-package_0.1.4-1_amd64.deb 2>/dev/null || (dpkg -i ares-package_0.1.4-1_amd64.deb && apt-get -f install -y -qq)
which ares-package || { echo "Error: ares-package not installed"; exit 1; }

cd /build
# Windows bind mounts can leave stale git submodule lock files in .git/modules.
find .git/modules -name "*.lock" -delete 2>/dev/null || true
# The project is bind-mounted from the host with submodules already checked out.
# Updating here fails when ss4s (or others) have local patches — skip by default.
if [ "${DOCKER_SKIP_SUBMODULES:-1}" = "1" ] || [ -n "${CI}" ]; then
    echo "Skipping git submodule update (using host checkout)."
else
    git submodule sync --recursive
    git submodule update --init --recursive
fi

# Download SDK
cd /tmp
if [ ! -d arm-webos-linux-gnueabi_sdk-buildroot ] || \
   [ ! -f arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake ]; then
    if [ ! -f arm-webos-linux-gnueabi_sdk-buildroot.tar.gz ]; then
        echo "Downloading webOS SDK..."
        curl -sL -O https://github.com/openlgtv/buildroot-nc4/releases/download/webos-b17b4cc/arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
    fi
    echo "Extracting SDK..."
    tar -xzf arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
    find arm-webos-linux-gnueabi_sdk-buildroot -type f \( -name '*.sh' -o -name 'relocate-sdk' \) -exec sed -i 's/\r$//' {} +
    ./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
fi

cd /build
SDK_ROOT=/tmp/arm-webos-linux-gnueabi_sdk-buildroot
export TOOLCHAIN_FILE="${SDK_ROOT}/share/buildroot/toolchainfile.cmake"
if [ ! -f "${TOOLCHAIN_FILE}" ]; then
  echo "Error: toolchain not found: ${TOOLCHAIN_FILE}"
  exit 1
fi
if [ ! -x "${SDK_ROOT}/bin/arm-webos-linux-gnueabi-gcc" ]; then
  echo "Error: webOS compiler not found: ${SDK_ROOT}/bin/arm-webos-linux-gnueabi-gcc"
  exit 1
fi
# Build outside the Windows bind mount: cmake try_compile breaks on NTFS/exFAT volumes.
export CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR:-/tmp/aurora-webos-build}"
if [ "${DOCKER_CLEAN_BUILD:-0}" = "1" ]; then
    rm -rf "${CMAKE_BINARY_DIR}"
fi
export CI=1
sed 's/\r$//' ./scripts/webos/easy_build.sh | bash -s -- -DCMAKE_BUILD_TYPE=Release

mkdir -p dist
find "${CMAKE_BINARY_DIR}" -maxdepth 3 -name '*.ipk' -exec cp -f {} dist/ \;
