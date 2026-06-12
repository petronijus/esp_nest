#!/usr/bin/env bash
# Download a released OTA image and push it to a device.
# Usage: ./scripts/ota-update.sh <tag> <device-ip-or-host>
set -euo pipefail
TAG=${1:?usage: ota-update.sh <tag> <device-host>}
HOST=${2:?usage: ota-update.sh <tag> <device-host>}

curl -sf --max-time 5 "http://$HOST/" | grep -q "ESP Nest OTA Update" \
  || { echo "error: http://$HOST/ does not look like an ESP Nest OTA endpoint" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
gh release download "$TAG" --pattern '*-ota.bin' --dir "$TMP"
BIN=$(echo "$TMP"/*-ota.bin)

echo "Flashing $(basename "$BIN") → http://$HOST/update"
curl -F "firmware=@$BIN" "http://$HOST/update"
