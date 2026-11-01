#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/r36s"

echo "============================================================"
echo " Building NFSU2 Recompiled for R36S (Linux aarch64)         "
echo "============================================================"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

ARCH="$(uname -m)"
OS="$(uname -s)"
TOOLCHAIN_ARGS=""

# Check if cross-compiling to aarch64
if [ "$ARCH" != "aarch64" ] && [ "$ARCH" != "arm64" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        echo "[INFO] Using aarch64-linux-gnu cross compiler..."
        TOOLCHAIN_ARGS="-DCMAKE_TOOLCHAIN_FILE=$SCRIPT_DIR/toolchain-aarch64.cmake"
    else
        echo "[NOTICE] aarch64-linux-gnu-gcc not detected on host."
        echo "         Proceeding with host compiler for local compilation check."
    fi
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" $TOOLCHAIN_ARGS -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "============================================================"
echo " Build Completed Successfully!                              "
echo " Binary packaged in: $SCRIPT_DIR/package/nfsu2.aarch64      "
echo "============================================================"
