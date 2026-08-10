#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$APP_ROOT/bin/rk_dual_screen_video"
EXPECTED_EXE="$(readlink -f "$BIN" 2>/dev/null || true)"
PID_FILE="$APP_ROOT/run/rk_dual_screen_video.pid"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "ERROR: stop.sh must run as root." >&2
    exit 1
fi

[[ -n "$EXPECTED_EXE" ]] || {
    echo "ERROR: cannot resolve executable path: $BIN" >&2
    exit 1
}

process_matches_binary() {
    local pid="$1"
    local actual_exe=""

    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    actual_exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    [[ "$actual_exe" == "$EXPECTED_EXE" ]]
}

declare -A target_pids=()
PID=""
if [[ -f "$PID_FILE" ]]; then
    PID="$(cat "$PID_FILE" 2>/dev/null || true)"
    if process_matches_binary "$PID"; then
        target_pids["$PID"]=1
    elif [[ "$PID" =~ ^[1-9][0-9]*$ ]] && kill -0 "$PID" 2>/dev/null; then
        echo "WARNING: PID file points to another process; it will not be signalled: PID $PID" >&2
    fi
fi

# 前台启动不会写 PID 文件；通过 /proc/exe 精确匹配当前发布目录中的 ELF。
for proc_exe in /proc/[0-9]*/exe; do
    [[ -e "$proc_exe" ]] || continue
    candidate_pid="${proc_exe#/proc/}"
    candidate_pid="${candidate_pid%/exe}"
    if process_matches_binary "$candidate_pid"; then
        target_pids["$candidate_pid"]=1
    fi
done

if ((${#target_pids[@]} == 0)); then
    rm -f "$PID_FILE"
    echo "No process is running from $EXPECTED_EXE"
    exit 0
fi

for candidate_pid in "${!target_pids[@]}"; do
    if process_matches_binary "$candidate_pid"; then
        kill -TERM "$candidate_pid"
    fi
done

for _ in $(seq 1 50); do
    active_count=0
    for candidate_pid in "${!target_pids[@]}"; do
        if process_matches_binary "$candidate_pid"; then
            active_count=$((active_count + 1))
        fi
    done
    ((active_count == 0)) && break
    sleep 0.2
done

forced_count=0
for candidate_pid in "${!target_pids[@]}"; do
    if process_matches_binary "$candidate_pid"; then
        echo "WARNING: graceful shutdown timed out; sending SIGKILL to PID $candidate_pid" >&2
        kill -KILL "$candidate_pid"
        forced_count=$((forced_count + 1))
    fi
done

rm -f "$PID_FILE"
if ((forced_count == 0)); then
    echo "Stopped ${#target_pids[@]} process(es) gracefully."
else
    echo "Stopped ${#target_pids[@]} process(es); $forced_count required SIGKILL."
fi
