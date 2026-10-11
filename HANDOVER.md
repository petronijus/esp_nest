# Handover — working on ESP Nest from another machine

Since 2026-10-11 the knob runs the **ESPHome** build (branch
`esphome-migration`, see `docs/esphome-migration-plan.md`). The native
ESP-IDF toolchain is no longer needed on a dev machine: the native rollback
image builds inside the `espressif/idf:v5.5.3` container that
`scripts/check.sh` runs.

| Machine | State |
|---|---|
| Omarchy (Linux desktop, `omarchy`) | Set up 2026-10-11; `scripts/check.sh` passes. |
| petronijus-PC (Ubuntu) | Steps below — AI-handover task. |
| MacBook | Steps below — AI-handover task. |
| Windows | Steps below — AI-handover task. |

Tick a machine off here (and complete its Todoist task) once
`scripts/check.sh` passes on it.

## What every machine needs

| Tool | Why |
|---|---|
| git, `make` + a C compiler | clone, host tests (`test/host`) |
| [uv](https://docs.astral.sh/uv/) → `uv tool install esphome==2026.9.1` | ESPHome, pinned to the Home Assistant Device Builder's version |
| Docker or Podman | `check.sh` native step (`espressif/idf:v5.5.3`, ~1.5 GB first pull) |
| `sops`, `age`, 1Password CLI `op` (signed in to `my`) | `scripts/secrets-decrypt.sh` (age key from 1P document "SOPS age key (dev repos)") |
| SSH access to `petronijus/esp_nest-private` | the private overlay in `private/` |

The first ESPHome compile on a machine downloads ESPHome's ESP-IDF 5.5.5
toolchain into `~/.cache/esphome` (a few minutes).

## Steps (all machines)

```bash
cd ~/Documents/Dev/esp_nest
git fetch && git switch esphome-migration && git pull       # until it is merged to main
git clone git@github.com:petronijus/esp_nest-private.git private 2>/dev/null || (cd private && git pull)
uv tool install esphome==2026.9.1        # or: uv tool upgrade --reinstall esphome==2026.9.1
scripts/secrets-decrypt.sh               # writes esphome/secrets.yaml and secrets.defaults (gitignored)
make -C test/host run                    # "=== N test binaries, 0 failed"
scripts/check.sh                         # "All checks passed: secrets host esphome smoke native"
```

Optional, to reach the device (on the LAN, IP in HA
`sensor.esp_nest_ip_address`):

```bash
esphome logs esphome/esp-nest-dev.yaml --device <ip>        # read-only
```

Uploading firmware from a second machine is fine (`esphome upload …`), but
only from a clean checkout of the same branch — never a benchmark build
(`esp-nest-bench.yaml`).

## Per-OS notes

**petronijus-PC (Ubuntu).** `sudo apt install sops age build-essential` (or
the release binaries if the archive's sops is old), Docker from the
official repo with your user in the `docker` group. If the old native IDF
venv (`~/.espressif/v5.5.3`) is still there it is no longer used by this
project; leave it or remove it, nothing depends on it. Serial log (read
only): the CH343 port is `/dev/ttyACM0`, group `dialout` on Ubuntu (`uucp`
on Arch); USB *flashing* still does not work on Linux.

**MacBook.** `brew install uv sops age` and Docker Desktop or
`brew install podman && podman machine init && podman machine start`
(`check.sh` picks whichever engine exists; force one with
`CONTAINER_ENGINE=podman`). Xcode command-line tools provide `make`/`cc`.
The Mac is the machine for a **USB** flash should one ever be needed
(`esphome run esphome/esp-nest-dev.yaml --device /dev/cu.usbmodem*`): the
CH343 bridge works with macOS's driver.

**Windows.** `scripts/*.sh` are bash and the gate needs Docker, so work in
**WSL2** (Ubuntu) with Docker Desktop's WSL integration and follow the
Ubuntu notes inside WSL; clone the repo inside the WSL filesystem, not under
`/mnt/c`. `.gitattributes` keeps `partitions.csv` LF, so a Windows-side
checkout would not break the partition guard either. 1Password CLI in WSL:
use the Windows `op.exe` through interop (`alias op=op.exe`) or install the
Linux `op` and sign in.
