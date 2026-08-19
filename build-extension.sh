#!/usr/bin/env bash
# Build the freq-tuner extension bundle (does not install).
set -euo pipefail

UUID=freq-tuner@neural75.github.com
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$BASE_DIR"
gnome-extensions pack . --force --out-dir "$BASE_DIR"

echo "Built $BASE_DIR/$UUID.shell-extension.zip"