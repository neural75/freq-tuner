#!/usr/bin/env bash
# freq-tuner installer.
#
#   sudo ./install.sh [gqrx_host] [gqrx_port]
#
# Detects the VIRTUAL volume-knob device that the keyboard grabber
# publishes (a uinput clone) and wires freq-tuner into the interception
# pipeline:
#
#     interception -g $DEVICE | freq-tuner | uinput -d $DEVICE
#
# Detection never grabs anything: each /dev/input/event* node is probed
# READ-ONLY (EVIOCGBIT via knobprobe) for the knob keys, physical devices
# are filtered out by only accepting uinput (virtual) devices, and the
# SOURCE clone is picked automatically: it is the device that carries the
# physical keyboard's vendor id, while the clones a pipeline creates with
# its own uinput stage have none.
#
# Both arguments are optional (defaults: 127.0.0.1:7356).
#
# The service is started but NOT enabled at boot: a reboot restores the
# original configuration.
set -euo pipefail

GQRX_HOST="${1:-127.0.0.1}"
GQRX_PORT="${2:-7356}"

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BIN=/usr/local/bin/freq-tuner
PROBE=/usr/local/bin/knobprobe
RUNNER=/usr/local/bin/freq-tuner-run
CFG_DIR=/etc/freq-tuner
CFG="$CFG_DIR/config"
UNIT=/etc/systemd/system/freq-tuner.service
SERVICE=freq-tuner.service
FOCUS_SOCK=/run/freq-tuner/focus.sock

# ---------------------------------------------------------------- detection

device_name() {
    local dev
    dev="$(readlink -f "$1")"
    cat "/sys/class/input/$(basename "$dev")/device/name" 2>/dev/null \
        || echo "$1"
}

# Print "NAME|DEVICE|VENDOR" for every VIRTUAL (uinput) event device that
# exposes the volume-knob keys (KEY_MUTE / KEY_VOLUMEDOWN / KEY_VOLUMEUP).
# Read-only probing, no grab. VENDOR is the device's sysfs vendor id: the
# source clone that the grabber publishes copies the physical keyboard's
# id, while a pipeline's own uinput output clone has none.
detect_knob_devices() {
    local dev sys_dev name vendor

    for dev in /dev/input/event*; do
        [ -e "$dev" ] || continue
        # only uinput devices: the clone the grabber publishes
        sys_dev="$(readlink -f "/sys/class/input/$(basename "$dev")/device" 2>/dev/null || true)"
        [[ "$sys_dev" == */devices/virtual/* ]] || continue
        if "$BASE_DIR/knobprobe" "$dev" >/dev/null 2>&1; then
            name="$(device_name "$dev")"
            vendor="$(cat "/sys/class/input/$(basename "$dev")/device/id/vendor" 2>/dev/null || true)"
            printf '%s|%s|%s\n' "$name" "$dev" "$vendor"
        fi
    done
}

choose_device() {
    local -a names=() links=() vendors=() snames=() slinks=()
    local sel n l v i

    while IFS='|' read -r n l v; do
        names+=("$n"); links+=("$l"); vendors+=("$v")
    done < <(detect_knob_devices)

    if [ "${#links[@]}" -eq 0 ]; then
        echo "ERROR: no virtual knob device found." >&2
        echo "       Is the keyboard grabber running? It publishes the" >&2
        echo "       virtual knob device that freq-tuner grabs." >&2
        exit 1
    fi

    # Prefer the source clone, which carries the physical keyboard's
    # vendor id. Pipeline output clones (no id) are filtered out.
    for i in "${!links[@]}"; do
        if [ -n "${vendors[$i]}" ]; then
            snames+=("${names[$i]}")
            slinks+=("${links[$i]}")
        fi
    done

    if [ "${#slinks[@]}" -eq 1 ]; then
        DEVICE="${slinks[0]}"
        DEVNAME="${snames[0]}"
        echo "    source clone : $DEVICE"
        return
    fi

    # No unique source clone: fall back to all knob devices.
    if [ "${#slinks[@]}" -gt 1 ]; then
        names=("${snames[@]}")
        links=("${slinks[@]}")
    fi

    echo
    echo "Multiple virtual knob devices found:"
    for i in "${!links[@]}"; do
        printf '  [%d] %s\n      %s\n' "$((i + 1))" "${names[$i]}" "${links[$i]}"
    done
    echo
    echo "Pick the SOURCE clone (the one the keyboard grabber publishes,"
    echo "not a pipeline's own output clone)."
    while :; do
        printf 'Select [1-%d, q]: ' "${#links[@]}"
        read -r sel
        case "$sel" in
            q) echo "Aborted."; exit 1 ;;
            *[!0-9]*) : ;;
            *)
                if [ "$sel" -ge 1 ] 2>/dev/null && [ "$sel" -le "${#links[@]}" ]; then
                    DEVICE="${links[$((sel - 1))]}"
                    DEVNAME="${names[$((sel - 1))]}"
                    return
                fi
                ;;
        esac
        echo "  invalid choice."
    done
}

# --------------------------------------------------------------------- main

if [ "$(id -u)" -ne 0 ]; then
    echo "Root required. Use: sudo $0 [gqrx_host] [gqrx_port]" >&2
    exit 1
fi

echo "==> interception-tools"
if command -v interception >/dev/null 2>&1 \
    && command -v uinput >/dev/null 2>&1; then
    echo "    already present"
else
    apt-get install -y interception-tools
fi
INTERCEPTION="$(command -v interception)"
UINPUT="$(command -v uinput)"

echo "==> Checking the binaries"
# We do not build here: building as root would leave root-owned files in
# the user's tree. Build as a normal user first:  cd "$BASE_DIR" && make
if [ ! -x "$BASE_DIR/freq-tuner" ]; then
    echo "ERROR: $BASE_DIR/freq-tuner does not exist." >&2
    echo "       Build first, as a normal user:" >&2
    echo "         cd $BASE_DIR && make" >&2
    exit 1
fi
if [ ! -x "$BASE_DIR/knobprobe" ]; then
    echo "ERROR: $BASE_DIR/knobprobe does not exist." >&2
    echo "       Build first, as a normal user:" >&2
    echo "         cd $BASE_DIR && make" >&2
    exit 1
fi
if [ "$BASE_DIR/src/main.c" -nt "$BASE_DIR/freq-tuner" ]; then
    echo "ERROR: src/main.c is newer than the binary." >&2
    echo "       Rebuild as a normal user:  cd $BASE_DIR && make" >&2
    exit 1
fi
echo "    $BASE_DIR/freq-tuner"
echo "    $BASE_DIR/knobprobe"

choose_device

echo
echo "==> Routing      : $DEVNAME"
echo "    knob device : $DEVICE"
echo "    gqrx host   : $GQRX_HOST"
echo "    gqrx port   : $GQRX_PORT"
echo "    focus socket: $FOCUS_SOCK"
echo

printf 'Install freq-tuner for "%s"? [y/N]: ' "$DEVNAME"
read -r ans
case "$ans" in
    y|Y|yes|YES) : ;;
    *) echo "Aborted."; exit 1 ;;
esac

echo "==> Installing the binaries"
install -m 755 "$BASE_DIR/freq-tuner" "$BIN"
install -m 755 "$BASE_DIR/knobprobe" "$PROBE"
"$BIN" --version

echo "==> Writing the configuration"
mkdir -p "$CFG_DIR"
cat > "$CFG" <<EOF
# freq-tuner configuration - generated by install.sh on $(date -Iseconds).
# Change a value and restart the service to apply it.
DEVICE="$DEVICE"
DEVNAME="$DEVNAME"
GQRX_HOST="$GQRX_HOST"
GQRX_PORT=$GQRX_PORT
FOCUS_SOCK="$FOCUS_SOCK"
VERBOSE=0
EOF

echo "==> Installing the launcher"
cat > "$RUNNER" <<EOF
#!/bin/sh
# Launcher for the freq-tuner pipeline. Reads /etc/freq-tuner/config.
. $CFG
ARGS="--gqrx-host \$GQRX_HOST --gqrx-port \$GQRX_PORT --focus-sock \$FOCUS_SOCK"
[ "\$VERBOSE" = 1 ] && ARGS="\$ARGS --verbose"
$INTERCEPTION -g "\$DEVICE" | $BIN \$ARGS | $UINPUT -d "\$DEVICE"
EOF
chmod 755 "$RUNNER"

echo "==> Writing the systemd unit"
cat > "$UNIT" <<EOF
# freq-tuner - rotary tuning router.
#
# Grabs the VIRTUAL volume-knob device that the keyboard grabber publishes
# (never the physical keyboard). Sleeps 1s before grabbing so a manually
# started/restarted service cannot land mid-keystroke and split a key
# across the grab boundary.
[Unit]
Description=freq-tuner rotary tuning router

[Service]
Type=simple
ExecStartPre=/bin/mkdir -p /run/freq-tuner
ExecStartPre=/bin/sleep 1
ExecStart=$RUNNER
Restart=on-failure
RestartSec=2
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=20
LimitMEMLOCK=infinity
LimitRTPRIO=20

[Install]
WantedBy=multi-user.target
EOF

echo "==> Starting (NOT enabled at boot)"
systemctl daemon-reload
systemctl restart "$SERVICE"
sleep 2

if ! systemctl is-active --quiet "$SERVICE"; then
    echo "ERROR: $SERVICE did not start." >&2
    systemctl status "$SERVICE" --no-pager -l | tail -20 >&2
    exit 1
fi
echo "    $SERVICE ACTIVE"

cat <<EOF

==================================================
 DONE. The router is active NOW.
==================================================

 Check        : $BASE_DIR/status.sh
 Disable now  : sudo systemctl stop $SERVICE
 Make it stick: sudo systemctl enable $SERVICE
 Remove all   : sudo $BASE_DIR/uninstall.sh

 It is NOT enabled at boot: if anything goes wrong, reboot.

 SAFETY NET: the knob is only swallowed while gqrx is focused; at all
 other times every event passes through unchanged.
EOF