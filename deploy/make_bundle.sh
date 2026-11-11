#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-}"
OUTPUT_DIR="${2:-$PROJECT_ROOT/dist}"
DRIVER_SOURCE_DIR="$PROJECT_ROOT/assets/driver/rk3568-kernel-6.1.99"

if [[ -z "$BINARY" || ! -f "$BINARY" ]]; then
    echo "Usage: $0 <arm64-rk_dual_screen_video> [output-dir]" >&2
    exit 2
fi

if ! file "$BINARY" | grep -q "ARM aarch64"; then
    echo "ERROR: binary is not ARM aarch64: $BINARY" >&2
    file "$BINARY" >&2
    exit 1
fi

for file in pango_pci_driver.ko SHA256SUMS DRIVER_INFO.txt; do
    [[ -f "$DRIVER_SOURCE_DIR/$file" ]] || {
        echo "ERROR: missing PCIe driver asset: $DRIVER_SOURCE_DIR/$file" >&2
        exit 1
    }
done

if ! (
    cd "$DRIVER_SOURCE_DIR"
    sha256sum -c SHA256SUMS >/dev/null
); then
    echo "ERROR: source PCIe driver SHA256 mismatch." >&2
    exit 1
fi

LEGACY_PREPROC_ONLY_DRIVER_SHA256="ba83e3ccd5f9537d3b5dc2669efcbd2526a5e5e2972471886a91ccffcbad5dd2"
CURRENT_DRIVER_SHA256="$(sha256sum "$DRIVER_SOURCE_DIR/pango_pci_driver.ko" | awk '{print $1}')"

if [[ "$CURRENT_DRIVER_SHA256" == "$LEGACY_PREPROC_ONLY_DRIVER_SHA256" ]]; then
    echo "ERROR: driver asset is the old preprocessing-only .ko and has no Ethernet ioctl." >&2
    echo "Run: tools/sync_eth_driver_from_board.sh root@192.168.1.100" >&2
    exit 1
fi

VERSION="$(date -u +%Y%m%dT%H%M%SZ)"
APP_DIR="$OUTPUT_DIR/RK3568_UAV_Qt"
ARCHIVE_NAME="RK3568_UAV_Qt_${VERSION}.tar.gz"
ARCHIVE="$OUTPUT_DIR/$ARCHIVE_NAME"

rm -rf "$APP_DIR"
mkdir -p \
    "$APP_DIR/bin" \
    "$APP_DIR/models" \
    "$APP_DIR/config" \
    "$APP_DIR/driver" \
    "$APP_DIR/scripts" \
    "$APP_DIR/logs" \
    "$APP_DIR/run"

install -m 0755 \
    "$BINARY" \
    "$APP_DIR/bin/rk_dual_screen_video"

cp -a "$PROJECT_ROOT/assets/models/." \
    "$APP_DIR/models/"

cp -f "$PROJECT_ROOT"/assets/config/* \
    "$APP_DIR/config/"

cp -f "$DRIVER_SOURCE_DIR"/pango_pci_driver.ko \
      "$DRIVER_SOURCE_DIR"/SHA256SUMS \
      "$DRIVER_SOURCE_DIR"/DRIVER_INFO.txt \
      "$APP_DIR/driver/"

cp -f "$PROJECT_ROOT"/deploy/scripts/*.sh \
    "$APP_DIR/scripts/"

chmod 0755 "$APP_DIR/scripts"/*.sh
chmod 0644 "$APP_DIR/driver"/*

printf '%s\n' "$VERSION" > "$APP_DIR/VERSION"

(
    cd "$APP_DIR"

    find bin models config driver scripts \
        -type f \
        -print0 |
        sort -z |
        xargs -0 sha256sum > MANIFEST.sha256

    sha256sum VERSION >> MANIFEST.sha256
)

rm -f "$OUTPUT_DIR"/RK3568_UAV_Qt_*.tar.gz
rm -f "$OUTPUT_DIR"/RK3568_UAV_Qt_*.tar.gz.sha256

tar -C "$OUTPUT_DIR" \
    -czf "$ARCHIVE" \
    RK3568_UAV_Qt

(
    cd "$OUTPUT_DIR"
    sha256sum "$ARCHIVE_NAME" > "$ARCHIVE_NAME.sha256"
)

echo "Version:          $VERSION"
echo "Bundle directory: $APP_DIR"
echo "Bundle archive:   $ARCHIVE"
echo "Archive SHA256:   $ARCHIVE.sha256"
