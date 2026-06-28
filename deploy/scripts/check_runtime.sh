#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$APP_ROOT/bin/rk_dual_screen_video"
FAIL=0

ok() {
    printf '[OK] %s\n' "$*"
}

err() {
    printf '[FAIL] %s\n' "$*" >&2
    FAIL=1
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

for dev in \
    /dev/pango_pci_driver \
    /dev/dri/card0 \
    /dev/video0 \
    /dev/ttyS0 \
    /dev/mem
do
    if [[ -e "$dev" ]]; then
        ok "device: $dev"
    else
        err "missing device: $dev"
    fi
done

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
