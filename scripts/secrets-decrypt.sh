#!/usr/bin/env bash
# Decrypt the SOPS-encrypted secrets into the gitignored ./secrets.defaults.
#
# The age private key lives in 1Password (document "SOPS age key (dev repos)")
# and is fetched at runtime — it never touches disk in plaintext. Requires the
# private overlay (esp_nest-private) cloned into ./private.
set -euo pipefail
cd "$(dirname "$0")/.."

ENC="private/config/secrets.defaults.sops"
OUT="secrets.defaults"
KEY_ITEM="SOPS age key (dev repos)"

command -v sops >/dev/null || { echo "sops not found — 'brew install sops age'"; exit 1; }
command -v op   >/dev/null || { echo "op (1Password CLI) not found"; exit 1; }
[ -f "$ENC" ] || { echo "Missing $ENC — clone the private overlay into ./private first."; exit 1; }

key="$(op document get "$KEY_ITEM" 2>/dev/null)" \
  || { echo "Could not read '$KEY_ITEM' from 1Password."; exit 1; }

SOPS_AGE_KEY="$(printf '%s' "$key" | grep '^AGE-SECRET-KEY')" \
  sops --input-type binary --output-type binary -d "$ENC" > "$OUT"

echo "Wrote $OUT (gitignored). Re-run 'idf.py reconfigure build' to pick it up."
