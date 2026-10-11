# ESP Nest

A round smart-home controller: a rotary-encoder knob with a circular 480×480
display that drives zigbee2mqtt lights, a fan, stereo volume and blinds over
MQTT, with its own entities in Home Assistant.

The firmware is **ESPHome** (ESP32-S3) with the `esp_nest` external
component: ESPHome provides WiFi, the Home Assistant native API, encrypted
OTA, safe mode and the web server; the component brings the ST7701S display
pipeline, an LVGL 9.6 UI — four colour-coded menu pages (Lights / Fan /
Blinds / Sound), detail screens — a WS2812 LED ring breathing in the active
page colour, and the MQTT logic for the external devices.

> The original ESP-IDF firmware (`main/`, v2.0.1) is kept buildable as the
> rollback image during the transition and will be removed afterwards. See
> [Native firmware (legacy)](#native-firmware-legacy).

## Hardware

| Part | Detail |
|---|---|
| Board | Panlee **ZX2D10GE01R-V4848** — ESP32-S3, 16 MB flash, 8 MB octal PSRAM |
| Display | 2.1" round IPS 480×480, ST7701S (9-bit SPI init, then 16-bit parallel RGB) |
| Input | MT8901 rotary encoder (PCNT) + push button |
| LED ring | 13× WS2812 |

The SPI init pins are intentionally shared with RGB data pins — the bus is
freed after the ST7701S init sequence. The picture is turned 180° by the
panel (X mirror) plus a row-reversing flush.

## Install in Home Assistant (ESPHome Device Builder)

1. Put the secrets listed in [`esphome/secrets.example.yaml`](esphome/secrets.example.yaml)
   into Home Assistant's `secrets.yaml`.
2. Create a device in the ESPHome Device Builder and paste
   [`esphome/example-device.yaml`](esphome/example-device.yaml) as its YAML;
   set the MQTT topic substitutions to your zigbee2mqtt / ESPHome entity
   names. The package is pinned to a release tag (`@vX.Y.Z`); bump the tag to
   update.
3. **First flash of a device that still runs the native firmware:** build
   the image and send the app image to its OTA endpoint — the device keeps
   its bootloader and partition table:

   ```bash
   curl -F "firmware=@firmware.ota.bin" http://<device-ip>/update
   ```

   Never upload `firmware.factory.bin` this way. From then on, updates are
   ESPHome's encrypted OTA (*Install* in the Device Builder).

A brand-new board needs one USB flash of the factory image (from macOS or
Windows — see the Linux note below).

## Build locally

```bash
uv tool install esphome==2026.9.1            # same version as the Device Builder
scripts/secrets-decrypt.sh                   # maintainer: writes esphome/secrets.yaml
esphome compile esphome/esp-nest-dev.yaml
esphome upload  esphome/esp-nest-dev.yaml --device <device-ip>
```

Without the private overlay, copy `esphome/secrets.example.yaml` to
`esphome/secrets.yaml`, fill it in, and build `esphome/esp-nest.yaml`
(generic topic defaults) or your own entry that overrides the substitutions.
Details — layout, guards, the local gate, the migration path and the way
back, rendering performance — are in [`esphome/README.md`](esphome/README.md).

> **Maintainer note:** the real secrets are SOPS-encrypted in the private
> overlay (`private/`, a clone of `esp_nest-private`). With the 1Password CLI
> signed in, `./scripts/secrets-decrypt.sh` materializes `esphome/secrets.yaml`
> and the native `secrets.defaults`; `./scripts/secrets-edit.sh
> [native|esphome]` changes one of them. Per-machine setup: [`HANDOVER.md`](HANDOVER.md).

> **Linux USB note:** the board's CH343 USB bridge under the `cdc-acm` driver
> rejects the DTR/RTS ioctl esptool uses for bootloader reset
> (`BrokenPipeError`). Flash over the network, or from macOS/Windows.

### Security

The web server (`/`, `POST /update`) is protected by HTTP basic auth
(`esp_nest_web_*` secrets); native API and OTA are encrypted with the API
key. The fallback access point (after 10 minutes without WiFi) has a
password. Keep the device on a trusted/IoT VLAN anyway.

### UI assets

The compiled UI assets (page glow images and the pre-rasterized fonts) live as
C arrays in `esphome/esp-nest-components/esp_nest/`, a single copy shared by
the ESPHome component and the native build (via `components/esp_nest_shared`).
Their design sources and the exact regeneration commands are in
[`assets/`](assets/README.md).

## MQTT topology

No middleman: Home Assistant ↔ zigbee2mqtt via MQTT discovery, ESP ↔ z2m
directly via topics. The knob's own entities (screen brightness, dim level,
idle timeout, LED ring, diagnostics) reach Home Assistant through the native
API. External topic names are package substitutions (`topic_*`, `blinds_*`
in [`esphome/esp-nest.yaml`](esphome/esp-nest.yaml)); the defaults are
generic — set them in your device YAML.

- Lights: `zigbee2mqtt/<group>` state/set/get
- Fan: retained text topic (Off/low/medium/high)
- Stereo volume: stateless UP/DOWN topic (bridged to IR by an HA automation)
- Blinds: ESPHome `time_based` covers via a CC1101 bridge, group broadcast +
  per-cover position read-back

### Light sync design (read before touching!)

Echoes of our own `/set` publishes are indistinguishable from external
commands on the state topic. Do **not** reintroduce value-comparison echo
detection — it was tried and caused brightness jump-backs and stuck ON states.
Current design (`esphome/esp-nest-components/esp_nest/light_sync.h`, pure
logic, host-testable):

1. Local changes throttled to one publish / 300 ms.
2. Every publish (re)opens a 1.5 s **authority window**; all incoming state
   messages inside it are dropped.
3. When the window closes, one `/get` resync is emitted; from then on incoming
   state is applied unconditionally (`state` wins over `brightness`).

zigbee2mqtt is the single source of truth; the device is eventually consistent
even when changes race the window.

## Testing

Pure-logic modules have host-side tests (plain gcc, no ESP-IDF):

```bash
make -C test/host run    # must end with "=== N test binaries, 0 failed"
```

Any change to `light_sync.c` needs a scenario test, including a regression
test when fixing a sync bug.

The whole local gate — host tests, ESPHome config and compile with the
partition-table check, the Home Assistant remote-package smoke test, the
native ESP-IDF 5.5.3 build in the `espressif/idf:v5.5.3` container, and a
check that no plaintext secrets are tracked — is one command, also run by the
release preflight:

```bash
scripts/check.sh            # all steps; or e.g. scripts/check.sh host esphome
```

It needs ESPHome 2026.9.1, Docker or Podman, and (for the dev entry) the
private overlay plus `esphome/secrets.yaml`; see
[`esphome/README.md`](esphome/README.md#local-gate).

## Native firmware (legacy)

ESP-IDF 5.5 firmware in `main/` (v2.0.1), kept as the rollback image until
the ESPHome build has soaked. It is not developed further.

```bash
cp secrets.defaults.example secrets.defaults   # fill in WiFi + MQTT credentials
source <esp-idf>/export.sh
idf.py build                                   # output: build/ZX2D10GE01R.bin
```

Its OTA endpoint is unauthenticated (`curl -F "firmware=@…" http://<ip>/update`).
CI release images of the native firmware carry **no credentials**: never
flash one to a device you want to stay online — use a local build with your
`secrets.defaults`.

## Releases

Tags `vX.Y.Z` are cut with the `repo-release` skill and serve as the
remote-package refs Home Assistant builds from. During the transition CI
still attaches the native `*-ota.bin` / `*-factory.bin` images (without
credentials, see above). See [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE)
