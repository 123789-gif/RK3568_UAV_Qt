#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$APP_ROOT/config/runtime.env"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "ERROR: start.sh must run as root." >&2
    exit 1
fi

FOREGROUND=0

if [[ ${1:-} == "--foreground" ]]; then
    FOREGROUND=1
elif [[ $# -gt 0 ]]; then
    echo "Usage: $0 [--foreground]" >&2
    exit 2
fi

BIN="$APP_ROOT/bin/rk_dual_screen_video"
EXPECTED_EXE="$(readlink -f "$BIN" 2>/dev/null || true)"
PID_DIR="$APP_ROOT/run"
PID_FILE="$PID_DIR/rk_dual_screen_video.pid"
LOG_DIR="$APP_ROOT/logs"
LOG_FILE="$LOG_DIR/rk_dual_screen_video.log"
RUNTIME_DIR=/tmp/rk3568-uav-qt-runtime
TERM_VALUE="${TERM:-xterm-256color}"
mkdir -p "$PID_DIR" "$LOG_DIR" "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

[[ -n "$EXPECTED_EXE" && -x "$EXPECTED_EXE" ]] || {
    echo "ERROR: executable is missing or invalid: $BIN" >&2
    exit 1
}

process_matches_binary() {
    local pid="$1"
    local actual_exe=""

    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    actual_exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    [[ "$actual_exe" == "$EXPECTED_EXE" ]]
}

if [[ -f "$PID_FILE" ]]; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"

    if process_matches_binary "$OLD_PID"; then
        echo "Already running: PID $OLD_PID"
        exit 0
    fi

    if [[ "$OLD_PID" =~ ^[1-9][0-9]*$ ]] && kill -0 "$OLD_PID" 2>/dev/null; then
        echo "WARNING: stale PID file points to another process; it will not be signalled: PID $OLD_PID" >&2
    fi

    rm -f "$PID_FILE"
fi

"$SCRIPT_DIR/load_pcie_driver.sh"
"$SCRIPT_DIR/check_runtime.sh"
"$SCRIPT_DIR/setup_display.sh"

RUN_ENV=(
    env -i
    HOME=/root
    USER=root
    LOGNAME=root
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    TERM="$TERM_VALUE"
    DISPLAY="$DISPLAY_ID"
    XAUTHORITY="$XAUTHORITY_FILE"
    XDG_RUNTIME_DIR="$RUNTIME_DIR"
    QT_QPA_PLATFORM="$QT_PLATFORM"
    QT_XCB_GL_INTEGRATION=none
    UAV_NPU_MODEL_DIR="$APP_ROOT/models"
)

cd "$APP_ROOT"

echo "UAV model directory: $APP_ROOT/models"

if [[ $FOREGROUND -eq 1 ]]; then
    echo "Starting in foreground; log: $LOG_FILE"

    "${RUN_ENV[@]}" "$BIN" 2>&1 | tee -a "$LOG_FILE"
    exit "${PIPESTATUS[0]}"
fi

nohup "${RUN_ENV[@]}" "$BIN" >>"$LOG_FILE" 2>&1 &
PID=$!

PID_TMP="$PID_FILE.tmp.$$"
printf '%s\n' "$PID" > "$PID_TMP"
chmod 600 "$PID_TMP"
mv -f "$PID_TMP" "$PID_FILE"
sleep 2

if process_matches_binary "$PID"; then
    echo "Started rk_dual_screen_video: PID $PID"
    echo "Log: $LOG_FILE"
else
    rm -f "$PID_FILE"
    echo "ERROR: process exited during startup. Last log lines:" >&2
    tail -n 100 "$LOG_FILE" >&2 || true
    exit 1
fi
