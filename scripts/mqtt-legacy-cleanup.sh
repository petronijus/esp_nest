#!/usr/bin/env bash
# Clear the retained MQTT topics the native firmware (v2.x) left on the
# broker, so Home Assistant drops its MQTT device ("Nest Hub") once the knob
# runs ESPHome and reaches HA through the native API.
#
#   scripts/mqtt-legacy-cleanup.sh <device-id>            dry run: list what would be cleared
#   scripts/mqtt-legacy-cleanup.sh <device-id> --apply    clear it, then verify
#
# <device-id> is the native firmware's id, "esp_" + the last three MAC bytes
# (e.g. esp_24ff10; its discovery topics are homeassistant/<component>/<id>/…).
# The topics are found on the broker, not assumed: native published 9
# retained discovery configs (4 sensors, 1 button, 3 numbers, 1 switch) and
# a retained espnest/<id>/avail (its LWT). Clearing a retained topic means
# publishing an empty retained message to it.
#
# Run it only after the device runs ESPHome: native republishes everything
# on every MQTT (re)connect. Broker and credentials come from
# esphome/secrets.yaml (mqtt_broker, mqtt_username, mqtt_password).
set -euo pipefail
# shellcheck source=scripts/lib/migration.sh
. "$(dirname "$0")/lib/migration.sh"

ID="${1:-}"
APPLY=0
[ "${2:-}" = "--apply" ] && APPLY=1
[[ "$ID" =~ ^esp_[0-9a-f]{6}$ ]] || { say "usage: $0 <device-id like esp_24ff10> [--apply]" >&2; exit 2; }
if ! command -v mosquitto_sub >/dev/null || ! command -v mosquitto_pub >/dev/null; then
  fail "mosquitto_sub / mosquitto_pub missing (package mosquitto or mosquitto-clients)"
fi

# Credentials go into option files (mosquitto_* read $XDG_CONFIG_HOME/<tool>),
# never onto a command line where `ps` would show them.
make_private_tmpdir
TMP="$PRIVATE_TMP"
{
  printf -- '-h %s\n-u %s\n-P %s\n' "$(secret mqtt_broker)" "$(secret mqtt_username)" "$(secret mqtt_password)"
} >"$TMP/mosquitto_sub"
cp "$TMP/mosquitto_sub" "$TMP/mosquitto_pub"
chmod 600 "$TMP"/mosquitto_*
export XDG_CONFIG_HOME="$TMP"

# retained_topics: every retained topic of the native device, one per line.
retained_topics() {
  mosquitto_sub --retained-only -W 3 -F '%t' \
    -t "homeassistant/+/$ID/+/config" -t "espnest/$ID/#" 2>/dev/null | sort -u || true
}

step "retained topics of $ID on the broker"
mapfile -t topics < <(retained_topics)
configs=0
for t in "${topics[@]}"; do
  say "    $t"
  [[ "$t" == homeassistant/* ]] && configs=$((configs + 1))
done
ok "${#topics[@]} retained topic(s): $configs discovery config(s), $((${#topics[@]} - configs)) other(s)"
[ ${#topics[@]} -gt 0 ] || { ok "nothing to clear"; exit 0; }

if [ "$APPLY" = 0 ]; then
  say ""
  say "Dry run. Re-run with --apply to clear them."
  exit 0
fi

step "clearing (empty retained message per topic)"
for t in "${topics[@]}"; do
  mosquitto_pub -r -n -q 1 -t "$t"
  ok "cleared $t"
done

step "verify"
mapfile -t left < <(retained_topics)
[ ${#left[@]} -eq 0 ] || fail "still retained: ${left[*]} — is the native firmware still running?"
ok "no retained topics of $ID left; Home Assistant removes the MQTT device and its entities"
