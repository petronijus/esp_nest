#!/usr/bin/env bash
# Local gate: the whole CI of this repository (the hosted release workflow
# only runs on tags). The release preflight runs it (.release.yml);
# docs/esphome-migration-plan.md, section 9, is the specification.
#
#   scripts/check.sh              every step, in the order below
#   scripts/check.sh <step>...    only the named steps
#
# Steps:
#   secrets  no plaintext secrets file (*secrets.yaml, *secrets.defaults) is
#            tracked, or would be added by `git add -A`.
#   host     host-side unit tests of the pure C modules (test/host).
#   esphome  `esphome config` of the generic package (dummy secrets) and of
#            the dev entry (real secrets + private substitutions), the
#            blinds label validator's rejections (bad labels must fail
#            `esphome config` with its message), `esphome compile` of the
#            generic package, and the partition-table.bin sha256 gate.
#   smoke    the package consumed the way the Home Assistant Device Builder
#            consumes it: example-device.yaml in an empty HA-like directory,
#            its github:// package swapped for a file:// clone of the tree.
#   native   the native ESP-IDF 5.5.3 build, in the espressif/idf container,
#            plus the same partition-table.bin gate.
#
# What is tested is the content git would commit (`git add -A`): tracked
# files plus untracked, non-ignored ones, copied into a scratch snapshot.
# Gitignored files (secrets, the private overlay, build state) are invisible
# there, as in a clone of the published repository. The exceptions are by
# design: the dev entry is validated in the working tree, because it reads
# esphome/secrets.yaml and private/esphome/substitutions.yaml, and the native
# build gets a copy of secrets.defaults when it exists.
#
# The scratch space (~/.cache/esp_nest-check/run.*) is deleted on exit: it
# may hold a copy of secrets.defaults and the native sdkconfig built from it.
# A run that could not clean up (SIGKILL, OOM, power loss) is swept by the
# next run. The repository's own build/ is never touched.
#
# Environment:
#   CONTAINER_ENGINE   docker or podman (default: docker when its daemon
#                      answers, else podman).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && git rev-parse --show-toplevel)"

# Same version as the Home Assistant Device Builder (esphome/README.md).
ESPHOME_VERSION="2026.9.1"
# The native firmware's toolchain.
IDF_IMAGE="espressif/idf:v5.5.3"
# sha256 of the partition table generated from partitions.csv. The device
# keeps this table (OTA cannot change it), so both builds must produce it
# byte for byte.
PARTITION_TABLE_SHA256="81104acc28f8049debaaeff6a586a5eb5e51cf71f835f95c54ccc09889fe6830"
# esphome.name of the package (its default `name` substitution): the build
# directory is .esphome/build/<name>.
ESPHOME_NAME="esp-nest"
# Per-run scratch directories and the component-manager downloads of the
# native build (kept between runs) live here.
CHECK_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/esp_nest-check"
IDF_COMPONENT_CACHE="$CHECK_ROOT/idf-components"

ALL_STEPS=(secrets host esphome smoke native)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Start time of process <pid> as ps reports it (Linux and macOS), empty when
# no such process exists. With the PID it identifies a process even after
# its PID has been reused.
process_start() {
  ps -o lstart= -p "$1" 2>/dev/null | sed -e 's/^ *//' -e 's/ *$//'
}

# Each run records its owner (PID + start time) in run.*/owner. A run.*
# directory whose owner is gone was left behind by a run that was killed
# before its EXIT trap: remove it, it may hold secrets. A directory without
# an owner file is either from a run killed between mktemp and writing the
# file, or still being set up by a concurrent run; only the former is old.
# These directories are this script's own scratch (build output and a
# secrets copy that must not survive in a trash can), so they are removed
# outright like the EXIT trap does.
sweep_stale_runs() {
  local dir pid start
  for dir in "$CHECK_ROOT"/run.*; do
    [ -d "$dir" ] || continue
    if [ -f "$dir/owner" ]; then
      pid="" start=""
      { IFS= read -r pid || true; IFS= read -r start || true; } <"$dir/owner"
      if [ -n "$pid" ] && [ -n "$start" ] && [ "$(process_start "$pid")" = "$start" ]; then
        continue
      fi
    elif [ -z "$(find "$dir" -maxdepth 0 -mmin +10)" ]; then
      continue
    fi
    rm -rf "$dir"
    printf 'Removed stale scratch %s (left by a killed earlier run).\n' "$dir"
  done
}

# Scratch space on disk, not in $TMPDIR: the two builds need ~1.5 GB, and
# /tmp is often a size- or quota-limited RAM tmpfs. Removed on any exit,
# including Ctrl-C; see sweep_stale_runs for the exits no trap sees.
mkdir -p "$CHECK_ROOT"
sweep_stale_runs
SCRATCH="$(mktemp -d "$CHECK_ROOT/run.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf '%s\n%s\n' "$$" "$(process_start "$$")" >"$SCRATCH/owner"
LOGS="$SCRATCH/logs"
mkdir -p "$LOGS"

say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
ok()   { printf '    ok: %s\n' "$*"; }
fail() { printf '    FAIL: %s\n' "$*" >&2; exit 1; }

# run_logged <description> <log name> <command...>: output goes to a log;
# on failure its tail is shown and the gate stops.
run_logged() {
  local what="$1" log="$LOGS/$2.log"
  shift 2
  say "    $what ..."
  if ! "$@" >"$log" 2>&1; then
    printf '    --- last 80 lines of %s ---\n' "$(basename "$log")" >&2
    tail -n 80 "$log" >&2
    fail "$what"
  fi
  ok "$what"
}

# expect_config_rejected <description> <log name> <message> <esphome args...>:
# `esphome <args...> config` must fail, and its output must contain
# <message> (a fixed string), so the failure is the expected rejection and
# not some unrelated error.
expect_config_rejected() {
  local what="$1" log="$LOGS/$2.log" message="$3"
  shift 3
  say "    $what ..."
  if "$@" >"$log" 2>&1; then
    printf '    --- last 40 lines of %s ---\n' "$(basename "$log")" >&2
    tail -n 40 "$log" >&2
    fail "$what: esphome config passed, expected the rejection '$message'"
  fi
  if ! grep -qF -- "$message" "$log"; then
    printf '    --- last 80 lines of %s ---\n' "$(basename "$log")" >&2
    tail -n 80 "$log" >&2
    fail "$what: esphome config failed without the expected message '$message'"
  fi
  ok "$what"
}

sha256_of() {
  if command -v sha256sum >/dev/null; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

# check_partition_table <partition-table.bin> <which build>
check_partition_table() {
  local bin="$1" which="$2" digest
  [ -f "$bin" ] || fail "$which: no partition table at $bin"
  digest="$(sha256_of "$bin")"
  [ "$digest" = "$PARTITION_TABLE_SHA256" ] \
    || fail "$which: partition-table.bin sha256 $digest, expected $PARTITION_TABLE_SHA256"
  ok "$which partition-table.bin sha256 = ${PARTITION_TABLE_SHA256:0:8}…${PARTITION_TABLE_SHA256: -4}"
}

# snapshot_to <dir>: copy what `git add -A` would commit (tracked files that
# still exist, plus untracked non-ignored files) into <dir>.
snapshot_to() {
  mkdir -p "$1"
  (
    cd "$REPO"
    git ls-files -z --cached --others --exclude-standard \
      | while IFS= read -r -d '' f; do
          if [ -e "$f" ] || [ -L "$f" ]; then printf '%s\0' "$f"; fi
        done
  ) | rsync -a --from0 --files-from=- "$REPO/" "$1/"
}

# The shared snapshot for the host and esphome steps, made once.
SNAP=""
snapshot() {
  if [ -z "$SNAP" ]; then
    SNAP="$SCRATCH/tree"
    snapshot_to "$SNAP"
  fi
}

# Top-level keys of a flat YAML mapping (the secrets files).
yaml_keys() {
  sed -nE 's/^([A-Za-z0-9_]+):.*/\1/p' "$1"
}

# write_dummy_secrets <file>: a secrets.yaml with a dummy value for every
# name in secrets.example.yaml (*_api_key: a valid 32-byte base64 key; not
# all zeros, which ESPHome rejects as reserved).
write_dummy_secrets() {
  local out="$1" key value
  : >"$out"
  for key in $(yaml_keys "$REPO/esphome/secrets.example.yaml"); do
    case "$key" in
      *_api_key) value="$(head -c 32 /dev/zero | tr '\0' 'x' | base64)" ;;
      *) value="dummy-${key//_/-}" ;;
    esac
    printf '%s: "%s"\n' "$key" "$value" >>"$out"
  done
}

require_esphome() {
  command -v esphome >/dev/null \
    || fail "esphome not found — uv tool install esphome==$ESPHOME_VERSION"
  local version
  version="$(esphome version 2>/dev/null | sed -n 's/^Version: //p')"
  [ "$version" = "$ESPHOME_VERSION" ] \
    || fail "esphome $version installed, the gate needs $ESPHOME_VERSION (the HA Device Builder's) — uv tool install --force esphome==$ESPHOME_VERSION"
}

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------

step_secrets() {
  step "secrets: no plaintext secrets in git"
  local found
  found="$(git -C "$REPO" ls-files --cached --others --exclude-standard \
    -- '*secrets.yaml' '*secrets.defaults')"
  if [ -n "$found" ]; then
    say "    tracked or addable (not gitignored):" >&2
    printf '%s\n' "$found" | sed 's/^/      /' >&2
    fail "plaintext secrets files would be committed"
  fi
  ok "no *secrets.yaml / *secrets.defaults tracked or addable"
}

step_host() {
  step "host: unit tests of the pure modules"
  snapshot
  run_logged "make -C test/host run" host make -C "$SNAP/test/host" run
  ok "$(grep -E '^=== [0-9]+ test binaries' "$LOGS/host.log")"
}

step_esphome() {
  step "esphome: config, compile, partition gate (ESPHome $ESPHOME_VERSION)"
  require_esphome
  snapshot

  write_dummy_secrets "$SNAP/esphome/secrets.yaml"
  run_logged "config esphome/esp-nest.yaml (generic package, dummy secrets)" \
    config-generic esphome config "$SNAP/esphome/esp-nest.yaml"

  [ -f "$REPO/private/esphome/substitutions.yaml" ] \
    || fail "private/esphome/substitutions.yaml missing — clone or pull the private overlay into ./private"
  [ -f "$REPO/esphome/secrets.yaml" ] \
    || fail "esphome/secrets.yaml missing — run scripts/secrets-decrypt.sh"
  local missing
  missing="$(comm -23 <(yaml_keys "$REPO/esphome/secrets.example.yaml" | sort) \
                      <(yaml_keys "$REPO/esphome/secrets.yaml" | sort))"
  [ -z "$missing" ] \
    || fail "esphome/secrets.yaml lacks $(printf '%s' "$missing" | tr '\n' ' ') (names in secrets.example.yaml) — pull the private overlay and re-run scripts/secrets-decrypt.sh"
  run_logged "config esphome/esp-nest-dev.yaml (working tree, real secrets)" \
    config-dev esphome config "$REPO/esphome/esp-nest-dev.yaml"

  # The blinds label validator (esp_nest/__init__.py) must reject what
  # would render wrong: a character the label font has no glyph for, a
  # label with nothing visible, and leading or trailing whitespace.
  local cfg="$SNAP/esphome/esp-nest.yaml"
  expect_config_rejected "label with a glyph the font lacks ('Čáp') is rejected" \
    label-glyph "which font_helvetica_100.c has no glyph for" \
    esphome -s blinds_label_a "Čáp" config "$cfg"
  expect_config_rejected "whitespace-only label is rejected" \
    label-blank "must not be empty or only whitespace" \
    esphome -s blinds_label_b "   " config "$cfg"
  expect_config_rejected "label with a leading space is rejected" \
    label-space "has leading or trailing whitespace" \
    esphome -s blinds_label_a " DJ" config "$cfg"

  # The MQTT topic validator (esp_nest/__init__.py) must reject topics that
  # collide or contain wildcards: ESPHome would call every matching
  # subscription, and a published topic that is also subscribed would feed
  # our own publishes back as state.
  expect_config_rejected "equal fan and lights topics are rejected" \
    topic-duplicate "is used for both" \
    esphome -s topic_fan zigbee2mqtt/lights config "$cfg"
  expect_config_rejected "equal blinds group ids are rejected" \
    topic-group "is used for both" \
    esphome -s blinds_group_b group_a config "$cfg"
  expect_config_rejected "a published topic that is also subscribed is rejected" \
    topic-feedback "is published to as" \
    esphome -s topic_lights x/set -s topic_stereo_vol x config "$cfg"
  expect_config_rejected "a fan topic equal to a published topic is rejected" \
    topic-fan "The fan topic" \
    esphome -s topic_fan zigbee2mqtt/lights/set config "$cfg"
  expect_config_rejected "a wildcard topic is rejected" \
    topic-wildcard "Wildcards can not be used in topic names" \
    esphome -s topic_fan 'home/#' config "$cfg"

  # The broker is a bare host: the native Kconfig value is a full mqtt:// URI
  # and would only fail at runtime, when the name never resolves.
  local bad_broker="$SNAP/esphome/esp-nest-bad-broker.yaml"
  sed 's|!secret mqtt_broker|mqtt://broker.invalid:1883|' "$cfg" >"$bad_broker"
  expect_config_rejected "a broker with a scheme and port is rejected" \
    broker-uri "without a scheme (mqtt://)" \
    esphome config "$bad_broker"
  sed 's|!secret mqtt_broker|broker.invalid:1883|' "$cfg" >"$bad_broker"
  expect_config_rejected "a broker with a ':port' suffix is rejected" \
    broker-port "without a scheme (mqtt://)" \
    esphome config "$bad_broker"

  run_logged "compile esphome/esp-nest.yaml (generic package)" \
    compile esphome compile "$SNAP/esphome/esp-nest.yaml"
  local build="$SNAP/esphome/.esphome/build/$ESPHOME_NAME/build"
  [ -s "$build/firmware.ota.bin" ] || fail "no firmware.ota.bin in $build"
  ok "firmware.ota.bin $(wc -c <"$build/firmware.ota.bin" | tr -d ' ') bytes"
  check_partition_table "$build/partition_table/partition-table.bin" "ESPHome"
}

step_smoke() {
  step "smoke: example-device.yaml with the package from a git clone"
  require_esphome
  local pkg="$SCRATCH/package-repo" ha="$SCRATCH/ha" hooks="$SCRATCH/no-hooks"

  # A throwaway repository holding the snapshot, standing in for GitHub.
  snapshot_to "$pkg"
  mkdir -p "$hooks"
  git -C "$pkg" -c core.hooksPath="$hooks" init -q -b gate
  git -C "$pkg" add -A
  git -C "$pkg" -c core.hooksPath="$hooks" -c commit.gpgsign=false \
    -c user.name="esp_nest check" -c user.email="check@localhost" \
    commit -q -m "check.sh snapshot"

  # example-device.yaml, unchanged except for its package source:
  # github://<owner>/<repo>/<file>@<tag>  ->  file:// clone, same <file>.
  mkdir -p "$ha"
  local example="$REPO/esphome/example-device.yaml" re line file
  re='^([[:space:]]*)esp_nest:[[:space:]]+github://[^/]+/[^/]+/([^@[:space:]]+)@[^[:space:]]+[[:space:]]*$'
  [ "$(grep -cE "$re" "$example")" = 1 ] \
    || fail "example-device.yaml: expected exactly one 'esp_nest: github://<owner>/<repo>/<file>@<tag>' line"
  while IFS= read -r line; do
    if [[ $line =~ $re ]]; then
      file="${BASH_REMATCH[2]}"
      printf '%sesp_nest:\n' "${BASH_REMATCH[1]}"
      printf '%s  url: file://%s\n' "${BASH_REMATCH[1]}" "$pkg"
      printf '%s  ref: gate\n' "${BASH_REMATCH[1]}"
      printf '%s  files: [%s]\n' "${BASH_REMATCH[1]}" "$file"
      printf '%s  refresh: 0s\n' "${BASH_REMATCH[1]}"
    else
      printf '%s\n' "$line"
    fi
  done <"$example" >"$ha/esp-nest.yaml"
  write_dummy_secrets "$ha/secrets.yaml"

  run_logged "config example-device.yaml (remote package via file://)" \
    smoke esphome config "$ha/esp-nest.yaml"

  # Everything the package refers to by relative path must come from the
  # clone, never from the HA directory.
  local clones clone
  clones="$(find "$ha/.esphome/packages" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)"
  [ -n "$clones" ] && [ "$(printf '%s\n' "$clones" | wc -l | tr -d ' ')" = 1 ] \
    || fail "expected exactly one package clone under $ha/.esphome/packages"
  clone="$clones"
  [ -f "$clone/$file" ] || fail "the clone $clone has no $file"
  ok "package cloned into .esphome/packages/$(basename "$clone")"
  # The validated config dump names both by absolute path.
  grep -qF "  partitions: $clone/" "$LOGS/smoke.log" \
    || fail "esp32.partitions did not resolve inside the clone (see the config dump)"
  ok "partitions.csv resolved from the clone"
  grep -qF "  path: $clone/$(dirname "$file")/esp-nest-components" "$LOGS/smoke.log" \
    || fail "external component esp_nest did not resolve inside the clone (see the config dump)"
  ok "esp_nest component resolved from the clone"
}

step_native() {
  step "native: ESP-IDF build in $IDF_IMAGE"
  local engine="${CONTAINER_ENGINE:-}"
  if [ -z "$engine" ]; then
    if command -v docker >/dev/null && docker info >/dev/null 2>&1; then
      engine=docker
    elif command -v podman >/dev/null; then
      engine=podman
    else
      fail "no container engine (docker or podman) for the native build"
    fi
  fi
  # Build as the invoking user, so nothing root-owned is left behind.
  local user_args=(--user "$(id -u):$(id -g)")
  if [ "$engine" = podman ]; then
    user_args=(--userns=keep-id "${user_args[@]}")
  fi

  local tree="$SCRATCH/native"
  snapshot_to "$tree"
  if [ -f "$REPO/secrets.defaults" ]; then
    cp -p "$REPO/secrets.defaults" "$tree/"
    ok "building with secrets.defaults"
  else
    ok "no secrets.defaults: building with the Kconfig defaults"
  fi
  mkdir -p "$IDF_COMPONENT_CACHE"

  run_logged "idf.py build ($engine)" native \
    "$engine" run --rm "${user_args[@]}" \
      -e HOME=/tmp \
      -e IDF_COMPONENT_CACHE_PATH=/idf-component-cache \
      -v "$IDF_COMPONENT_CACHE:/idf-component-cache" \
      -v "$tree:/project" -w /project \
      "$IDF_IMAGE" idf.py build
  [ -s "$tree/build/ZX2D10GE01R.bin" ] || fail "no build/ZX2D10GE01R.bin"
  ok "ZX2D10GE01R.bin $(wc -c <"$tree/build/ZX2D10GE01R.bin" | tr -d ' ') bytes"
  check_partition_table "$tree/build/partition_table/partition-table.bin" "native"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=("${ALL_STEPS[@]}")
for s in "${steps[@]}"; do
  case " ${ALL_STEPS[*]} " in
    *" $s "*) ;;
    *) say "unknown step '$s' (steps: ${ALL_STEPS[*]})" >&2; exit 2 ;;
  esac
done

for s in "${steps[@]}"; do
  "step_$s"
done
printf '\nAll checks passed: %s\n' "${steps[*]}"
