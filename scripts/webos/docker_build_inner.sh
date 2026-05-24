#!/usr/bin/env bash
set -e

apt-get update -qq && apt-get install -y -qq cmake gawk curl git build-essential ca-certificates wget file > /dev/null

# Instalar ares-package (necessario para gerar o IPK webOS)
echo "Instalando ares-package..."
cd /tmp
wget -q https://github.com/webosbrew/ares-cli-rs/releases/download/20241111-d97ba96/ares-package_0.1.4-1_amd64.deb
apt-get install -y -qq ./ares-package_0.1.4-1_amd64.deb 2>/dev/null || (dpkg -i ares-package_0.1.4-1_amd64.deb && apt-get -f install -y -qq)
which ares-package || { echo "Erro: ares-package nao instalado"; exit 1; }

cd /build
git submodule update --init --recursive
rm -rf build/webos-easy_build

# Download SDK
cd /tmp
if [ ! -f arm-webos-linux-gnueabi_sdk-buildroot.tar.gz ]; then
    echo "Baixando webOS SDK..."
    curl -sL -O https://github.com/openlgtv/buildroot-nc4/releases/download/webos-b17b4cc/arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
fi
tar -xzf arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh

cd /build
SDK_ROOT=/tmp/arm-webos-linux-gnueabi_sdk-buildroot
export TOOLCHAIN_FILE="${SDK_ROOT}/share/buildroot/toolchainfile.cmake"
if [ ! -f "${TOOLCHAIN_FILE}" ]; then
  echo "Erro: toolchain nao encontrado: ${TOOLCHAIN_FILE}"
  exit 1
fi
if [ ! -x "${SDK_ROOT}/bin/arm-webos-linux-gnueabi-gcc" ]; then
  echo "Erro: compilador webOS nao encontrado: ${SDK_ROOT}/bin/arm-webos-linux-gnueabi-gcc"
  exit 1
fi
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
