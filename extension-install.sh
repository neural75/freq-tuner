#!/usr/bin/env bash
# Install the freq-tuner extension for the current user.
#   ./extension-install.sh   (no root needed)
set -euo pipefail

UUID=freq-tuner@neural75.github.com
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"

mkdir -p "$DEST"
install -m 644 "$BASE_DIR/extension.js" "$DEST/extension.js"
install -m 644 "$BASE_DIR/metadata.json" "$DEST/metadata.json"

echo "Installed to $DEST"
echo
echo "Enable it (a session restart picks up the new extension):"
echo "    gnome-extensions enable $UUID"