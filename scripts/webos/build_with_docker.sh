#!/usr/bin/env bash
# Build Aurora for LG webOS via Docker, on macOS or Linux hosts.
# Mirrors build_with_docker.ps1 / docker_build_invoke.ps1 (Windows), so behavior
# stays identical across platforms. Requires Docker Desktop (or dockerd) running.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
INNER_SCRIPT="${SCRIPT_DIR}/docker_build_inner.sh"

echo "=== Aurora - Build for LG webOS (via Docker) ==="
echo ""

if ! docker ps >/dev/null 2>&1; then
    echo "ERROR: Docker is not running or is not installed."
    echo "  - Install Docker Desktop: https://www.docker.com/products/docker-desktop"
    exit 1
fi

echo "Using Docker to build in Ubuntu environment..."
if [ -n "${WEBOS_APPINFO_ID:-}" ]; then
    echo "  WEBOS_APPINFO_ID = ${WEBOS_APPINFO_ID}"
fi
if [ -n "${WEBOS_APPINFO_TITLE:-}" ]; then
    echo "  WEBOS_APPINFO_TITLE = ${WEBOS_APPINFO_TITLE}"
fi
echo ""

DOCKER_ENV_ARGS=(-e CI=1 -e DOCKER_SKIP_SUBMODULES=1)
if [ -n "${WEBOS_APPINFO_ID:-}" ]; then
    DOCKER_ENV_ARGS+=(-e "WEBOS_APPINFO_ID=${WEBOS_APPINFO_ID}")
fi
if [ -n "${WEBOS_APPINFO_TITLE:-}" ]; then
    DOCKER_ENV_ARGS+=(-e "WEBOS_APPINFO_TITLE=${WEBOS_APPINFO_TITLE}")
fi

docker run --rm \
    --platform linux/amd64 \
    --dns 8.8.8.8 --dns 8.8.4.4 --dns 1.1.1.1 \
    "${DOCKER_ENV_ARGS[@]}" \
    -v "${PROJECT_ROOT}:/build" \
    -v "${INNER_SCRIPT}:/docker_build.sh" \
    -v aurora-webos-sdk-cache:/tmp/arm-webos-linux-gnueabi_sdk-buildroot \
    -v aurora-webos-build-cache:/tmp/aurora-webos-build \
    -w /build \
    ubuntu:22.04 \
    bash -c "sed 's/\r\$//' /docker_build.sh | bash"

exit_code=$?

if [ ${exit_code} -eq 0 ]; then
    echo ""
    echo "=== Build complete! ==="
    echo "Package in: ${PROJECT_ROOT}/dist/"
    ls "${PROJECT_ROOT}"/dist/*.ipk 2>/dev/null || true
else
    echo ""
    echo "Build failed. Scroll up for the Docker log (apt/DNS, cmake, etc.)."
    exit 1
fi
