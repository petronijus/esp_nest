# ESP Nest

Round smart-home controller for the living room: a rotary-encoder knob with a
circular display that controls zigbee2mqtt lights and a fan over MQTT, with
Home Assistant discovery.

## Hardware

- **Board:** Panlee ZX2D10GE01R-V4848 — ESP32-S3, 16 MB flash, 8 MB octal PSRAM
- **Display:** 2.1" round IPS 480×480, ST7701S (9-bit SPI for init only, then
  16-bit parallel RGB; SPI pins are intentionally shared with RGB data pins —
  the SPI bus is freed after the init sequence, see `screen.c`)
- **Input:** MT8901 rotary encoder (PCNT) + push button (GPIO 3)
- **LED ring:** 13× WS2812 (GPIO 4)
- Pin map lives in `main/board.h`. Rendering is software-rotated 180° in the
  LVGL flush callback.

## Architecture (`main/`)

| Module | Role |
|---|---|
| `main.c` | Startup + LVGL task (core 1). The LVGL loop drains `ui_process_pending()` and `ha_mqtt_process_pending()` every iteration — cross-thread work happens via pending flags, never direct LVGL calls. |
| `screen.c` | ST7701S init, RGB panel, LVGL display + encoder indev, backlight PWM with idle dimming |
| `ui.c` | Single LVGL screen, 4 menu pages driven by the `PAGES[]` table (Lights / Fiiuuu / Blinds / Sound — title, accent color, dot position, arc angle, edge glow) + detail screens for brightness, fan and sound (relative volume). The arc indicator is a solid color on purpose — a gradient was too slow. Thread-safe setters (`ui_set_brightness` etc.) only set pending values. |
| `ha_mqtt.c` | MQTT client, HA discovery, topic routing. Thin layer — light sync logic is delegated. |
| `light_sync.c/.h` | **Pure-logic** light synchronization state machine (no ESP-IDF includes — host-testable). |
| `wifi.c` | STA with endless reconnect + backoff |
| `ota_server.c` | HTTP OTA upload on port 80 (`/` form, `POST /update`) |
| `led_ring.c` | Breathing animation in current page color |

## MQTT topology

No middleman: HA ↔ Z2M via MQTT discovery, ESP ↔ Z2M directly via topics.

- Lights: `zigbee2mqtt/livingroom_lights` (state), `…/set`, `…/get`
- Fan: `bashouse/livingroom-fan` (retained text: Off/low/medium/high)
- Stereo volume: `bashouse/stereo/volume/set` (`UP`/`DOWN`, stateless — an HA
  automation bridges it to NEC IR codes, the level cannot be read back)
- Blinds: ESPHome bridge `blinds-cc1101/cover/rolety_{klavir|dj}_{1..3}/…`
  (time_based covers, position 0=closed..100=open; read = average of the three
  members' `position/state`) + `…_group/command` broadcast for full open/close.
  The CC1101 bridge is a separate private project — pointer in `private/ops/notes.md`.
- Device: `espnest/<id>/state|avail|cmd/#`, discovery under `homeassistant/…`
- JSON is parsed with tiny hand-rolled helpers in `ha_mqtt.c` — no JSON lib.

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

## Build & flash

```bash
source ~/.espressif/v5.5.3/esp-idf/export.sh   # IDF 5.5, target esp32s3
idf.py build                                    # output: build/ZX2D10GE01R.bin
```

- WiFi/MQTT credentials live in `secrets.defaults` (gitignored, loaded by the
  root `CMakeLists.txt`). After an Ubuntu Python upgrade the IDF venv may need
  `./install.sh esp32s3` + `idf.py fullclean`.

**Deploy via OTA (preferred):**

```bash
# IP is in HA: sensor.esp_nest_ip_address (or on the device status line)
curl -F "firmware=@build/ZX2D10GE01R.bin" http://<ip>/update
```

Verify the target first: `curl http://<ip>/` must return "ESP Nest OTA Update".

**USB flashing does not work on Linux:** the board's CH343 bridge under the
`cdc-acm` driver rejects the DTR/RTS ioctl esptool uses for bootloader reset
(`BrokenPipeError`). Other ESP devices may be connected (e.g. on a CP2104 /
`ttyUSB0`) — never probe/flash ports blindly.

## Testing

Pure-logic modules get host-side tests (plain gcc, no ESP-IDF):

```bash
make -C test/host run    # must print "0 failures"
```

Any change to `light_sync.c` needs a scenario test — including a regression
test when fixing a sync bug (see the `BUG n regression` scenarios).

## Repo conventions (public repo)

- Conventional commits: `feat:` / `fix:` / `chore:` / `docs:` / `refactor:`.
- Releases are cut with the `repo-release` skill; config in `.release.yml`.
  Never tag or edit CHANGELOG.md manually.
- Private overlay: `private/` (gitignored) = clone of `petronijus/esp_nest-private`
  — ops notes, device details. Bootstrap:
  `git clone git@github.com:petronijus/esp_nest-private.git private`
