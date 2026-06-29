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
PID_DIR="$APP_ROOT/run"
PID_FILE="$PID_DIR/rk_dual_screen_video.pid"
LOG_DIR="$APP_ROOT/logs"
LOG_FILE="$LOG_DIR/rk_dual_screen_video.log"
RUNTIME_DIR=/tmp/rk3568-ptz-qt-runtime
TERM_VALUE="${TERM:-xterm-256color}"

mkdir -p "$PID_DIR" "$LOG_DIR" "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

if [[ -f "$PID_FILE" ]]; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"

    if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Already running: PID $OLD_PID"
        exit 0
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
    PTZ_NPU_MODEL_DIR="$APP_ROOT/models"
)

cd "$APP_ROOT"

if [[ $FOREGROUND -eq 1 ]]; then
    echo "Starting in foreground; log: $LOG_FILE"

    "${RUN_ENV[@]}" "$BIN" 2>&1 | tee -a "$LOG_FILE"
    exit "${PIPESTATUS[0]}"
fi

nohup "${RUN_ENV[@]}" "$BIN" >>"$LOG_FILE" 2>&1 &
PID=$!

echo "$PID" > "$PID_FILE"
sleep 2

if kill -0 "$PID" 2>/dev/null; then
    echo "Started rk_dual_screen_video: PID $PID"
    echo "Log: $LOG_FILE"
else
    rm -f "$PID_FILE"
    echo "ERROR: process exited during startup. Last log lines:" >&2
    tail -n 100 "$LOG_FILE" >&2 || true
    exit 1
fi
