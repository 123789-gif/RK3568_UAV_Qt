#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/out/arm64-release}"
QMAKE_BIN="${QMAKE_BIN:-/home/simba/Qt-5.12.9-arm64/bin/qmake}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"
JOBS="${JOBS:-$(nproc)}"

PROJECT_ROOT_REAL="$(realpath -m "$PROJECT_ROOT")"
BUILD_DIR_REAL="$(realpath -m "$BUILD_DIR")"

case "$BUILD_DIR_REAL" in
"$PROJECT_ROOT_REAL"/out/*)
    ;;
*)
    echo "ERROR: 为防止误删除，BUILD_DIR 必须位于工程 out/ 目录下。" >&2
    echo "PROJECT_ROOT=$PROJECT_ROOT_REAL" >&2
    echo "BUILD_DIR=$BUILD_DIR_REAL" >&2
    exit 2
    ;;
esac

[[ -x "$QMAKE_BIN" ]] || {
    echo "ERROR: 找不到可执行 qmake：$QMAKE_BIN" >&2
    exit 1
}

if [[ "$CLEAN_BUILD" == "1" ]]; then
    rm -rf --one-file-system -- "$BUILD_DIR_REAL"
elif [[ "$CLEAN_BUILD" != "0" ]]; then
    echo "ERROR: CLEAN_BUILD 只能是 0 或 1。" >&2
    exit 2
fi

BUILD_DIR="$BUILD_DIR_REAL"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$QMAKE_BIN" "$PROJECT_ROOT/rk_dual_screen_video.pro" CONFIG+=release
make -j"$JOBS"

echo "Build output: $BUILD_DIR/rk_dual_screen_video"
