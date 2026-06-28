#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$APP_ROOT/config/runtime.env"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "ERROR: setup_display.sh must run as root." >&2
    exit 1
fi

if ! systemctl is-active --quiet lightdm; then
    echo "Starting LightDM..."
    systemctl start lightdm
fi

for _ in $(seq 1 30); do
    [[ -S /tmp/.X11-unix/X0 && -f "$XAUTHORITY_FILE" ]] && break
    sleep 1
done

[[ -S /tmp/.X11-unix/X0 ]] || {
    echo "ERROR: X11 socket /tmp/.X11-unix/X0 was not created." >&2
    exit 1
}

[[ -f "$XAUTHORITY_FILE" ]] || {
    echo "ERROR: Xauthority file not found: $XAUTHORITY_FILE" >&2
    exit 1
}

run_desktop() {
    sudo -u "$DESKTOP_USER" env \
        DISPLAY="$DISPLAY_ID" \
        XAUTHORITY="$XAUTHORITY_FILE" \
        "$@"
}

XRANDR_STATE="$(run_desktop xrandr --query)"

DSI_OK=0
HDMI_OK=0

grep -qE "^${DSI_OUTPUT}[[:space:]]+connected primary 1280x720\\+0\\+0 right" \
    <<<"$XRANDR_STATE" && DSI_OK=1 || true

grep -qE "^${HDMI_OUTPUT}[[:space:]]+connected 1920x1080\\+1280\\+0" \
    <<<"$XRANDR_STATE" && HDMI_OK=1 || true

if [[ $DSI_OK -eq 1 && $HDMI_OK -eq 1 ]]; then
    echo "Display layout already configured; skip xrandr modeset."
else
    echo "Applying display layout..."

    run_desktop xrandr \
        --output "$DSI_OUTPUT" \
        --mode "$DSI_MODE" \
        --rotate "$DSI_ROTATE" \
        --pos "$DSI_POSITION" \
        --primary \
        --output "$HDMI_OUTPUT" \
        --mode "$HDMI_MODE" \
        --rate "$HDMI_RATE" \
        --rotate normal \
        --pos "$HDMI_POSITION"

    sleep 1
fi

run_desktop xinput map-to-output "$TOUCH_DEVICE" "$DSI_OUTPUT"
run_desktop xhost +SI:localuser:root >/dev/null

echo "Display ready: $DSI_OUTPUT + $HDMI_OUTPUT; touch '$TOUCH_DEVICE' -> $DSI_OUTPUT"
