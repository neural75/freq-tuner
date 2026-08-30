#!/usr/bin/env bash
# Remove freq-tuner completely and restore the original knob behaviour.
#   sudo ./uninstall.sh
set -euo pipefail

UNIT=/etc/systemd/system/freq-tuner.service
RUNNER=/usr/local/bin/freq-tuner-run
BIN=/usr/local/bin/freq-tuner
PROBE=/usr/local/bin/knobprobe
CFG_DIR=/etc/freq-tuner
RUN_DIR=/run/freq-tuner
UUID=freq-tuner@neural75.github.com

if [ "$(id -u)" -ne 0 ]; then
    echo "Root required. Use: sudo $0" >&2
    exit 1
fi

echo "==> Stopping and disabling the service"
systemctl disable --now freq-tuner.service 2>/dev/null || true

echo "==> Removing the systemd unit"
rm -f "$UNIT"

echo "==> Removing the launcher and the binaries"
rm -f "$RUNNER" "$BIN" "$PROBE"

echo "==> Removing the configuration"
rm -rf "$CFG_DIR"

echo "==> Removing the runtime directory"
rm -rf "$RUN_DIR"

echo "==> Removing the GNOME Shell extension"
REAL_USER="${SUDO_USER:-$LOGNAME}"
if [ "$REAL_USER" = "" ] || [ "$REAL_USER" = "root" ]; then
    echo "    cannot determine the session user; extension left installed."
    echo "    remove it manually as your user:"
    echo "      gnome-extensions uninstall $UUID"
else
    runuser -u "$REAL_USER" -- gnome-extensions uninstall "$UUID" \
        >/dev/null 2>&1 || true
    # Drop $UUID from the enabled-extensions list so it cannot come back
    # enabled (the files are gone, but a stale entry is confusing).
    enabled="$(runuser -u "$REAL_USER" -- gsettings get org.gnome.shell \
        enabled-extensions 2>/dev/null \
        | sed -e 's/^@as //' -e 's/[][]//g' -e "s/'//g; s/,//g")"
    if [ -n "$enabled" ]; then
        kept=""
        for entry in $enabled; do
            [ "$entry" = "$UUID" ] || kept="$kept $entry"
        done
        kept="${kept# }"
        if [ -z "$kept" ]; then
            value="[]"
        else
            value="[$(printf '%s' "$kept" \
                | sed "s/ /', '/g; s/^/'/; s/$/'/")]"
        fi
        runuser -u "$REAL_USER" -- gsettings set org.gnome.shell \
            enabled-extensions "$value" >/dev/null 2>&1 || true
    fi
    echo "    extension removed for $REAL_USER"
fi

systemctl daemon-reload

echo
echo "Restored: the knob is back to its original behaviour."
echo "interception-tools is still installed; to remove it:"
echo "    sudo apt remove interception-tools"