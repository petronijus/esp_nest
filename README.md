# ESP Nest

A round smart-home controller: a rotary-encoder knob with a circular 480×480
display that drives zigbee2mqtt lights, a fan, stereo volume and blinds over
MQTT, with Home Assistant auto-discovery.

Firmware is ESP-IDF (ESP32-S3) with an LVGL UI — four color-coded menu pages
(Lights / Fan / Blinds / Sound), detail screens, a WS2812 LED ring breathing in
the active page color, and HTTP OTA updates.

## Hardware

| Part | Detail |
|---|---|
| Board | Panlee **ZX2D10GE01R-V4848** — ESP32-S3, 16 MB flash, 8 MB octal PSRAM |
| Display | 2.1" round IPS 480×480, ST7701S (9-bit SPI init, then 16-bit parallel RGB) |
| Input | MT8901 rotary encoder (PCNT) + push button |
| LED ring | 13× WS2812 |

Pin map: `main/board.h`. Rendering is software-rotated 180° in the LVGL flush
callback. The SPI init pins are intentionally shared with RGB data pins — the
SPI bus is freed after the ST7701S init sequence (see `main/screen.c`).

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5, target `esp32s3`.

```bash
cp secrets.defaults.example secrets.defaults   # fill in WiFi + MQTT credentials
source <esp-idf>/export.sh
idf.py build                                   # output: build/ZX2D10GE01R.bin
```

`secrets.defaults` is gitignored and loaded by the root `CMakeLists.txt` as an
extra sdkconfig-defaults file. Firmware version comes from `version.txt`.

## Flash

**First time (USB):**

```bash
idf.py -p <port> flash
```

> Linux note: the board's CH343 USB bridge under the `cdc-acm` driver rejects
> the DTR/RTS ioctl esptool uses for bootloader reset (`BrokenPipeError`).
> Flash from macOS/Windows, or hold BOOT during reset to enter the bootloader
> manually. Prebuilt `*-factory.bin` images from
> [Releases](../../releases) can be flashed with
> `esptool.py --chip esp32s3 write_flash 0x0 esp_nest-vX.Y.Z-factory.bin`.

**Updates (OTA, preferred):** the device runs an HTTP OTA server on port 80.

```bash
curl -F "firmware=@build/ZX2D10GE01R.bin" http://<device-ip>/update
# or pull a released build and push it in one step:
./scripts/ota-update.sh v0.1.0 <device-ip>
```

> ⚠️ **Security:** the OTA endpoint is intentionally unauthenticated — anyone
> on the LAN can flash arbitrary firmware. Run the device on a trusted/IoT
> VLAN and never expose port 80 beyond it.

## MQTT topology

No middleman: Home Assistant ↔ zigbee2mqtt via MQTT discovery, ESP ↔ z2m
directly via topics. The topic names are defined at the top of
`main/ha_mqtt.c` — adjust them to your own z2m/ESPHome entity names.

- Lights: `zigbee2mqtt/<group>` state/set/get
- Fan: retained text topic (Off/low/medium/high)
- Stereo volume: stateless UP/DOWN topic (bridged to IR by an HA automation)
- Blinds: ESPHome `time_based` covers via a CC1101 bridge, group broadcast +
  per-cover position read-back
- Device: `espnest/<id>/state|avail|cmd/#`, discovery under `homeassistant/…`

### Light sync design (read before touching!)

Echoes of our own `/set` publishes are indistinguishable from external
commands on the state topic. Do **not** reintroduce value-comparison echo
detection — it was tried and caused brightness jump-backs and stuck ON states.
Current design (`main/light_sync.h`, pure logic, host-testable):

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
make -C test/host run    # must print "0 failures"
```

Any change to `light_sync.c` needs a scenario test, including a regression
test when fixing a sync bug.

## Releases

Tags `vX.Y.Z` build firmware in CI and attach `*-ota.bin` (for the OTA
endpoint), `*-factory.bin` (full image for first flash) and `sha256sums.txt`
to the GitHub release. See [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE)
