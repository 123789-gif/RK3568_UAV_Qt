#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER_ASSET_DIR="$PROJECT_ROOT/assets/driver/rk3568-kernel-6.1.99"

BOARD="${1:-root@192.168.1.100}"
REMOTE_KO="${2:-/userdata/Projects/driver_config/pango_pci_driver.ko}"

mkdir -p "$DRIVER_ASSET_DIR"

TMP_DIR="$(mktemp -d)"
TMP_KO="$TMP_DIR/pango_pci_driver.ko"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Fetching verified Ethernet-control driver:"
echo "  $BOARD:$REMOTE_KO"
scp "$BOARD:$REMOTE_KO" "$TMP_KO"

if ! file "$TMP_KO" | grep -q "ARM aarch64"; then
    echo "ERROR: fetched file is not an ARM aarch64 kernel module." >&2
    file "$TMP_KO" >&2
    exit 1
fi

if command -v modinfo >/dev/null 2>&1; then
    VERMAGIC="$(modinfo -F vermagic "$TMP_KO" 2>/dev/null || true)"
    if [[ "$VERMAGIC" != *"6.1.99"* || "$VERMAGIC" != *"aarch64"* ]]; then
        echo "ERROR: unexpected vermagic: $VERMAGIC" >&2
        exit 1
    fi
fi

OLD_PREPROC_ONLY_SHA="ba83e3ccd5f9537d3b5dc2669efcbd2526a5e5e2972471886a91ccffcbad5dd2"
NEW_SHA="$(sha256sum "$TMP_KO" | awk '{print $1}')"

if [[ "$NEW_SHA" == "$OLD_PREPROC_ONLY_SHA" ]]; then
    echo "ERROR: the board still contains the old preprocessing-only driver." >&2
    echo "Rebuild/load the Ethernet-control driver in /userdata/Projects/driver_config first." >&2
    exit 1
fi

install -m 0644 "$TMP_KO" "$DRIVER_ASSET_DIR/pango_pci_driver.ko"
printf '%s  %s\n' "$NEW_SHA" "pango_pci_driver.ko" \
    > "$DRIVER_ASSET_DIR/SHA256SUMS"

{
    echo "Driver: pango_pci_driver.ko"
    echo "Source board: $BOARD"
    echo "Remote source: $REMOTE_KO"
    echo "Feature: FPGA preprocessing + Ethernet H2/UDP output control"
    echo "Required kernel: 6.1.99 aarch64"
    echo "SHA256: $NEW_SHA"
    echo "Synced at UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DRIVER_ASSET_DIR/DRIVER_INFO.txt"

echo "Driver asset updated:"
echo "  $DRIVER_ASSET_DIR/pango_pci_driver.ko"
echo "  SHA256=$NEW_SHA"
(
    cd "$DRIVER_ASSET_DIR"
    sha256sum -c SHA256SUMS
)
