#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$(cd "$PROJECT_ROOT/.." && pwd)/build-RK3568_PTZ_Qt-arm64-Release}"
QMAKE_BIN="${QMAKE_BIN:-/home/simba/Qt-5.12.9-arm64/bin/qmake}"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$QMAKE_BIN" "$PROJECT_ROOT/rk_dual_screen_video.pro" CONFIG+=release
make -j"$(nproc)"

echo "Build output: $BUILD_DIR/rk_dual_screen_video"
