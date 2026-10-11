#!/usr/bin/env bash
# Edit SOPS-encrypted secrets in $EDITOR: sops decrypts to a temp buffer,
# you edit, and it re-encrypts in place on save. Commit the resulting .sops
# file in the private overlay afterwards, then run scripts/secrets-decrypt.sh
# to refresh the plaintext copy.
#
#   scripts/secrets-edit.sh [native]   private/config/secrets.defaults.sops
#                                      (native build: WiFi/MQTT creds, topics)
#   scripts/secrets-edit.sh esphome    private/config/esphome-secrets.yaml.sops
#                                      (ESPHome build; names in esphome/secrets.example.yaml)
#
# Until the native firmware is removed, the WiFi and MQTT credentials exist in
# both files, and the external MQTT topics in the native file and in
# private/esphome/substitutions.yaml: change them in both copies.
# secrets-decrypt.sh warns, by key name, when the copies have drifted apart.
set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-native}" in
  native)  ENC="private/config/secrets.defaults.sops" ;;
  esphome) ENC="private/config/esphome-secrets.yaml.sops" ;;
  *) echo "usage: secrets-edit.sh [native|esphome]"; exit 2 ;;
esac
KEY_ITEM="SOPS age key (dev repos)"

command -v sops >/dev/null || { echo "sops not found — 'brew install sops age'"; exit 1; }
[ -f "$ENC" ] || { echo "Missing $ENC — clone the private overlay into ./private first."; exit 1; }

if [ -z "${SOPS_AGE_KEY:-}" ]; then
  command -v op >/dev/null || { echo "op (1Password CLI) not found"; exit 1; }
  key="$(op document get "$KEY_ITEM" 2>/dev/null)" \
    || { echo "Could not read '$KEY_ITEM' from 1Password."; exit 1; }
  SOPS_AGE_KEY="$(printf '%s\n' "$key" | grep '^AGE-SECRET-KEY')" \
    || { echo "'$KEY_ITEM' holds no AGE-SECRET-KEY line."; exit 1; }
  unset key
fi
export SOPS_AGE_KEY

sops --input-type binary --output-type binary "$ENC"

echo "Re-encrypted $ENC — commit it in the private overlay (cd private && git add -A && git commit),"
echo "then run scripts/secrets-decrypt.sh to refresh the plaintext copy."
