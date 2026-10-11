#!/usr/bin/env bash
# Move a knob from the native firmware (v2.x) to ESPHome, over the network.
#
#   scripts/migrate-to-esphome.sh <device-ip> [<firmware.ota.bin>]
#
# The ESPHome image keeps the device's bootloader and partition table (the
# package's partitions.csv is the native one), so it goes in through the
# native, unauthenticated `POST /update`. Default image: the dev build,
# esphome/.esphome/build/esp-nest/build/firmware.ota.bin — build it from a
# release tag first:
#
#   git checkout vX.Y.Z && esphome compile esphome/esp-nest-dev.yaml
#
# Refuses anything but an ESPHome *app* image (a factory image would be
# accepted by the device's first-byte check and brick the slot) and a target
# that does not answer as the native firmware. Afterwards:
#   scripts/mqtt-legacy-cleanup.sh <device-id> --apply, then adopt the device
#   in Home Assistant (ESPHome integration, key esp_nest_api_key).
# The way back: scripts/rollback-to-native.sh.
set -euo pipefail
# shellcheck source=scripts/lib/migration.sh
. "$(dirname "$0")/lib/migration.sh"

IP="${1:-}"
IMAGE="${2:-$REPO/esphome/.esphome/build/esp-nest/build/firmware.ota.bin}"
[ -n "$IP" ] || { say "usage: $0 <device-ip> [<firmware.ota.bin>]" >&2; exit 2; }

step "image $IMAGE"
[ -f "$IMAGE" ] || fail "no such file"
info="$(app_image_info "$IMAGE" 2>&1)" || fail "$info"
read -r project version <<<"$info"
[ "$project" = "esp-nest" ] || fail "app project '$project' is not the ESPHome build (esp-nest)"
size=$(wc -c <"$IMAGE")
[ "$size" -le $((0x400000)) ] || fail "image is $size bytes, larger than the 4 MB OTA slot"
ok "ESPHome app image ($project, ESPHome $version, $size bytes)"

step "target http://$IP/"
curl -sf --max-time 5 "http://$IP/" | grep -q "ESP Nest OTA Update" \
  || fail "http://$IP/ does not answer as the native ESP Nest firmware (already migrated?)"
ok "native firmware answers"

step "upload (the device restarts into ESPHome)"
reply="$(curl -s --max-time 180 -F "firmware=@$IMAGE" "http://$IP/update" | sed 's/<[^>]*>/ /g' | tr -s ' ')"
case "$reply" in
  *Successful*) ok "native OTA accepted the image" ;;
  *) fail "upload failed:${reply:- no reply}" ;;
esac

step "wait for ESPHome"
sleep 5
wait_for_http "http://$IP/" 120 || fail "no answer from http://$IP/ within 2 minutes — check the serial log or the fallback AP"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://$IP/")"
[ "$code" = 401 ] || fail "http://$IP/ answered $code, expected 401 (ESPHome web server with basic auth)"
ok "ESPHome web server answers (basic auth)"

say ""
say "Next:"
say "  1. scripts/mqtt-legacy-cleanup.sh <device-id>            (dry run, then --apply)"
say "  2. Home Assistant → ESPHome integration → add $IP with the API key (esp_nest_api_key)"
