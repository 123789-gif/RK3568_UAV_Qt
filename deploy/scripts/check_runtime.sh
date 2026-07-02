#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$APP_ROOT/config/runtime.env"

BIN="$APP_ROOT/bin/rk_dual_screen_video"
DRIVER_DIR="$APP_ROOT/driver"
DRIVER_FILE="${PCIE_DRIVER_FILE:-pango_pci_driver.ko}"
DRIVER_KO="$DRIVER_DIR/$DRIVER_FILE"
MODULE_NAME="${PCIE_DRIVER_MODULE:-pango_pci_driver}"
DEVICE_NODE="${PCIE_DRIVER_DEVICE:-/dev/pango_pci_driver}"
FAIL=0

ok() {
    printf '[OK] %s\n' "$*"
}

err() {
    printf '[FAIL] %s\n' "$*" >&2
    FAIL=1
}

warn() {
    printf '[WARN] %s\n' "$*" >&2
}

if [[ -x "$BIN" ]]; then
    ok "executable: $BIN"
else
    err "missing executable: $BIN"
fi

if [[ -f "$APP_ROOT/MANIFEST.sha256" ]]; then
    if (
        cd "$APP_ROOT"
        sha256sum -c MANIFEST.sha256 >/dev/null
    ); then
        ok "deployment MANIFEST SHA256"
    else
        err "deployment MANIFEST SHA256 mismatch"
    fi
else
    err "missing MANIFEST.sha256"
fi

if [[ -x "$BIN" ]]; then
    FILE_INFO="$(file "$BIN" 2>&1 || true)"

    if grep -q "ARM aarch64" <<<"$FILE_INFO"; then
        ok "executable architecture: ARM aarch64"
    else
        err "invalid executable architecture: $FILE_INFO"
    fi
fi

if [[ -x "$BIN" ]]; then
    DYNAMIC_INFO="$(readelf -d "$BIN" 2>&1)"
    READELF_RC=$?

    if [[ $READELF_RC -ne 0 ]]; then
        err "readelf failed: $DYNAMIC_INFO"
    else
        for lib in \
            libdrm.so.2 \
            librga.so.2 \
            librknnrt.so \
            libopencv_core.so.406 \
            libopencv_imgproc.so.406
        do
            if grep -Fq "[$lib]" <<<"$DYNAMIC_INFO"; then
                ok "ELF dependency: $lib"
            else
                err "missing or corrupt ELF dependency: $lib"
            fi
        done
    fi
fi

for model in ped_det.rknn plate_det.rknn plate_reco.rknn; do
    if [[ -s "$APP_ROOT/models/$model" ]]; then
        ok "model: $model"
    else
        err "missing model: $model"
    fi
done

if [[ -f "$DRIVER_KO" ]]; then
    ok "PCIe driver file: $DRIVER_KO"

    DRIVER_INFO="$(file "$DRIVER_KO" 2>&1 || true)"
    if grep -q "ARM aarch64" <<<"$DRIVER_INFO"; then
        ok "PCIe driver architecture: ARM aarch64"
    else
        err "invalid PCIe driver architecture: $DRIVER_INFO"
    fi

    if [[ -f "$DRIVER_DIR/SHA256SUMS" ]] && (
        cd "$DRIVER_DIR"
        sha256sum -c SHA256SUMS >/dev/null
    ); then
        ok "PCIe driver SHA256"
    else
        err "PCIe driver SHA256 mismatch or missing SHA256SUMS"
    fi

    DRIVER_VERMAGIC="$(modinfo -F vermagic "$DRIVER_KO" 2>/dev/null || true)"
    DRIVER_KERNEL="${DRIVER_VERMAGIC%% *}"
    RUNNING_KERNEL="$(uname -r)"

    if [[ -n "$DRIVER_KERNEL" && "$DRIVER_KERNEL" == "$RUNNING_KERNEL" ]]; then
        ok "PCIe driver vermagic: $DRIVER_VERMAGIC"
    else
        err "PCIe driver kernel mismatch: driver=${DRIVER_KERNEL:-unknown} running=$RUNNING_KERNEL"
    fi
else
    err "missing PCIe driver file: $DRIVER_KO"
fi

if grep -q "^${MODULE_NAME} " /proc/modules; then
    ok "PCIe module loaded: $MODULE_NAME"
else
    err "PCIe module not loaded: $MODULE_NAME"
fi

if [[ -c "$DEVICE_NODE" ]]; then
    ok "PCIe character device: $DEVICE_NODE"
else
    err "missing PCIe character device: $DEVICE_NODE"
fi

for dev in \
    /dev/dri/card0 \
    /dev/ttyS0 \
    /dev/mem
do
    if [[ -e "$dev" ]]; then
        ok "device: $dev"
    else
        err "missing device: $dev"
    fi
done

VIDEO_NODES=()
for node in /dev/video[0-9]*; do
    [[ -c "$node" ]] || continue
    [[ ${node##*/} =~ ^video[0-9]+$ ]] || continue
    VIDEO_NODES+=("$node")
done

if [[ ${#VIDEO_NODES[@]} -gt 0 ]]; then
    ok "V4L2 candidate nodes: ${VIDEO_NODES[*]}"
else
    # PTZ热插拔版本允许无相机启动，Qt进入等待态并在设备接入后自动恢复。
    warn "no V4L2 /dev/videoN candidate node; PTZ hotplug monitor will wait"
fi

if [[ -x "$BIN" ]]; then
    LDD_OUTPUT="$(
        env -i \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        ldd "$BIN" 2>&1
    )"
    LDD_RC=$?

    if [[ $LDD_RC -ne 0 ]]; then
        err "ldd failed:\n$LDD_OUTPUT"
    elif grep -qE \
        'not found|error while loading|unsupported version|cannot open shared object' \
        <<<"$LDD_OUTPUT"
    then
        err "runtime library check failed:\n$LDD_OUTPUT"
    else
        ok "runtime libraries"
    fi
fi

if [[ -e /usr/lib/librknnrt.so || -e /lib/librknnrt.so ]]; then
    ok "system RKNN runtime"
else
    err "system librknnrt.so not found"
fi

if [[ $FAIL -ne 0 ]]; then
    exit 1
fi

echo "Runtime check passed."
