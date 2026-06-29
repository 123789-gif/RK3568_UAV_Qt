#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$APP_ROOT/config/runtime.env"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "ERROR: load_pcie_driver.sh must run as root." >&2
    exit 1
fi

MODULE_NAME="${PCIE_DRIVER_MODULE:-pango_pci_driver}"
DEVICE_NODE="${PCIE_DRIVER_DEVICE:-/dev/pango_pci_driver}"
DRIVER_FILE="${PCIE_DRIVER_FILE:-pango_pci_driver.ko}"
DRIVER_DIR="$APP_ROOT/driver"
DRIVER_KO="$DRIVER_DIR/$DRIVER_FILE"
SHA_FILE="$DRIVER_DIR/SHA256SUMS"

dump_driver_log() {
    dmesg -T 2>/dev/null |
        grep -Ei 'pango|pcie|pci|dma|BAR|error|fail' |
        tail -n 120 >&2 || true
}

[[ -f "$DRIVER_KO" ]] || {
    echo "ERROR: PCIe driver file missing: $DRIVER_KO" >&2
    exit 1
}

[[ -f "$SHA_FILE" ]] || {
    echo "ERROR: PCIe driver SHA256 file missing: $SHA_FILE" >&2
    exit 1
}

if ! (
    cd "$DRIVER_DIR"
    sha256sum -c SHA256SUMS >/dev/null
); then
    echo "ERROR: PCIe driver SHA256 mismatch." >&2
    exit 1
fi

DRIVER_FILE_INFO="$(file "$DRIVER_KO" 2>&1 || true)"
if ! grep -q "ARM aarch64" <<<"$DRIVER_FILE_INFO"; then
    echo "ERROR: PCIe driver is not ARM aarch64: $DRIVER_FILE_INFO" >&2
    exit 1
fi

DRIVER_VERMAGIC="$(modinfo -F vermagic "$DRIVER_KO" 2>/dev/null || true)"
DRIVER_KERNEL="${DRIVER_VERMAGIC%% *}"
RUNNING_KERNEL="$(uname -r)"

if [[ -z "$DRIVER_KERNEL" ]]; then
    echo "ERROR: unable to read PCIe driver vermagic." >&2
    exit 1
fi

if [[ "$DRIVER_KERNEL" != "$RUNNING_KERNEL" ]]; then
    echo "ERROR: PCIe driver kernel mismatch: driver=$DRIVER_KERNEL running=$RUNNING_KERNEL" >&2
    exit 1
fi

MODULE_LOADED=0
grep -q "^${MODULE_NAME} " /proc/modules && MODULE_LOADED=1 || true

if [[ -c "$DEVICE_NODE" ]]; then
    if [[ $MODULE_LOADED -eq 1 ]]; then
        echo "[OK] PCIe driver already ready: module=$MODULE_NAME device=$DEVICE_NODE"
        exit 0
    fi

    echo "ERROR: stale PCIe device node exists but module is not loaded: $DEVICE_NODE" >&2
    exit 1
fi

if [[ $MODULE_LOADED -eq 1 ]]; then
    echo "ERROR: PCIe module is loaded but character device is missing: $DEVICE_NODE" >&2
    dump_driver_log
    exit 1
fi

echo "Loading PCIe driver: $DRIVER_KO"
if ! insmod "$DRIVER_KO"; then
    echo "ERROR: insmod failed: $DRIVER_KO" >&2
    dump_driver_log
    exit 1
fi

for _ in $(seq 1 50); do
    [[ -c "$DEVICE_NODE" ]] && break
    sleep 0.1
done

if ! grep -q "^${MODULE_NAME} " /proc/modules; then
    echo "ERROR: PCIe module was not registered after insmod: $MODULE_NAME" >&2
    dump_driver_log
    exit 1
fi

if [[ ! -c "$DEVICE_NODE" ]]; then
    echo "ERROR: PCIe character device was not created: $DEVICE_NODE" >&2
    dump_driver_log
    exit 1
fi

echo "[OK] PCIe driver loaded: module=$MODULE_NAME device=$DEVICE_NODE"
