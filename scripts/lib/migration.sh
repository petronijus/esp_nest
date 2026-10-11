# shellcheck shell=bash
# Shared helpers of the migration scripts (migrate-to-esphome.sh,
# rollback-to-native.sh, mqtt-legacy-cleanup.sh). Sourced, not executed.
#
# Secrets are read from the gitignored esphome/secrets.yaml (written by
# scripts/secrets-decrypt.sh) and never printed or passed on a command line.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SECRETS_YAML="$REPO/esphome/secrets.yaml"

say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
ok()   { printf '    ok: %s\n' "$*"; }
warn() { printf '    WARN: %s\n' "$*" >&2; }
fail() { printf '    FAIL: %s\n' "$*" >&2; exit 1; }

# secret <name>: the value of one key of esphome/secrets.yaml (stdout).
secret() {
  [ -f "$SECRETS_YAML" ] || fail "$SECRETS_YAML missing — run scripts/secrets-decrypt.sh"
  python3 - "$SECRETS_YAML" "$1" <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r'^' + re.escape(sys.argv[2]) + r':\s*(?:"((?:[^"\\]|\\.)*)"|\'([^\']*)\'|(\S.*?))\s*$', text, re.M)
if not m:
    sys.exit(f"secret {sys.argv[2]!r} not found in {sys.argv[1]}")
print(next(g for g in m.groups() if g is not None), end="")
EOF
}

# make_private_tmpdir: sets PRIVATE_TMP to a 0700 scratch directory that is
# removed on exit. Call it directly, not in $(...): the EXIT trap belongs to
# the calling shell.
make_private_tmpdir() {
  PRIVATE_TMP="$(mktemp -d)" || fail "mktemp failed"
  chmod 700 "$PRIVATE_TMP"
  trap 'rm -rf "$PRIVATE_TMP"' EXIT
}

# app_image_info <file>: "<project_name> <version>" from the ESP-IDF app
# descriptor of an app image, or fails when the file is not one (a factory
# image starts with the bootloader, whose descriptor is not at 0x20).
app_image_info() {
  python3 - "$1" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read(0x100)
if len(data) < 0xB0 or data[0] != 0xE9:
    sys.exit("not an ESP32 image (no 0xE9 magic)")
magic, = struct.unpack_from("<I", data, 0x20)
if magic != 0xABCD5432:
    sys.exit("no app descriptor at 0x20 — a factory or bootloader image, not an app image")
version = data[0x30:0x50].split(b"\0")[0].decode(errors="replace")
project = data[0x50:0x70].split(b"\0")[0].decode(errors="replace")
print(project, version)
EOF
}

# wait_for_http <url> <seconds> [curl args...]: until the URL answers (any HTTP status).
wait_for_http() {
  local url="$1" secs="$2" i
  shift 2
  for ((i = 0; i < secs; i += 2)); do
    if curl -s -o /dev/null --max-time 2 "$@" "$url"; then return 0; fi
    sleep 2
  done
  return 1
}
