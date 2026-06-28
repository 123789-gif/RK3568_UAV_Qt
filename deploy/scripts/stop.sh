#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PID_FILE="$APP_ROOT/run/rk_dual_screen_video.pid"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "ERROR: stop.sh must run as root." >&2
    exit 1
fi

PID=""
if [[ -f "$PID_FILE" ]]; then
    PID="$(cat "$PID_FILE" 2>/dev/null || true)"
fi

if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID"
    for _ in $(seq 1 30); do
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.2
    done
    if kill -0 "$PID" 2>/dev/null; then
        kill -KILL "$PID"
    fi
    echo "Stopped PID $PID"
else
    pkill -f "/bin/rk_dual_screen_video" 2>/dev/null || true
    echo "No active PID file; cleared any matching process."
fi

rm -f "$PID_FILE"
