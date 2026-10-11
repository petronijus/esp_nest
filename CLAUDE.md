# ESP Nest

Round smart-home controller for the living room: a rotary-encoder knob with a
circular display that controls zigbee2mqtt lights, a fan, stereo volume and
blinds over MQTT, with its own entities in Home Assistant.

**Two builds during the transition.** The device runs **ESPHome 2026.9.1**
with the `esp_nest` external component (`esphome/`) — this is the firmware
that gets developed. The **native ESP-IDF firmware** (`main/`, `components/`,
v2.0.1) is frozen: it stays buildable as the rollback image until the
ESPHome build has soaked, then it is removed (plan step S17). Do not add
features to `main/`. Design, decisions and step plan:
`docs/esphome-migration-plan.md`; build, gate, migration, performance:
`esphome/README.md`.

## Hardware

- **Board:** Panlee ZX2D10GE01R-V4848 — ESP32-S3, 16 MB flash, 8 MB octal PSRAM
- **Display:** 2.1" round IPS 480×480, ST7701S (9-bit SPI for init only, then
  16-bit parallel RGB; the SPI pins double as RGB data pins — the bus is
  freed after the init sequence). The panel has **no reset line**: its
  registers survive MCU resets and OTA reboots, so init always starts with a
  software reset and writes SDIR explicitly (`nest_panel.cpp`).
- **Input:** MT8901 rotary encoder (PCNT, GPIO 5/6) + push button (GPIO 3)
- **LED ring:** 13× WS2812 (GPIO 4); backlight PWM on GPIO 38
- 180°: the panel mirrors X (SDIR), the flush reverses the row order.
- Pin map: `nest_panel.cpp` / `nest_input.cpp` (native: `main/board.h`).

## Architecture (`esphome/esp-nest-components/esp_nest/`)

ESPHome owns the platform (WiFi, native API, encrypted OTA, safe mode, web
server, diagnostic entities, the MQTT client); `esp_nest` owns the display
pipeline, LVGL 9.6, the UI and the device logic. The full module table is in
`esphome/README.md`; the essentials:

| Module | Thread | Role |
|---|---|---|
| `nest_core` | loopTask | `EspNest` component: starts the tasks, drains the UI event queue, settings entities. |
| `nest_ui_task` | nest_ui (core 1) | LVGL lifecycle and loop, one 80-line draw buffer, render statistics / benchmark. |
| `nest_ui.c` | nest_ui | The UI (port of `main/ui.c`): 4 menu pages from the `PAGES[]` table + detail screens. The arc indicator is a solid colour on purpose. |
| `nest_panel`, `nest_input`, `nest_backlight` | nest_ui | ST7701S + RGB panel, encoder/button indev, LEDC backlight with idle dimming. |
| `nest_led_ring` | nest_led (core 0) | Breathing animation in the page colour. |
| `nest_mqtt_bridge` | loopTask | External-device MQTT glue; delegates to the pure modules. |
| `light_sync`, `mqtt_payload`, `fan_sync`, `blinds_sync`, `input_fsm`, `backlight_policy`, … | caller's | **Pure C** (no ESP-IDF / ESPHome includes), host-tested. |

**Threading rule (hard):** LVGL and LEDC only on `nest_ui`; ESPHome APIs
(MQTT, entities, preferences) only on loopTask. Cross-task traffic goes
through the explicit hand-offs (UI event queue, atomics, the status
seqlock); `NEST_ASSERT_UI_TASK()` / `NEST_ASSERT_LOOP_TASK()` enforce it.
C files log with `NEST_LOGx` (`nest_c.h`), never IDF's `ESP_LOGx`.

Time in the pure modules is int64 milliseconds (`millis_64()` /
`esp_timer`), never ESPHome's uint32 `millis()` — it wraps after 49.7 days.

## MQTT topology

No middleman: HA ↔ Z2M via MQTT discovery, ESP ↔ Z2M directly via topics.
The knob's **own** entities (backlight, dim level, idle timeout, LED ring,
diagnostics) reach HA through ESPHome's **native API**, not MQTT.
External topic names are package substitutions (`topic_*`, `blinds_*` in
`esphome/esp-nest.yaml`); the defaults are generic, the real values live in
the private overlay (`private/esphome/substitutions.yaml`) and in the HA
device YAML. Petr wants this kept on direct MQTT — do not propose moving the
external devices to HA actions.

- Lights: `<topic_lights>` (state), `…/set`, `…/get`
- Fan: `<topic_fan>` (retained text: Off/low/medium/high)
- Stereo volume: `<topic_stereo_vol>/set` (`UP`/`DOWN`, stateless — an HA
  automation bridges it to NEC IR codes, the level cannot be read back)
- Blinds: ESPHome bridge `<topic_blinds_prefix>/<group>_{1..3}/…` for two
  groups (time_based covers, position 0=closed..100=open; read = average of
  the three members' `position/state`) + `…_group/command` broadcast for
  full open/close. The CC1101 bridge is a separate private project — pointer
  in `private/ops/notes.md`.
- Payload parsing: `mqtt_payload.c` (hand-rolled, exact native semantics).

### Light sync design (read before touching!)

Echoes of our own `/set` publishes are **indistinguishable** from external
commands on the state topic. Do NOT reintroduce value-comparison echo
detection — it was tried (two-phase suppression + rounding guard) and caused
brightness jump-backs and stuck ON states. Current design (`light_sync.h`):

1. Local changes throttled to one publish / 300 ms.
2. Every publish (re)opens a 1.5 s **authority window**; ALL incoming state
   messages inside it are dropped, no exceptions.
3. When the window closes, one `/get` resync is emitted; from then on
   incoming state is applied **unconditionally** (`state` field wins over
   `brightness` — Z2M reports stored brightness even when OFF).

Z2M is the single source of truth; the device is eventually consistent even
when changes race the window.

## Build, check & deploy

```bash
uv tool install esphome==2026.9.1            # same version as the HA Device Builder
scripts/secrets-decrypt.sh                   # writes esphome/secrets.yaml (+ native secrets.defaults)
esphome compile esphome/esp-nest-dev.yaml    # dev build: private substitutions + secrets
scripts/check.sh                             # the whole local gate (is the CI)
```

`scripts/check.sh` runs the secrets check, host tests, ESPHome config and
compile, the HA remote-package smoke test and the native build in the
`espressif/idf:v5.5.3` container. It must pass before a commit is proposed.

**Deploy (OTA, encrypted):**

```bash
# IP: HA sensor.esp_nest_ip_address, or the device status line
esphome upload esphome/esp-nest-dev.yaml --device <ip>
esphome logs   esphome/esp-nest-dev.yaml --device <ip>
```

The native → ESPHome migration (`curl` of `firmware.ota.bin` to the native
`/update`) and the way back (basic-auth `POST /update` on ESPHome's web
server with the native image) are in `esphome/README.md`. Upload
`firmware.ota.bin` only, never `firmware.factory.bin`. The rollback image is
the locally built native v2.0.1 with real secrets — never a CI release asset
(those carry no credentials).

**Performance work:** benchmark with `esphome/esp-nest-bench.yaml` (the menu
pages itself, DEBUG `Render:` lines) and flash the dev build back afterwards.
Petr cares about fps / smoothness, not UX timings — keep the visual result
identical and do not offer timing changes. Measured results and rejected
experiments: `esphome/README.md` → *Rendering performance*.

**USB flashing does not work on Linux:** the board's CH343 bridge under the
`cdc-acm` driver rejects the DTR/RTS ioctl esptool uses for bootloader reset
(`BrokenPipeError`). The serial port (`/dev/ttyACM0`, group `uucp`) still
works for reading the bootloader log. Other ESP devices may be connected
(e.g. on a CP2104 / `ttyUSB0`) — never probe/flash ports blindly.

## Testing

Pure-logic modules get host-side tests (plain gcc, no ESP-IDF):

```bash
make -C test/host run    # must end with "=== N test binaries, 0 failed"
```

Any change to `light_sync.c` needs a scenario test — including a regression
test when fixing a sync bug (see the `BUG n regression` scenarios). Time
values in tests cross 2^32 ms.

## Repo conventions (public repo)

- Conventional commits: `feat:` / `fix:` / `chore:` / `docs:` / `refactor:` / `perf:`.
- Releases are cut with the `repo-release` skill; config in `.release.yml`
  (preflight runs `scripts/check.sh`). Never tag or edit CHANGELOG.md manually.
- Private overlay: `private/` (gitignored) = clone of `petronijus/esp_nest-private`
  — ops notes, SOPS-encrypted secrets, the real substitutions. Bootstrap:
  `git clone git@github.com:petronijus/esp_nest-private.git private`
- Cross-machine setup: `HANDOVER.md`.
