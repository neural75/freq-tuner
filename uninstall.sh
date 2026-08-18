#!/usr/bin/env bash
# Remove freq-tuner completely and restore the original knob behaviour.
#   sudo ./uninstall.sh
set -euo pipefail

UNIT=/etc/systemd/system/freq-tuner.service
RUNNER=/usr/local/bin/freq-tuner-run
BIN=/usr/local/bin/freq-tuner
PROBE=/usr/local/bin/knobprobe
CFG_DIR=/etc/freq-tuner

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

systemctl daemon-reload

echo
echo "Restored: the knob is back to its original behaviour."
echo "interception-tools is still installed; to remove it:"
echo "    sudo apt remove interception-tools"