#!/usr/bin/env bash
# Edit the SOPS-encrypted secrets in $EDITOR: sops decrypts to a temp buffer,
# you edit, and it re-encrypts in place on save. Commit the resulting
# private/config/secrets.defaults.sops in the private overlay afterwards.
#
# Use this to fill the FILL_ME WiFi/MQTT credentials (e.g. on the build host
# where the real values live) without ever writing plaintext to the repo.
set -euo pipefail
cd "$(dirname "$0")/.."

ENC="private/config/secrets.defaults.sops"
KEY_ITEM="SOPS age key (dev repos)"

command -v sops >/dev/null || { echo "sops not found — 'brew install sops age'"; exit 1; }
command -v op   >/dev/null || { echo "op (1Password CLI) not found"; exit 1; }
[ -f "$ENC" ] || { echo "Missing $ENC — clone the private overlay into ./private first."; exit 1; }

key="$(op document get "$KEY_ITEM" 2>/dev/null)" \
  || { echo "Could not read '$KEY_ITEM' from 1Password."; exit 1; }

SOPS_AGE_KEY="$(printf '%s' "$key" | grep '^AGE-SECRET-KEY')" \
  sops --input-type binary --output-type binary "$ENC"

echo "Re-encrypted $ENC — commit it in the private overlay (cd private && git add -A && git commit)."
