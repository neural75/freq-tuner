#!/usr/bin/env bash
# Status of the freq-tuner router.
CFG=/etc/freq-tuner/config
UNIT=/etc/systemd/system/freq-tuner.service
BIN=/usr/local/bin/freq-tuner
SERVICE=freq-tuner.service

echo "=============================================="
echo " FREQ-TUNER STATUS"
echo "=============================================="

if [ -f "$CFG" ]; then
    . "$CFG"
    echo "Config     : present"
    echo "Knob device: $DEVICE"
    [ -n "${DEVNAME:-}" ] && echo "             : $DEVNAME"
    echo "gqrx       : $GQRX_HOST:$GQRX_PORT"
    echo "Focus sock : $FOCUS_SOCK"
    if [ "${VERBOSE:-0}" = 1 ]; then echo "Verbose    : on"; else echo "Verbose    : off"; fi
else
    echo "Config     : MISSING"
fi

[ -f "$UNIT" ] && echo "Unit       : present" || echo "Unit       : missing"

if [ -x "$BIN" ]; then
    echo "Binary     : $("$BIN" --version)"
else
    echo "Binary     : MISSING"
fi

systemctl is-active --quiet "$SERVICE" 2>/dev/null \
    && echo "Service    : ACTIVE" || echo "Service    : stopped"
systemctl is-enabled "$SERVICE" >/dev/null 2>&1 \
    && echo "At boot    : enabled" || echo "At boot    : NOT enabled"

if [ -f "$UNIT" ]; then
    echo
    echo "--- running pipeline ---"
    ps -o pid,cls,rtprio,ni,comm,args -C sh -C interception -C freq-tuner -C uinput 2>/dev/null \
        | grep -i 'freq-tuner\|interception\|uinput' || echo "(no pipeline running)"

    echo
    echo "--- systemctl status ---"
    systemctl status "$SERVICE" --no-pager -l | grep -E 'Active:|Status:' || true
fi