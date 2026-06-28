#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-}"
OUTPUT_DIR="${2:-$PROJECT_ROOT/dist}"

if [[ -z "$BINARY" || ! -f "$BINARY" ]]; then
    echo "Usage: $0 <arm64-rk_dual_screen_video> [output-dir]" >&2
    exit 2
fi

if ! file "$BINARY" | grep -q "ARM aarch64"; then
    echo "ERROR: binary is not ARM aarch64: $BINARY" >&2
    file "$BINARY" >&2
    exit 1
fi

VERSION="$(date -u +%Y%m%dT%H%M%SZ)"
APP_DIR="$OUTPUT_DIR/RK3568_PTZ_Qt"
ARCHIVE_NAME="RK3568_PTZ_Qt_${VERSION}.tar.gz"
ARCHIVE="$OUTPUT_DIR/$ARCHIVE_NAME"

rm -rf "$APP_DIR"
mkdir -p \
    "$APP_DIR/bin" \
    "$APP_DIR/models" \
    "$APP_DIR/config" \
    "$APP_DIR/scripts" \
    "$APP_DIR/logs" \
    "$APP_DIR/run"

install -m 0755 \
    "$BINARY" \
    "$APP_DIR/bin/rk_dual_screen_video"

cp -f "$PROJECT_ROOT"/assets/models/* \
    "$APP_DIR/models/"

cp -f "$PROJECT_ROOT/assets/config/runtime.env" \
    "$APP_DIR/config/"

cp -f "$PROJECT_ROOT"/deploy/scripts/*.sh \
    "$APP_DIR/scripts/"

chmod +x "$APP_DIR/scripts"/*.sh

printf '%s\n' "$VERSION" > "$APP_DIR/VERSION"

(
    cd "$APP_DIR"

    find bin models config scripts \
        -type f \
        -print0 |
        sort -z |
        xargs -0 sha256sum > MANIFEST.sha256

    sha256sum VERSION >> MANIFEST.sha256
)

rm -f "$OUTPUT_DIR"/RK3568_PTZ_Qt_*.tar.gz
rm -f "$OUTPUT_DIR"/RK3568_PTZ_Qt_*.tar.gz.sha256

tar -C "$OUTPUT_DIR" \
    -czf "$ARCHIVE" \
    RK3568_PTZ_Qt

(
    cd "$OUTPUT_DIR"
    sha256sum "$ARCHIVE_NAME" > "$ARCHIVE_NAME.sha256"
)

echo "Version:          $VERSION"
echo "Bundle directory: $APP_DIR"
echo "Bundle archive:   $ARCHIVE"
echo "Archive SHA256:   $ARCHIVE.sha256"
