# ESP Nest on ESPHome

ESP Nest is moving from the native ESP-IDF firmware (`main/`) to ESPHome
2026.9.1. ESPHome provides WiFi, the native API, OTA, safe mode, the web
server and the diagnostic entities; the `esp_nest` external component keeps
the display pipeline, the UI and the device logic. The full design, the
decisions and the step plan are in
[`docs/esphome-migration-plan.md`](../docs/esphome-migration-plan.md).

## Layout

| Path | Role |
|---|---|
| `esp-nest.yaml` | The package: complete configuration with generic substitution defaults. |
| `esp-nest-dev.yaml` | Local entry point: private substitutions + the package. |
| `example-device.yaml` | What goes into the Home Assistant Device Builder (remote package pinned to a release tag). |
| `esp-nest-bench.yaml` | Development only: the dev build in render-benchmark mode (the menu pages itself). Never leave it on the device. |
| `secrets.example.yaml` | The secret names (never values). |
| `esp-nest-components/esp_nest/` | The external component. Flat on purpose: ESPHome compiles only top-level sources. |

Component modules so far:

| Module | Thread | Role |
|---|---|---|
| `__init__.py` | — | Schema (display, blinds labels), partition, version and label-glyph guards, IDF components, LVGL 9.6 Kconfig. |
| `nest_core` | loopTask | `EspNest` component: records the loop task, starts `nest_ui` and `nest_led`, reports their start-up (and the LED strip write errors, and once the `nest_led` stack high-water mark), drains the UI event queue (UI → loop, FreeRTOS queue of 64, drops counted and logged); task-ownership assertion macros. |
| `nest_ui_task` | nest_ui | LVGL lifecycle and loop (core 1, prio 5, 16 KB internal stack): `ui_process_pending()` then `lv_timer_handler()`; one 80-line draw buffer, LVGL allocator and log forwarding; DEBUG render statistics while the screen is being redrawn, and the optional render benchmark (see *Rendering performance*). |
| `nest_ui.c` | nest_ui (+ atomic getters / pending setters from any task) | The UI: port of `main/ui.c` to LVGL 9.6 (four menu pages, brightness / fan / blinds / sound detail screens). Cross-task state is C11 atomics; values from outside are applied without a change notification (anti-loop rule). |
| `nest_input` | nest_ui | Encoder (PCNT, accumulating across the 16-bit hardware range) + button (raw GPIO 3); LVGL indev read every 33 ms (native cadence) → `input_fsm` → `ui_encoder_*` + `backlight_update`. |
| `nest_panel` | nest_ui | ST7701S init over 9-bit SPI (software reset + explicit SDIR), RGB panel (one PSRAM framebuffer, 10-line bounce buffers), 180° flush (timed for the render statistics). |
| `nest_backlight` | nest_ui | LEDC PWM + fade, driven by `backlight_policy`. |
| `nest_led_ring` | nest_led | 13× WS2812 on GPIO 4 via `espressif/led_strip` 3.0.3 (RMT 10 MHz, no DMA); task on core 0 (prio 3, 3 KB), which creates the strip so the RMT interrupt stays on core 0 as in native. `main/led_ring.c` logic: 20 ms tick, 3 s breathing at 5–45 % in the page colour, Lights detail tracks the brightness, 600 ms long-press feedback (swell on / double blink off). Inputs are atomics only: the UI's page / detail / brightness, the enabled flag, and the feedback word written by the UI's `ui_led_feedback()` hook (implemented here). Never logs; `nest_core` reports for it. |
| `nest_c.h` | caller's | The component's C interface, implemented in `nest_core`. `NEST_LOGE/W/I/D` log through the ESPHome logger and are gated at compile time by `ESPHOME_LOG_LEVEL` (the `logger:` level), the same as the C++ `ESP_LOGx`. C files must use them and never IDF's `ESP_LOGx`: in a `.c` file those go to the IDF log, whose level (`esp32: framework: log_level`, default ERROR) compiles out everything below an error. Also `NEST_C_ASSERT_UI_TASK()` / `nest_on_ui_task()`, the C form of the UI-task assertion. |
| `nest_lv_assert` | caller's | LVGL's assert handler (via `CONFIG_LV_ASSERT_CUSTOM_INCLUDE`): prints the location on the console and aborts, instead of LVGL's default endless loop on `nest_ui`. |
| `light_sync`, `mqtt_payload`, `fan_sync`, `blinds_sync`, `input_fsm`, `backlight_policy`, `settings_word`, `status_line`, `status_handoff` | caller's | Pure C, host-tested in `test/host/`. |

Threading rule: LVGL and LEDC run only on `nest_ui`, ESPHome APIs only on
loopTask; `NEST_ASSERT_UI_TASK()` / `NEST_ASSERT_LOOP_TASK()` (C++) and
`NEST_C_ASSERT_UI_TASK()` (C, `nest_c.h`) refuse calls from the wrong task.

## Building

```bash
uv tool install esphome==2026.9.1        # same version as the HA Device Builder
esphome config  esphome/esp-nest-dev.yaml
esphome compile esphome/esp-nest-dev.yaml  # -> esphome/.esphome/build/esp-nest/build/firmware.ota.bin
```

The dev entry needs two gitignored files:

- `esphome/secrets.yaml`: the keys listed in `secrets.example.yaml`. It is
  written by `scripts/secrets-decrypt.sh` from
  `private/config/esphome-secrets.yaml.sops` (SOPS + age, the key comes from
  1Password), in the same run that writes the native `secrets.defaults`.
  Change values with `scripts/secrets-edit.sh esphome`, commit the `.sops`
  file in the private overlay, then decrypt again;
- `private/esphome/substitutions.yaml`: the real external topics and blinds
  labels, in the private overlay (`private/` = clone of `esp_nest-private`).

Until the native firmware is removed, the WiFi and MQTT credentials and the
external topics exist twice: in `secrets.defaults` for the native build and
in the two files above. Rotate both copies; `secrets-decrypt.sh` warns when
they have drifted apart — credentials (including a native broker URI whose
scheme or port the ESPHome build, always `mqtt://` on 1883, cannot follow)
and the eight topic/blinds substitutions against `CONFIG_TOPIC_*` — naming
keys only, never values.

`esphome/esp-nest.yaml` builds on its own with the generic defaults; only
the secrets are needed.

### Guards

`esp_nest` refuses a configuration (hard error at `esphome config`) when:

- `esp32: partitions:` is not the repository's `partitions.csv`
  (sha256 `0a9ba053…528a`, hashed with CRLF normalised to LF, so a Windows
  checkout passes; `.gitattributes` also keeps the file LF, and the
  byte-exact check is the `partition-table.bin` sha256 gate in
  `scripts/check.sh`). The device keeps the native partition table and
  OTA cannot change it; this also catches a `/config/partitions.csv` in Home
  Assistant shadowing the package's file;
- `esphome: project: version:` differs from `version.txt`, the single
  version source (the package includes it with `!include ../version.txt`);
- a blinds group label (`blinds_label_a` / `_b` substitutions) is empty or
  only whitespace, has leading or trailing whitespace, or uses a character
  `font_helvetica_100.c` has no glyph for. The glyph set
  is read from the `--range` the font was generated with, so the check
  follows a regenerated font; today it is printable ASCII plus
  `ÁÉÍÓÚÝŠŽáéíóúýšž` (the source face has no `ČĎĚŇŘŤŮ`, see
  `assets/README.md`).

The expected config warning *"OTA encryption does not cover the web_server
OTA platform"* is deliberate: `web_server`'s `POST /update` is the way back
to the native firmware, guarded by basic auth.

### Local gate

`scripts/check.sh` is the repository's CI; the release preflight runs it
(`.release.yml`). Steps, in order (pass step names to run only some):

| Step | What it checks |
|---|---|
| `secrets` | No `*secrets.yaml` / `*secrets.defaults` is tracked, or would be added by `git add -A`. A committed `esphome/secrets.yaml` would also shadow Home Assistant's secrets for the remote package. |
| `host` | `make -C test/host run`: the pure C modules. |
| `esphome` | `esphome config` of `esp-nest.yaml` (dummy secrets built from `secrets.example.yaml`) and of `esp-nest-dev.yaml` (working tree, real secrets, private substitutions; the real `esphome/secrets.yaml` must carry every name of `secrets.example.yaml`); the blinds label validator's rejections (`esphome -s blinds_label_… config` with an unsupported glyph, a whitespace-only label and a leading space must each fail with the validator's message); `esphome compile esp-nest.yaml`; `partition-table.bin` sha256 `81104acc…6830`. Requires exactly ESPHome 2026.9.1. |
| `smoke` | `example-device.yaml` in an empty Home-Assistant-like directory with dummy secrets, its `github://` package swapped for a `file://` clone of the tree: `esphome config` must pass, and `partitions.csv` and the `esp_nest` component must resolve inside the clone (`.esphome/packages/<hash>/`). |
| `native` | The native firmware: `idf.py build` in the `espressif/idf:v5.5.3` container (Docker, or Podman; override with `CONTAINER_ENGINE`), as the invoking user, with the same `partition-table.bin` sha256 gate. |

The gate tests what git would commit (`git add -A`): tracked files plus
untracked, non-ignored ones, copied into a scratch snapshot under
`~/.cache/esp_nest-check/run.*` (on disk; the builds need ~1.5 GB). Gitignored
files — secrets, the private overlay, build state — are invisible there, as in
a clone of the published repository, except where a step needs them by
design: the dev entry is validated in the working tree, and the native build
gets a copy of `secrets.defaults` when it exists. The scratch space is deleted
on exit, also on Ctrl-C; the native build's component downloads are cached in
`~/.cache/esp_nest-check/idf-components`. The repository's `build/` is never
touched. A full run takes about 75 s on petronijus-PC with warm caches; the
first run on a machine also downloads ESPHome's ESP-IDF toolchain and the
container image.

### Home Assistant Device Builder

Paste `example-device.yaml` as the device YAML and put the secrets from
`secrets.example.yaml` into Home Assistant's `secrets.yaml`. The package ref
is always an immutable release tag, never a branch.

## Migration path (verified on hardware)

1. The device keeps its ESP-IDF 5.5.3 bootloader and partition table. The
   ESPHome build uses the repository's byte-identical `partitions.csv`
   (`esp32: partitions:`), so the ESPHome app goes in through the native
   `/update` endpoint: `curl -F firmware=@firmware.ota.bin http://<ip>/update`.
   Upload `firmware.ota.bin` only — never `firmware.factory.bin`.
2. From then on ESPHome's encrypted OTA works (`esphome upload`, Device
   Builder in HA).
3. The way back works too: ESPHome's `web_server` OTA accepts any app image,
   e.g. `curl -u <user>:<password> -F firmware=@native.bin http://<ip>/update`.

Scripted, with the checks the manual commands lack (app image only, ≤ 4 MB,
the right firmware on the target, no placeholder-WiFi rollback image; secrets
from `esphome/secrets.yaml`, never on a command line):

| Script | Does |
|---|---|
| `scripts/migrate-to-esphome.sh <ip> [image]` | Step 1, then waits for ESPHome's web server. |
| `scripts/mqtt-legacy-cleanup.sh <esp_xxxxxx> [--apply]` | Clears the native firmware's retained discovery configs and `avail`, so HA drops the old MQTT device. Dry run without `--apply`. |
| `scripts/rollback-to-native.sh <ip> [image]` | Step 3 with the local native build (`esphome/fallback/native-v2.0.1.bin`). |

After the migration and cleanup, adopt the device in HA (ESPHome
integration, key `esp_nest_api_key`) and add it to the Device Builder.

Safety net: safe mode after 10 failed boots (display and UI are not started
there), fallback AP + captive portal after 10 minutes without WiFi. There is
no bootloader rollback with the current bootloader.

## Gotcha found on hardware: panel state survives MCU resets

The ST7701S has no reset line on this board, so its registers persist across
ESP resets and OTA reboots. An SDIR (X mirror) value left by one firmware
mirrored the image of the next one. `nest_panel` therefore always issues a
software reset (`0x01`) and writes SDIR explicitly in both rotation modes.
The native firmware (`main/screen.c`) does neither — harmless today, but it
would show a mirrored image after a firmware that set SDIR.

## Feasibility spike (2026-10-10)

Two throwaway ESPHome 2026.9.1 builds decided how ESP Nest moves to ESPHome.
Both were flashed to the real device and measured; the device was then
returned to the native firmware (v2.0.1). Their sources (`esphome/spike-a/`,
`esphome/spike-b/`) were removed once `esp_nest` superseded them; they are in
git history at commit `7479120`.

| | spike A | spike B |
|---|---|---|
| Display driver | ESPHome `mipi_rgb`, upstream model `ZX2D10GE01R-V4848` | own ST7701S init + `esp_lcd` RGB panel in an external component (now `nest_panel`) |
| LVGL | ESPHome `lvgl` component (LVGL 9.5), rendered from ESPHome's main loop | LVGL 9.6.0 from the component registry, own FreeRTOS task on core 1, 2 SW draw units |
| 180° rotation | LVGL software rotation (extra PSRAM copy) | ST7701S SDIR mirrors X, flush only reverses row order (`hw_mirror_x: true`), or per-pixel copy like the native firmware (`false`) |
| Encoder | — (rotary_encoder, GPIO interrupts) | PCNT with 1 µs glitch filter, like the native firmware |

Both scenes were identical: a 480×200 edge glow fading in and out
continuously, a 456 px ring, a sweeping 440 px arc and a 140 px label
redrawn every 100 ms with the project's own C-array font.

| Variant | fps | refresh avg | refresh max |
|---|---|---|---|
| A — ESPHome LVGL | **15.1** | 49.4 ms | 76 ms |
| B — own pipeline, per-pixel 180° | 33.6 | 28.3 ms | 46 ms |
| B — own pipeline, SDIR + row flip | **35.0** | 27.1 ms | 45 ms |

- B is 2.3× faster than A. Most of B's gain comes from the configuration
  (`-O2`, LVGL 9.6, FreeRTOS + 2 draw units, larger caches), not from the
  rotation trick, which saves only ~1.2 ms per frame.
- Free memory under B: ~77 KB internal, 6.5 MB PSRAM.
- Encoder via PCNT works (counts arrive in LVGL's indev).
- Colours, orientation and timings are correct with the native init
  sequence and `pclk` not inverted.

**Decision:** variant B — ESPHome provides WiFi, native API, OTA, safe mode
and HA entities; ESP Nest code keeps the display pipeline and UI.

The spike ran LVGL with the C library allocator and draw threads at LVGL's
default priority. `esp_nest` changes both on review findings: LVGL allocates
from PSRAM first (ESPHome's `malloc` is internal-only) and its draw threads
run at priority 1 so they cannot starve loopTask. Step 13 of the plan
re-measures fps and memory with the real UI.

## Rendering performance (2026-10-11)

The spike's 35 fps came from a synthetic scene. The real UI measured far
slower on the first ESPHome build, so the page transition — the animation
that redraws almost the whole screen — was benchmarked on the device and
tuned.

**Method.** `esp-nest-bench.yaml` is the dev build with `esp_nest:
display: benchmark: true`: the UI pages through the menu every 1.5 s on its
own and logs, at DEBUG, one `Render:` line per 2 s window while the screen
is being redrawn (frames, average and maximum refresh time, time spent in
the flush copy, free internal RAM). Every variant below was flashed and
measured for 18 windows. The benchmark build must never stay on the device.

```bash
esphome compile esphome/esp-nest-bench.yaml
esphome upload  esphome/esp-nest-bench.yaml --device <ip>
esphome logs    esphome/esp-nest-bench.yaml --device <ip>   # "Render:" lines
```

| Variant | Refresh per transition frame |
|---|---|
| First migrated build (2 draw units, 2×40-line buffers) | 68 ms |
| One draw unit | 62 ms |
| Single 80-line draw buffer (internal RAM) | 52 ms |
| + 140 px fonts as static A8 (`tools/font_a8.py`, pixel-identical) | **45.5 ms (shipped)** |

Kept: one 80-line buffer, static A8 fonts, two draw units (on par with one
at this buffer size), bounce buffers on.

Measured and rejected:

| Experiment | Result |
|---|---|
| Draw threads at priority 3 / LVGL heap in internal RAM | no change (68 / 66 ms) |
| Code and rodata from flash instead of PSRAM (XIP off) | slower (79 ms) |
| 240-line draw buffer in PSRAM | slower (73 ms) |
| LVGL without OS (no draw threads) | slower (43 vs 40 ms) |
| Flush on a core-0 worker with 2×40 / 2×60 buffers | 51 / 41 ms, 2×60 leaves 19 KB internal RAM |
| No bounce buffers | −3 ms, but the picture sits a few pixels off centre |
| No bounce buffers + `CONFIG_LCD_RGB_RESTART_IN_VSYNC` | −4 ms, but the picture shakes during animation |
| Glows as A8 masks recoloured at draw time | −3 ms, but visible banding: LVGL mixes a mask in 32 steps, the RGB565 glow has 64 levels of green |
| ESP32-S3 PIE (SIMD) kernels for the RGB565 image-with-opacity and colour-through-mask blends, bit-exact with LVGL (self-tested on the device) | −0.5 ms: LVGL 9.6's packed C blend is already about as fast; half the glow blend is waiting on PSRAM reads of the image |

An LVGL profiler pass showed where the remaining time goes: about three
quarters of a frame is `execute_drawing` (roughly 62 draw tasks per frame:
glyph and anti-aliased-edge masks, image setup, blending), about 10 ms is
the copy into the PSRAM framebuffer, and walking the object tree is
negligible. Removing every masked/image blend altogether would only bring a
frame to ~34 ms, which bounds what any blend acceleration can gain.
