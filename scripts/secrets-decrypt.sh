#!/usr/bin/env bash
# Decrypt the SOPS-encrypted secrets of the private overlay into their
# gitignored plaintext files:
#
#   private/config/secrets.defaults.sops      -> secrets.defaults      (native ESP-IDF build)
#   private/config/esphome-secrets.yaml.sops  -> esphome/secrets.yaml  (ESPHome build)
#
# The age private key lives in 1Password (document "SOPS age key (dev repos)")
# and is fetched at runtime — it never touches disk in plaintext. A key that
# is already exported as SOPS_AGE_KEY is used as is. Requires the private
# overlay (esp_nest-private) cloned into ./private.
#
# Every pair whose .sops file exists is decrypted; a missing one (an overlay
# that has not been pulled yet) does not block the others. The script names
# the missing ones and exits non-zero at the end.
#
# A plaintext file that differs from the freshly decrypted content (an older
# decrypt, or local edits never folded back with secrets-edit.sh) is moved to
# the trash before it is replaced, never silently overwritten. Every plaintext
# target ends up mode 600, whether it was rewritten or already up to date.
#
# Until the native firmware is removed, the WiFi/MQTT credentials and the
# external MQTT topics exist twice: in secrets.defaults for the native build,
# and in esphome/secrets.yaml plus private/esphome/substitutions.yaml for the
# ESPHome build. The script warns, by key name only, when the copies have
# drifted apart.
set -euo pipefail
cd "$(dirname "$0")/.."
umask 077

KEY_ITEM="SOPS age key (dev repos)"
# encrypted source : plaintext target
PAIRS=(
  "private/config/secrets.defaults.sops:secrets.defaults"
  "private/config/esphome-secrets.yaml.sops:esphome/secrets.yaml"
)
NATIVE_PLAIN="secrets.defaults"
ESPHOME_PLAIN="esphome/secrets.yaml"
SUBSTITUTIONS="private/esphome/substitutions.yaml"
KCONFIG="main/Kconfig.projbuild"

command -v sops >/dev/null || { echo "sops not found — 'brew install sops age'"; exit 1; }

present=()
missing=()
for pair in "${PAIRS[@]}"; do
  if [ -f "${pair%%:*}" ]; then
    present+=("$pair")
  else
    missing+=("${pair%%:*}")
  fi
done

report_missing() {
  echo "Missing ${missing[*]} — clone the private overlay into ./private first"
  echo "(or pull it: cd private && git pull), then re-run."
}

if [ ${#present[@]} -eq 0 ]; then
  report_missing
  exit 1
fi

if [ -z "${SOPS_AGE_KEY:-}" ]; then
  command -v op >/dev/null || { echo "op (1Password CLI) not found"; exit 1; }
  key="$(op document get "$KEY_ITEM" 2>/dev/null)" \
    || { echo "Could not read '$KEY_ITEM' from 1Password."; exit 1; }
  SOPS_AGE_KEY="$(printf '%s\n' "$key" | grep '^AGE-SECRET-KEY')" \
    || { echo "'$KEY_ITEM' holds no AGE-SECRET-KEY line."; exit 1; }
  unset key
fi
export SOPS_AGE_KEY

# Move a file to the trash (freedesktop or macOS); fail if no trash exists.
trash_file() {
  if command -v gio >/dev/null; then
    gio trash -- "$1"
  elif command -v trash-put >/dev/null; then
    trash-put -- "$1"
  elif command -v trash >/dev/null; then
    trash "$1"
  else
    echo "No trash command (gio, trash-put, trash) to move the previous $1 to;"
    echo "move it away yourself and re-run."
    return 1
  fi
}

# Plaintext only ever exists in this private directory and in the targets.
tmp="$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/esp_nest-secrets.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

native_changed=false
for pair in "${present[@]}"; do
  enc="${pair%%:*}"
  out="${pair#*:}"
  sops --input-type binary --output-type binary -d "$enc" > "$tmp/plain"
  if [ -f "$out" ] && cmp -s "$tmp/plain" "$out"; then
    echo "$out is up to date."
  else
    if [ -e "$out" ]; then
      trash_file "$out" || {
        echo "Could not move the previous $out (it differs from $enc) to the trash;"
        echo "it was left untouched."
        exit 1
      }
      echo "The previous $out differed from $enc and was moved to the trash."
    fi
    mv "$tmp/plain" "$out"
    echo "Wrote $out (gitignored)."
    if [ "$out" = "$NATIVE_PLAIN" ]; then
      native_changed=true
    fi
  fi
done

# umask only covers files this script creates; an older plaintext copy (or
# one whose .sops is missing in this run) may still be group/world readable.
for pair in "${PAIRS[@]}"; do
  out="${pair#*:}"
  if [ -f "$out" ]; then
    chmod 600 "$out"
  fi
done

if $native_changed; then
  echo "Re-run 'idf.py reconfigure build' to pick up $NATIVE_PLAIN."
fi

# Drift check between the native and the ESPHome copies. It reports key
# names only, never values. YAML parsing needs PyYAML: the system Python's,
# or the one in ESPHome's own environment.
drift_check() {
  if [ ! -f "$NATIVE_PLAIN" ] || [ ! -f "$ESPHOME_PLAIN" ]; then
    echo "WARNING: $NATIVE_PLAIN or $ESPHOME_PLAIN does not exist;"
    echo "         the drift check between the native and ESPHome copies did not run."
    return 0
  fi
  local yaml_python="" candidate
  if python3 -I -c 'import yaml' 2>/dev/null; then
    yaml_python=python3
  elif command -v esphome >/dev/null; then
    candidate="$(dirname "$(readlink -f "$(command -v esphome)")")/python"
    if [ -x "$candidate" ] && "$candidate" -I -c 'import yaml' 2>/dev/null; then
      yaml_python="$candidate"
    fi
  fi
  if [ -z "$yaml_python" ]; then
    echo "WARNING: no Python with PyYAML (install ESPHome, see esphome/README.md);"
    echo "         the drift check between the native and ESPHome copies did not run."
    return 0
  fi

  "$yaml_python" -I - "$NATIVE_PLAIN" "$KCONFIG" "$ESPHOME_PLAIN" "$SUBSTITUTIONS" <<'EOF'
import os
import re
import sys
import urllib.parse

import yaml

native_path, kconfig_path, esphome_path, substitutions_path = sys.argv[1:5]

# ESPHome name -> native sdkconfig symbol. The MQTT broker is compared
# separately: the native side holds a URI, the ESPHome side a host name.
CREDENTIALS = {
    "wifi_ssid": "CONFIG_WIFI_SSID",
    "wifi_password": "CONFIG_WIFI_PASSWORD",
    "mqtt_username": "CONFIG_MQTT_USERNAME",
    "mqtt_password": "CONFIG_MQTT_PASSWORD",
}
TOPICS = {
    "topic_lights": "CONFIG_TOPIC_LIGHTS",
    "topic_fan": "CONFIG_TOPIC_FAN",
    "topic_stereo_vol": "CONFIG_TOPIC_STEREO_VOL",
    "topic_blinds_prefix": "CONFIG_TOPIC_BLINDS_PREFIX",
    "blinds_group_a": "CONFIG_TOPIC_BLINDS_GROUP_A",
    "blinds_group_b": "CONFIG_TOPIC_BLINDS_GROUP_B",
    "blinds_label_a": "CONFIG_TOPIC_BLINDS_LABEL_A",
    "blinds_label_b": "CONFIG_TOPIC_BLINDS_LABEL_B",
}
# What the ESPHome build always uses: plain TCP on the default port
# (esphome/secrets.example.yaml: host name only, no scheme, no port).
ESPHOME_MQTT_SCHEME = "mqtt"
ESPHOME_MQTT_PORTS = (None, 1883)


def sdkconfig_value(raw):
    """Value of an sdkconfig/Kconfig default: a quoted string with
    \\-escapes, or a bare token."""
    if len(raw) >= 2 and raw[0] == raw[-1] == '"':
        return re.sub(r"\\(.)", r"\1", raw[1:-1])
    return raw


def kconfig_defaults(path):
    """CONFIG_<name> -> first `default` of every `config <name>` entry."""
    defaults = {}
    name = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^\s*config\s+([A-Z0-9_]+)\s*$", line)
            if m:
                name = "CONFIG_" + m.group(1)
                continue
            m = re.match(r"^\s*default\s+(.+?)\s*$", line)
            if m and name and name not in defaults:
                defaults[name] = sdkconfig_value(m.group(1))
    return defaults


# The native build's effective values: secrets.defaults on top of the
# Kconfig defaults, as idf.py resolves them.
native = kconfig_defaults(kconfig_path)
with open(native_path, encoding="utf-8") as f:
    for line in f:
        m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line.rstrip("\n"))
        if m:
            native[m.group(1)] = sdkconfig_value(m.group(2))


def load_mapping(path):
    with open(path, encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data if isinstance(data, dict) else {}


def differs(esphome_values, key, expected):
    value = esphome_values.get(key)
    return expected is None or value is None or str(value) != expected


esphome = load_mapping(esphome_path)
credential_drift = [k for k, sym in CREDENTIALS.items()
                    if differs(esphome, k, native.get(sym))]

broker_uri = native.get("CONFIG_MQTT_BROKER_URI")
try:
    broker = urllib.parse.urlsplit(broker_uri) if broker_uri else None
    broker_port = broker.port if broker else None
except ValueError:  # a port that is not a number in 0..65535
    broker = None
if (broker is None or broker.scheme != ESPHOME_MQTT_SCHEME
        or broker_port not in ESPHOME_MQTT_PORTS):
    credential_drift.append(
        "mqtt_broker (scheme/port of CONFIG_MQTT_BROKER_URI: the ESPHome build "
        "always connects with mqtt:// on port 1883)")
elif (broker.hostname is None or esphome.get("mqtt_broker") is None
        or str(esphome["mqtt_broker"]).lower() != broker.hostname):
    credential_drift.append("mqtt_broker (host)")

clean = True
if credential_drift:
    clean = False
    print(f"WARNING: {native_path} and {esphome_path} disagree on: "
          + ", ".join(credential_drift))
    print("         Fix the stale copy with scripts/secrets-edit.sh [native|esphome].")
else:
    print(f"WiFi/MQTT credentials agree between {native_path} and {esphome_path}.")

if not os.path.isfile(substitutions_path):
    clean = False
    print(f"WARNING: {substitutions_path} does not exist (pull the private overlay);")
    print("         the topic drift check did not run.")
else:
    substitutions = load_mapping(substitutions_path)
    topic_drift = [f"{k} ({sym})" for k, sym in TOPICS.items()
                   if differs(substitutions, k, native.get(sym))]
    if topic_drift:
        clean = False
        print(f"WARNING: {native_path} and {substitutions_path} disagree on: "
              + ", ".join(topic_drift))
        print(f"         Fix the stale copy: scripts/secrets-edit.sh native, or edit "
              f"{substitutions_path} and commit it in the private overlay.")
    else:
        print(f"MQTT topics agree between {native_path} and {substitutions_path}.")

sys.exit(0 if clean else 3)
EOF
}

# Drift is a warning, not a failure: both builds still work with their own
# copy. A crash of the check itself is reported but does not hide the
# missing-overlay report below.
drift_status=0
drift_check || drift_status=$?
if [ "$drift_status" -ne 0 ] && [ "$drift_status" -ne 3 ]; then
  echo "WARNING: the drift check itself failed (exit $drift_status)."
fi

if [ ${#missing[@]} -gt 0 ]; then
  report_missing
  exit 1
fi
