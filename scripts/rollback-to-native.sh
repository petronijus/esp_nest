#!/usr/bin/env bash
# Put a knob back on the native firmware (v2.x) from ESPHome, over the network.
#
#   scripts/rollback-to-native.sh <device-ip> [<native.bin>]
#
# Default image: esphome/fallback/native-v2.0.1.bin — the LOCAL native build
# with real credentials (gitignored; see esphome/README.md). Never a CI
# release asset: those carry the placeholder WiFi ("myssid"), so the device
# would come back offline, with no OTA path and no USB flashing on Linux.
# This script refuses such an image.
#
# ESPHome's web server takes the image at `POST /update` (basic auth from
# esphome/secrets.yaml: esp_nest_web_username / esp_nest_web_password).
set -euo pipefail
# shellcheck source=scripts/lib/migration.sh
. "$(dirname "$0")/lib/migration.sh"

IP="${1:-}"
IMAGE="${2:-$REPO/esphome/fallback/native-v2.0.1.bin}"
[ -n "$IP" ] || { say "usage: $0 <device-ip> [<native.bin>]" >&2; exit 2; }

step "image $IMAGE"
[ -f "$IMAGE" ] || fail "no such file (build the native firmware locally with secrets.defaults)"
info="$(app_image_info "$IMAGE" 2>&1)" || fail "$info"
read -r project version <<<"$info"
[ "$project" = "ZX2D10GE01R" ] || fail "app project '$project' is not the native firmware (ZX2D10GE01R)"
if grep -aq 'myssid' "$IMAGE"; then
  fail "image carries the placeholder WiFi 'myssid' (a CI build without credentials) — refusing"
fi
ok "native app image v$version with real credentials"

make_private_tmpdir
# curl reads the credentials from a file (-K), not from its command line.
printf 'user = "%s:%s"\n' "$(secret esp_nest_web_username)" "$(secret esp_nest_web_password)" >"$PRIVATE_TMP/auth"
chmod 600 "$PRIVATE_TMP/auth"

step "target http://$IP/"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -K "$PRIVATE_TMP/auth" "http://$IP/")"
[ "$code" = 200 ] || fail "http://$IP/ answered $code to the ESPHome web credentials (not ESPHome, or wrong secrets)"
ok "ESPHome web server answers"

say ""
say "Before the native firmware connects to MQTT it republishes its discovery."
say "Delete the ESPHome device in Home Assistant first, or its entity ids stay"
say "taken and the native ones come back with _2 suffixes."
read -r -p "ESPHome device deleted in Home Assistant? [y/N] " answer
[ "$answer" = y ] || fail "aborted"

step "upload (the device restarts into the native firmware)"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 180 -K "$PRIVATE_TMP/auth" \
  -F "firmware=@$IMAGE" "http://$IP/update")"
[ "$code" = 200 ] || fail "upload answered HTTP $code"
ok "ESPHome accepted the image"

step "wait for the native firmware"
sleep 5
for _ in $(seq 60); do
  if curl -sf --max-time 2 "http://$IP/" | grep -q "ESP Nest OTA Update"; then
    ok "native firmware answers at http://$IP/"
    exit 0
  fi
  sleep 2
done
fail "the native firmware did not answer within 2 minutes — check the serial log"
