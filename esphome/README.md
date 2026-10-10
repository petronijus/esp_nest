# ESPHome migration — feasibility spike (2026-10-10)

Two throwaway ESPHome 2026.9.1 builds used to decide how ESP Nest moves to
ESPHome. Both were flashed to the real device and measured; the device was
then returned to the native firmware (v2.0.1).

## Variants

| | `spike-a/` | `spike-b/` |
|---|---|---|
| Display driver | ESPHome `mipi_rgb`, upstream model `ZX2D10GE01R-V4848` | own ST7701S init + `esp_lcd` RGB panel in the `nest_display` external component |
| LVGL | ESPHome `lvgl` component (LVGL 9.5), rendered from ESPHome's main loop | LVGL 9.6.0 from the component registry, own FreeRTOS task on core 1, 2 SW draw units |
| 180° rotation | LVGL software rotation (extra PSRAM copy) | ST7701S SDIR mirrors X, flush only reverses row order (`hw_mirror_x: true`), or per-pixel copy like the native firmware (`false`) |
| Encoder | — (rotary_encoder, GPIO interrupts) | PCNT with 1 µs glitch filter, like the native firmware |

Both scenes are identical (`nest_probe` / `nest_display`): a 480×200 edge
glow fading in and out continuously, a 456 px ring, a sweeping 440 px arc and
a 140 px label redrawn every 100 ms with the project's own C-array font.

## Results on hardware

| Variant | fps | refresh avg | refresh max |
|---|---|---|---|
| A — ESPHome LVGL | **15.1** | 49.4 ms | 76 ms |
| B — own pipeline, per-pixel 180° | 33.6 | 28.3 ms | 46 ms |
| B — own pipeline, SDIR + row flip | **35.0** | 27.1 ms | 45 ms |

- B is 2.3× faster than A. Most of B's gain comes from the configuration
  (`-O2`, LVGL 9.6, FreeRTOS + 2 draw units, CLIB memory, larger caches),
  not from the rotation trick, which saves only ~1.2 ms per frame.
- Free memory under B: ~77 KB internal, 6.5 MB PSRAM.
- Encoder via PCNT works (counts arrive in LVGL's indev).
- Colours, orientation and timings are correct with the native init
  sequence and `pclk` not inverted.

**Decision input:** variant B — ESPHome provides WiFi, native API, OTA,
safe mode and HA entities; ESP Nest code keeps the display pipeline and UI.

## Migration path (verified)

1. The device keeps its ESP-IDF 5.5.3 bootloader and partition table. The
   ESPHome builds use a byte-identical copy of `partitions.csv`
   (`esp32: partitions:`), so the ESPHome app goes in through the native
   `/update` endpoint: `curl -F firmware=@firmware.ota.bin http://<ip>/update`.
   Upload `firmware.ota.bin` only — never `firmware.factory.bin`.
2. From then on ESPHome's encrypted OTA works (`esphome upload`, Device
   Builder in HA).
3. The way back works too: ESPHome's `web_server` OTA accepts any app image,
   e.g. `curl -F firmware=@native.bin http://<ip>/update`.

Safety net: safe mode after 10 failed boots (display and UI are not started
there), fallback AP + captive portal for wrong WiFi credentials. There is no
bootloader rollback with the current bootloader.

## Gotcha found on hardware: panel state survives MCU resets

The ST7701S has no reset line on this board, so its registers persist across
ESP resets and OTA reboots. An SDIR (X mirror) value left by one firmware
mirrored the image of the next one. `nest_display` now always issues a
software reset (`0x01`) and writes SDIR explicitly. The native firmware
(`main/screen.c`) does neither — harmless today, but it would show a mirrored
image after a firmware that set SDIR.

## Building

```bash
uv tool install esphome==2026.9.1      # same version as the HA Device Builder
cd esphome/spike-b
esphome compile esp-nest-spike-b.yaml
```

`secrets.yaml` (gitignored) holds `wifi_ssid`, `wifi_password` and `api_key`;
the WiFi values come from the SOPS-managed `secrets.defaults`.
