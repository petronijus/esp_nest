# ESPHome migration plan (milestone 1: behaviour parity)

Generated 2026-10-10 from a multi-agent design pass (4 researchers, 1 architect, 2 adversarial critics) and the hardware spike in `esphome/README.md`. External devices stay on direct MQTT exactly as today (Petr's decision); only the knob's own HA entities move to ESPHome's native API.

## Decisions taken

| # | Decision |
|---|---|
| D1 | Autosync disarmed for this repo (`project-sync.sh disarm`); commit `3dd4597` stays in history (no force-push). |
| D2 | Settings (backlight, dim, idle timeout, LED ring) stay RAM-only like native; restore support is built so persistence is a YAML change. |
| D3 | `api.reboot_timeout: 0s`, `mqtt.reboot_timeout: 0s` — the native firmware never reboots for an unreachable HA/broker. |
| D4 | Legacy `espnest/<id>/state|avail|cmd` topics dropped after an inventory of their readers; retained leftovers cleared by `scripts/mqtt-legacy-cleanup.sh`. |
| D5 | Telemetry keeps the 10 s cadence and Free Heap in KB. |
| D6 | `font_helvetica_100` regenerated over ASCII + Czech diacritics so blinds labels are configurable. Done in S08: printable ASCII + `ÁÉÍÓÚÝŠŽáéíóúýšž`; the source face (`assets/helvetica.otf`) has no `ČĎĚŇŘŤŮ`, so the glyph validator rejects those. The 8 original glyphs are byte-identical and none of them has kerning pairs, but the font metrics changed: line_height 75 → 113, base_line 2 → 22 (room for the accents). A centred label's baseline would move 1 px up; `nest_ui.c` compensates (`blinds_label_baseline_fix()`, computed from the v2.0.1 metrics and the current ones), so the ESPHome build draws the labels on the v2.0.1 baseline. The native build shares the font and keeps the frozen `main/ui.c` offsets, so a native image built from this tree draws them 1 px higher than v2.0.1 — an accepted deviation of the fallback build only; photo references come from the v2.0.1 release image. Label validation also rejects whitespace-only labels and leading/trailing whitespace. |
| D7 | Friendly name `ESP Nest`, hostname `esp-nest` → entities `*.esp_nest_*` (keeps `sensor.esp_nest_ip_address`). |
| D8 | ESPHome `mqtt:` component, configured silent (no topic prefix, no discovery, no IP discovery). |
| D9 | Explicit `esp_nest_api_key` secret (the curl /update migration image needs it before HA adopts the device). |
| D10 | Basic auth on `web_server` (`/update`), password on the fallback AP. |
| D11 | After cutover, releases ship no firmware binaries; tags are remote-package refs for HA. |
| D12 | Local gate `scripts/check.sh`; native IDF 5.5.3 build runs in the `espressif/idf:v5.5.3` container — no new hosted CI. |
| D13 | HA Device Builder YAML pins an immutable tag (`@v3.0.0`), never `@main`. |
| D14 | WiFi fallback AP only after `ap_timeout: 10min`, so router reboots reconnect at native speed. |
| D15 | Settings value ranges. The three numbers and their steps/defaults live once, in `settings_word.h` (parsed by `__init__.py`, tied to `backlight_policy` by `_Static_assert`s, host-tested). The Screen Brightness entity advertises 1..100 like native discovery, but the stored range is 0..100 like native `screen_set_backlight()`: a direct API/automation write of 0 blacks the screen exactly as a native MQTT `cmd/backlight` of 0 did; HA's slider cannot send it. The step of 5 is advisory (frontends honour it; the device rounds to whole numbers and does not snap), as in native. A write from outside loopTask is refused with an error log (no corrective publish: publishing is itself loopTask-only); the API and web_server never do this. |


## Review findings folded into the plan

- **[blocker] The rollback path flashes the CI-built v2.0.1 release asset, and that image has no secrets.** Fix: Build the rollback image locally from tag v2.0.1 with the real secrets.defaults on IDF 5.5.3, keep it plus its sha256 in the private overlay (e.g. private/migration/native-v2.0.1-ota.bin), and make rollback-to-native.sh use only that file. Before cutover, prove the round trip on the device: native -> ESPHome -> this rollback image -> it reconnects to WiFi and MQTT. Optionally the script can also refuse an image whose app-desc or strings still contain "myssid".
- **[major] The bridge time base is ESPHome `millis()`, which is uint32 and wraps after 49.7 days.** Fix: Pass `millis_64()` (as int64_t) to light_sync, fan_sync and blinds_sync. Add host tests that run every pure module across now ≈ 2^32 ms. The UI-side modules (input_fsm, backlight_policy) are not uint32 either: per the finding on the time base below, they also take int64 `millis_64()` / `esp_timer_get_time()/1000`, never `lv_tick_get()` or `millis()`.
- **[major] The display/LVGL Kconfig that the design reuses from the spike sets `CONFIG_LV_DEF_REFR_PERIOD=15`, but native uses 33.** Fix: Keep the 15 ms display refresh, but pin the encoder indev read timer to native cadence: call `lv_timer_set_period(lv_indev_get_read_timer(indev), 33)` after creating the indev. Add a scripted fast-spin comparison against native (steps per N counts at high speed) to the S08 verification and the parity checklist.
- **[major] The settings entities never publish their initial state.** Fix: In EspNest::setup, after resolving the values, call `publish_state(value)` on each SettingNumber and `publish_state(initial)` on the switch, the way template number/switch do. Set the native icons in the schema defaults. Add a check to S10: 'HA shows 100/20/30/ON immediately after boot, not Unknown'.
- **[minor] S16 expects the wrong number of legacy topics.** Fix: Write the expected inventory as 9 configs + 1 avail. Keep the script driven by discovery (mosquitto_sub -W) and print the counts found instead of asserting a fixed number.
- **[minor] Rollback can collide on entity_ids, the reverse of the problem the cleanup-before-adopt ordering avoids.** Fix: In the rollback procedure, delete the ESPHome device from HA (which frees the entity_ids) before the native image connects to MQTT, or record that the remap has to be reversed. Put this in the rollback script's printed checklist.
- **[minor] WiFi outage behaviour changes compared with native.** Fix: Make this a deliberate decision and record it. Either raise `ap_timeout` (e.g. 10min) so a short router outage reconnects at native speed, or drop `ap:` and set `wifi: reboot_timeout: 0s` explicitly. Add a 'router off for 2 min' reconnect-time comparison to the S13 checklist.
- **[minor] The partition guard hashes the raw bytes of partitions.csv.** Fix: Add `partitions.csv -text` (or `eol=lf`) to .gitattributes, or hash the CSV after normalising line endings. The partition-table.bin sha gate in check.sh stays the byte-exact check.
- **[minor] Two easy mistakes in the secrets and broker setup can break the production migration image.** Fix: In esp_nest FINAL_VALIDATE, reject a broker containing '://'. Give the test broker a separate entry point or secrets file (e.g. esp-nest-test.yaml with its own secrets), and have migrate-to-esphome.sh or the S16 checklist confirm that the built image's broker is the production host (e.g. grep the dumped config).
- **[major] The LVGL heap can only use internal RAM, so the memory budget does not hold.** Fix: Specify LVGL's allocator now instead of leaving it to S13. Use CONFIG_LV_USE_CUSTOM_MALLOC with lv_malloc_core implemented as heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL) (or the builtin TLSF allocator with a pool from heap_caps_malloc(..., MALLOC_CAP_SPIRAM)). Keep only the draw buffers in internal RAM. Turn on LV_USE_ASSERT_MALLOC and register an lv_log print callback that forwards to ESP_LOGW. NULL-check the draw-buffer allocations. Change the S13 criterion to heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), measured with WiFi, API and MQTT connected during the heaviest page crossfade.
- **[major] LVGL's two software draw threads can starve ESPHome's loopTask, and the S13 fallbacks do not address this.** Fix: Make draw-thread priority part of the design. Set CONFIG_LV_DRAW_THREAD_PRIO to LV_THREAD_PRIO_LOWEST or LOW (FreeRTOS priority 0 or 1, at or below loopTask) and put this knob first in the S13 fallback chain, with nest_ui placement after it. Re-measure fps against the 50 ms loop-gap criterion. Optionally drop to one draw unit if fps stays acceptable.
- **[major] The time base breaks the pure modules after 49.7 days of uptime.** Fix: Pass esphome::millis_64() (or esp_timer_get_time()/1000) as int64_t to every pure module, on both the loop side and the UI side (input_fsm, backlight_policy), never millis(). Add host tests whose timestamps start near 2^32 ms and cross that boundary.
- **[major] The native build breaks at S05: the esp_nest_shared wrapper cannot find LVGL.** Fix: Add `REQUIRES lvgl__lvgl` to the wrapper's register call (or give the wrapper its own idf_component.yml with lvgl/lvgl ~9.2). Have S05's verification run the hosted native build before merging.
- **[minor] The generic package's default blinds labels fail its own glyph validator.** Fix: Decide D6 before S08. Either regenerate font_helvetica_100 with A-Z, or remove the label defaults and have check.sh and CI pass valid labels (for example "Jdl" and "Ka") as substitutions.
- **[minor] Two parts of the check.sh gate do not test what they claim.** Fix: Run the generic config/compile and the smoke test from a `git worktree add` of a temporary commit (for example `git stash create`). That tree has no esphome/secrets.yaml, and dummy secrets go in the wrapper's directory. Document that the gate tests committed content.
- **[minor] How the transition native build gets verified conflicts with project standards.** Fix: Make the native check local and reproducible instead: a check.sh step that runs IDF 5.5.3 through the espressif/idf:v5.5.3 container (docker or podman), or install IDF 5.5.3 and record it in HANDOVER.md. Keep the hosted workflow tag-only, as release.yml is today.

## Target architecture

# ESP Nest on ESPHome 2026.9.1, variant B: target architecture for milestone 1 (behaviour parity)

**Notation.** `E` = `/home/petronijus/.local/share/uv/tools/esphome/lib/python3.12/site-packages/esphome`. Repo paths are relative to `/home/petronijus/Documents/Dev/esp_nest`.

**Guiding rules**
- ESPHome owns the platform: WiFi, native API, OTA, safe_mode, web_server, the MQTT client and the diagnostic entities.
- One external component, `esp_nest`, owns everything device-specific.
- The pure-logic modules stay plain C and are host-tested.
- **Every** ESPHome call (MQTT publish/subscribe, entity `publish_state`, preferences) runs on `loopTask`.
- **Every** LVGL and LEDC call runs on `nest_ui`.
- The only things that cross threads are the hand-off primitives listed in section 3.

## 1. Repo tree (transition state, until the native code is removed)

```
partitions.csv                      # UNCHANGED, shared by both builds. sha256 0a9ba053…528a; generated table.bin sha256 81104acc…6830
version.txt                         # single version source (repo-release bumps it)
assets/                             # design SOURCES moved from main/assets (png/otf/ttf/ttc); not compiled
tools/gen_glow_assets.py            # output path -> esphome/esp-nest-components/esp_nest/
esphome/
  esp-nest.yaml                     # THE package. Complete config with generic substitution defaults (HA remote + local + CI)
  esp-nest-dev.yaml                 # local entry: substitutions from private overlay + packages: !include esp-nest.yaml
  example-device.yaml               # what goes into the HA Device Builder (github:// ref + substitutions)
  secrets.example.yaml              # secret NAMES only
  README.md                         # spike results (kept) + build / migration / rollback procedure
  esp-nest-components/              # distinctive name: a /config/esphome/<same name> folder would shadow it
    esp_nest/                       # FLAT: ESPHome compiles only top-level sources (E/loader.py:184-203)
      __init__.py                   # schema, codegen, IDF deps, LVGL Kconfig, FINAL_VALIDATE guards
      nest_core.h/.cpp              # EspNest : Component (owner, loop-side hand-off, task start)
      nest_settings.h/.cpp          # SettingNumber : number::Number; LedRingSwitch : switch_::Switch
      nest_mqtt_bridge.h/.cpp       # external-device MQTT glue (loopTask only)
      nest_ui_task.h/.cpp           # LVGL lifecycle and the nest_ui task loop
      nest_panel.h/.cpp             # ST7701S 9-bit SPI init (SWRESET + explicit SDIR), RGB panel, flush (from spike)
      nest_input.h/.cpp             # PCNT + raw GPIO button, LVGL indev read_cb -> input_fsm
      nest_backlight.h/.cpp         # LEDC + ledc_fade_func_install, driven by backlight_policy (nest_ui only)
      nest_led_ring.h/.cpp          # espressif/led_strip 3.0.3 task
      nest_ui.h/.c                  # port of main/ui.c, stays C, LVGL 9.6 API
      light_sync.h/.c               # MOVED from main/; .c byte-identical, .h only gains extern "C" guards
      mqtt_payload.h/.c             # pure: hand-rolled lights/fan/blinds payload parsing (exact native semantics)
      fan_sync.h/.c                 # pure: fan echo guard / last-published state
      blinds_sync.h/.c              # pure: debounce, block window, member average, step<->pos
      input_fsm.h/.c                # pure: screen.c:465-523 encoder/button state machine
      backlight_policy.h/.c         # pure: screen.c:526-538, 585-610 idle-dim / setter rules
      font_helvetica_140.c font_helvetica_100.c font_cambria_140.c bg-glow-*.c   # moved assets
components/esp_nest_shared/CMakeLists.txt   # NATIVE-ONLY wrapper (transition): SRCS = light_sync.c + 7 asset .c from esp_nest/
main/ components/button components/mt8901 CMakeLists.txt sdkconfig.defaults main/Kconfig.projbuild   # native, frozen
test/host/                          # MAIN := ../../esphome/esp-nest-components/esp_nest; one test_*.c per pure module
scripts/check.sh                    # local gate (see section 9)
scripts/migrate-to-esphome.sh scripts/rollback-to-native.sh scripts/mqtt-legacy-cleanup.sh
scripts/secrets-decrypt.sh          # also writes esphome/secrets.yaml
```

**Header names.** All new headers carry a `nest_` prefix, or are the pure modules. The native wrapper exposes the `esp_nest/` dir as an include dir, and the prefix keeps it from colliding with `main/ui.h`, `main/screen.h` and `main/led_ring.h`. IDF ignores `SRC_DIRS` whenever `SRCS` is given (`~/.cache/esphome/idf/frameworks/5.5.5/tools/cmake/component.cmake:276-277`), so the wrapper is a separate component that uses `SRCS` only.

**Removed with the spike.** `esphome/spike-a`, `esphome/spike-b` and their `partitions.csv` copies are deleted once `esp_nest` supersedes `nest_display`. Git history keeps them.

## 2. Component structure (`namespace esphome::esp_nest`)

| Unit | Thread | Responsibility |
|---|---|---|
| `EspNest` (Component, `setup_priority::HARDWARE`) | loopTask | See below. |
| `MqttBridge` (plain class owned by `EspNest`) | loopTask | Topics from config; 8× `subscribe(topic, cb, 1)` in `setup`. Connect edge detection. Drives `light_sync` / `fan_sync` / `blinds_sync`, publishes. |
| `SettingNumber` ×3, `LedRingSwitch` (inline entities, sprinkler pattern `E/components/sprinkler/__init__.py:24,259-278`) | loopTask | `control()` / `write_state()` clamps the value, writes the settings atomics, calls `publish_state` and the optional preference save. |
| `nest_ui_task` | nest_ui | See below. |
| `nest_panel` | nest_ui (init) | See below. |
| `nest_input` | nest_ui | indev `read_cb`: PCNT count + raw `gpio_get_level(3)` (native `button_is_pressed` is a raw read too, `components/button/button.c`). Feeds `input_fsm`, then dispatches `ui_encoder_navigate/press/long_press` and the activity flag. |
| `nest_backlight` | nest_ui | LEDC timer/channel and `ledc_fade_func_install` (missing in the spike). Runs `backlight_policy`; always `ledc_fade_stop` before a direct duty set. |
| `nest_ui.c` | nest_ui (+ atomic accessors from any task) | Port of `main/ui.c`. Details below. |
| `nest_led_ring` | nest_led (unpinned, prio 3, 3 KB) | `led_strip` RMT 10 MHz, 13× WS2812 GRB on GPIO4. Logic is `led_ring.c` verbatim (20 ms tick, 3 s breathing 5–45 %, page colours, brightness tracking on page 0 detail, 600 ms feedback); its inputs come only from atomics. |
| Pure modules | caller's thread | No ESP-IDF / ESPHome includes. Time is passed in. Host-tested. |

**`EspNest` (loopTask)**
- `setup()`:
  - records `xTaskGetCurrentTaskHandle()` as the loop task, used by the assertions;
  - resolves the entities' initial or restored values into the settings atomics;
  - creates the UI event queue;
  - starts `nest_ui` and `nest_led`;
  - calls `MqttBridge::setup()`.
- `loop()`:
  - drains UI events;
  - runs the `MqttBridge` tick;
  - refreshes the status atomics;
  - logs queue-drop counters.

**`nest_ui_task` (nest_ui)**
- Initialisation:
  - `backlight_init` with the backlight off;
  - `nest_panel` init;
  - `lv_init`, `lv_tick_set_cb(esp_timer)`;
  - 2× 40-line draw buffers via `heap_caps_malloc(INTERNAL|DMA)` (aligned; the native `static lv_color_t[]` would be rejected by 9.6);
  - indev, `ui_init`, then backlight to the active %.
- Loop: `apply settings` → `apply status` → `ui_process_pending()` → `lv_timer_handler()` → `vTaskDelay(max(next, 1))`.

**`nest_panel` (nest_ui, init only)** — copied from the spike:
- SPI2 9-bit init;
- **SWRESET 0x01 + 120 ms first**;
- native init table;
- **SDIR 0xC7 written explicitly in bank 0x10 in both modes**;
- MADCTL / COLMOD / SLPOUT / DISPON;
- frees the SPI bus;
- `esp_lcd` RGB panel (PLL160M, 15 MHz pclk, native porches, 1 fb in PSRAM, 10-line bounce buffer);
- `flush_cb` with `hw_mirror_x` (SDIR + row flip, default true, verified at 35 fps) or the per-pixel 180° path.

**`nest_ui.c`** — port of `main/ui.c`, compiled as C. As C, LVGL 9.6 flag ORs still compile; in C++ they are a hard error.
- **Deprecated APIs replaced:**
  - `lv_anim_set_playback_duration` → `lv_anim_set_reverse_duration`;
  - `lv_obj_add/clear/has_flag` → `lv_obj_set_hidden` / `set_clickable` / `set_scrollable` / `set_floating` / `lv_obj_is_hidden`.
- **Cross-thread state is C11 `atomic_int`:**
  - `s_brightness`, `s_fan_level`, `s_current_page`, `s_detail_mode`;
  - the pending slots for brightness, fan and blinds[2] (−1 = none, consumed with `atomic_exchange`).
- **New setters:**
  - `nest_ui_set_blinds_labels()`, replacing `CONFIG_TOPIC_BLINDS_LABEL_*`, called before `ui_init`;
  - `nest_ui_set_status_parts()`.
- `notify_change` still goes through `ui_register_change_cb`. The C++ callback only enqueues the event (section 3).

## 3. Threading model and hand-offs

| Task | Core / prio / stack | Owns |
|---|---|---|
| `loopTask` (ESPHome) | core 1 / 1 / ESPHome default (`E/components/esp32/core.cpp:46`) | All ESPHome APIs. MQTT callbacks are dispatched here (`E/components/mqtt/mqtt_backend_esp32.cpp:82-89,189-208`). API and web_server entity control also lands here (web_server uses `DEFER_ACTION`, `E/components/web_server/web_server.cpp:1146`). `light_sync`, `fan_sync`, `blinds_sync`. |
| `nest_ui` | core 1 / 5 / **16 KB internal** (native parity; the spike's 8 KB is too small) | LVGL, panel flush, indev, LEDC / backlight. |
| LVGL draw units ×2 | LVGL OSAL | Rendering only. |
| `nest_led` | unpinned / 3 / 3 KB | RMT LED strip. |
| esp-mqtt task | ESPHome-owned | Network I/O. |

Placement of `nest_ui` (core 1 at prio 5, next to loopTask) is confirmed or changed by the step-13 measurement:
- Criterion: maximum gap between `EspNest::loop()` calls during the heaviest UI animation must be under 50 ms.
- Fallback 1: pin to core 0.
- Fallback 2: drop to prio 2.

**Hand-off structures** (no other shared mutable state):
1. **UI → loop events.** FreeRTOS queue of `struct UiEvent { uint8_t type; int32_t value; }`, depth 64.
   - The UI callback calls `xQueueSend(q, &ev, 0)` and then `App.wake_loop_threadsafe()` (`E/core/application.h:370-372`).
   - If the queue is full, it increments `std::atomic<uint32_t> ui_event_drops`, which the loop logs.
   - The loop drains with `xQueueReceive(q, &ev, 0)` until empty, preserving order. Volume events stay 1:1.
   - A FreeRTOS queue was chosen over ESPHome's `LockFreeQueue`: that one stores pointers and needs an `EventPool` (`E/core/lock_free_queue.h:13-26`), while a FreeRTOS queue copies by value and is IDF-standard.
2. **loop → UI values.** `ui_set_brightness/fan_level/blinds_pos` store into the `nest_ui.c` atomic pending slots; last writer wins.
   - `ui_process_pending` applies them **without** `notify_change`. This is the core anti-loop rule, unchanged.
3. **loop → UI settings.** `NestSettings` atomics: `backlight_pct`, `dim_pct`, `idle_timeout_s`, plus `std::atomic<uint32_t> seq`.
   - The UI task applies a change when `seq` moves, following the native rules:
     - backlight while dimmed: store only;
     - dim while dimmed: apply now;
     - timeout: no reset of the activity timer.
   - `led_enabled` is an `std::atomic<bool>` read by `nest_led`.
4. **loop → UI status.** Atomics `wifi_connected`, `ip_v4` (uint32), `mqtt_connected` and `seq`. The UI task formats the line itself, so no string crosses threads (fixes the native torn `ui_set_status`).
5. **UI → LED / loop snapshot.** The `nest_ui.c` atomics for page, detail and brightness, read by `nest_led`.
   - Fan level and brightness are also read by the loop, for the fan handler (`ha_mqtt.c:347,362`) and for `light_sync_on_state_msg(current)` (`:386`). The native semantics are kept: the loop reads the applied value, not the pending one.
6. **UI → LED feedback.** One `std::atomic<uint32_t>` `(seq << 1) | turned_on`.

**Assertions**
- `NEST_ASSERT_LOOP_TASK()` sits in every `MqttBridge` publish path and in the entity setters. It compares against the recorded handle, logs an error and refuses the call.
- `NEST_ASSERT_UI_TASK()` sits in the `nest_ui_task` / `nest_backlight` entry points.
- `MqttBridge` also never calls `publish()` from MQTT `on_connect`: there `is_connected()` is still false and the publish would be dropped (`E/components/mqtt/mqtt_client.cpp:349,365,536-537`).

## 4. MQTT bridge design (external devices only, semantics byte-for-byte with v2.0.1)

**Client.** ESPHome's `mqtt:` component (`global` client passed in through `cv.use_id(mqtt.MQTTClientComponent)`), configured as:
- `topic_prefix: null`: no birth / will / shutdown / log topics, and every entity's MQTT twin becomes internal (`E/components/mqtt/__init__.py:176-221`, `E/components/mqtt/mqtt_component.cpp:371-375,434-438`);
- `discovery: false` **and** `discover_ip: false` (`E/components/mqtt/__init__.py:389-393`);
- `reboot_timeout: 0s`;
- `clean_session: true` (native: clean session);
- `idf_send_async: true`, so a stalled socket blocks the esp-mqtt task instead of the loop. The single producer is loopTask, which satisfies the SPSC contract (`E/components/mqtt/mqtt_backend_esp32.cpp:210-277`). This is re-verified in step 13; the fallback is `false`.

Rejected alternative: a private esp-mqtt client. It would duplicate an upstream-maintained client and would need its own thread marshalling. Inbound events already arrive on loopTask, and payloads are reassembled (`E/components/mqtt/mqtt_client.cpp:53-68`). Truncation to 255 / 31 / 15 chars is reapplied in `mqtt_payload.c`, so results equal the native "first chunk then truncate" path (255 < 1024-byte chunk).

**Subscriptions** (all QoS1, exact topics, no wildcards; mirrors `ha_mqtt.c:604-618` minus HA-discovery topics):
- `<fan>`;
- `<lights>`;
- 6× `<prefix>/<grp>_{1,2,3}/position/state`.
- Config validation rejects duplicate topics. Native if/else routing gave exactly one handler per message; ESPHome would call every matching subscription.
- **Not subscribed any more:** `homeassistant/status` and `espnest/<id>/cmd/#` (replaced by the API).

**Connect edge.** Detected in `MqttBridge::tick()` as the false→true transition of `is_connected()`. By then ESPHome has already re-sent the subscriptions (`E/components/mqtt/mqtt_client.cpp:349-356`). In native order:
1. `light_sync_init(&ls)`;
2. publish `<lights>/get` = `mqtt_payload_light_get()` (`{"state":""}`, QoS0);
3. `fan_sync_on_connect(&fs)` (`last_pub = -1`, `last_pub_ms = 0`).
- Blinds state is NOT reset (native).
- A disconnect edge only clears `connected`.

The pure modules (built in S03, headers in `esphome/esp-nest-components/esp_nest/`) all take their state first and `int64_t now_ms` second. `now_ms` is `(int64_t) esphome::millis_64()`, one clock for every call. The signatures below are the contract the bridge codes against.

**Inbound (loopTask)**
- **Lights**
  - `mqtt_payload_parse_light(data, len, &st)` → `mqtt_light_state_t` (strstr first-key semantics, quoted state ≤7, strtol brightness, `null` = absent; truncation to 255 happens inside).
  - Then `light_sync_on_state_msg(&ls, now_ms, st.have_state, st.state_on, st.have_brightness, st.brightness, ui_get_brightness())`.
  - If the result is ≥0, `ui_set_brightness(result)`.
- **Fan**
  - `mqtt_payload_parse_fan(data, len)` → 0..3, or -1 for an unknown payload (≤31 chars, exact case-insensitive).
  - The bridge passes the -1 through unchanged: `fan_sync_on_message(&fs, now_ms, parsed_level, ui_get_fan_level())`. The module maps -1 to the current level.
  - If the result is ≥0, `ui_set_fan_level(result)`. **The bridge does not touch `last_pub`**: `fan_sync_on_message` already records the applied level as the last published one (`fan_sync.c`).
- **Blinds member**
  - `mqtt_payload_parse_int(data, len, &pos)` (≤15 chars, `endp` check). On false, drop the message; `blinds_sync` never sees it.
  - Then `blinds_sync_on_member(&bs, now_ms, target, member, pos, NULL)`. It always stores the position. It returns the UI step, or -1 while blocked or queued.
  - If the result is ≥0, `ui_set_blinds_pos(target, result)`. Pass `&avg` instead of NULL to log the averaged position.

**Outbound (drained UI events, loopTask)**
- **BRIGHTNESS**
  - Disconnected: drop and leave `light_sync` untouched.
  - Otherwise: `light_sync_on_local_change(&ls, now_ms, value)`. On `LIGHT_SYNC_ACT_PUBLISH`, `mqtt_payload_light_set(value, buf, sizeof buf)` gives `{"state":"ON","brightness":raw}`, or `{"state":"OFF","brightness":0}` when the value is 0. It goes to `<lights>/set`, QoS0, no retain.
- **FAN**
  - Disconnected: drop.
  - Otherwise: `fan_sync_on_local(&fs, now_ms, level)`. When it returns true, publish `mqtt_payload_fan_label(level)` (`Off`/`low`/`medium`/`high`) to `<fan>`, **QoS0, retain**. The module has already set `last_pub` and its time; the bridge keeps no fan state of its own.
- **VOLUME**
  - Disconnected: drop.
  - Otherwise: `mqtt_payload_volume(dir)` (`UP`/`DOWN`, NULL for 0) to `<stereo>/set`, QoS0.
- **BLINDS**: `blinds_sync_on_local(&bs, now_ms, target, step)`, always. There is no connected check, because native queues across disconnects.

**Tick** (every loop pass)
- **Lights** run only while connected; the bridge gates this, as native `ha_mqtt_process_pending` does. `light_sync_tick(&ls, now_ms, &value)`:
  - `LIGHT_SYNC_ACT_PUBLISH` → `/set` with `mqtt_payload_light_set(value)`;
  - `LIGHT_SYNC_ACT_RESYNC` → `/get` with `mqtt_payload_light_get()`.
- **Blinds**: `int n = blinds_sync_tick(&bs, now_ms, mqtt->is_connected(), out)` with `blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS]`.
  - The module gates on `connected` itself. While disconnected it returns 0 and keeps the queued steps.
  - Each settled group (≥500 ms) yields one command and reopens its block window (`now + 2500`) inside the module.
  - Format each command with `mqtt_payload_blinds_command(&out[i], buf, sizeof buf)`, then publish it, QoS0:
    - `BLINDS_SYNC_CMD_OPEN` / `_CLOSE` (steps 0 / 6) → `OPEN` / `CLOSE` to `<prefix>/<grp>_group/command`, once;
    - `BLINDS_SYNC_CMD_POSITION` (steps 1–5 → `83` / `67` / `50` / `33` / `17`) → the bridge fans this one command out to all three `<prefix>/<grp>_{1,2,3}/position/command` topics.
- The 0–6 step ↔ 0–100 position conversions are `blinds_sync_step_to_pos` / `blinds_sync_pos_to_step` in `blinds_sync`, not in `mqtt_payload`. The bridge never needs them directly.
- Time is `millis_64()`, never the uint32 `millis()` (folded review finding; host tests cross 2^32 ms). Tick cadence follows the loop (≤16 ms, `E/core/application.h:525`), which is within the 300 / 1500 ms constants.

**Removed with the native discovery:**
- `espnest/<id>/state` / `avail` / `cmd`;
- the HA birth handling;
- the device-id topics.

## 5. Entity list (native API; friendly_name `ESP Nest` → entity_ids `*.esp_nest_*`)

| Entity | Source | Config |
|---|---|---|
| WiFi Signal | `sensor: wifi_signal` | `update_interval: 10s`, diagnostic, dBm |
| IP Address | `text_sensor: wifi_info: ip_address` | diagnostic, `mdi:ip-network`; HA domain `sensor` → `sensor.esp_nest_ip_address` |
| Uptime | `sensor: uptime` | `type: seconds`, `update_interval: 10s`, diagnostic, `mdi:timer-outline` |
| Free Heap | `sensor: template` | lambda `esp_get_free_heap_size() / 1024`, unit `KB`, `accuracy_decimals: 0`, 10 s, diagnostic, `mdi:memory` (native: KB **including PSRAM**; `debug:free` would be bytes and internal only) |
| Restart | `button: restart` | config category |
| Screen Brightness | `esp_nest` `SettingNumber` | 1–100, step 5, %, config, initial 100 |
| Dim Brightness | `esp_nest` `SettingNumber` | 0–100, step 5, %, config, initial 20 |
| Idle Timeout | `esp_nest` `SettingNumber` | 0–600, step 5, s, config, initial 30 |
| LED Ring | `esp_nest` `LedRingSwitch` | config, `restore_mode` default `ALWAYS_ON` |

- The device info carries `project_name` `petronijus.esp_nest` and `project_version` = version.txt (`E/components/api/api_connection.cpp:1898-1913`).
- `web_server` provides the configuration URL that native published as `cu`.

## 6. YAML package shape (`esphome/esp-nest.yaml`)

```yaml
substitutions:            # generic public defaults; real values come from HA device YAML / private overlay
  name: esp-nest
  friendly_name: ESP Nest
  topic_lights: zigbee2mqtt/lights
  topic_fan: home/fan
  topic_stereo_vol: home/stereo/volume
  topic_blinds_prefix: blinds/cover
  blinds_group_a: group_a
  blinds_group_b: group_b
  blinds_label_a: "A"     # must pass the glyph validator (see decisions)
  blinds_label_b: "B"
esphome:
  name: ${name}
  friendly_name: ${friendly_name}
  min_version: 2026.9.1
  project: {name: petronijus.esp_nest, version: !include ../version.txt}  # semver X.Y.Z always loads as a string
esp32:
  variant: esp32s3
  flash_size: 16MB
  flash_mode: qio
  cpu_frequency: 240MHz
  partitions: ../partitions.csv    # resolves inside the package clone (cv.file_ declaring-document fallback, E/config_validation.py:1971-2007)
  framework: {type: esp-idf, advanced: {compiler_optimization: PERF}, sdkconfig_options: {<spike-b set>}}
psram: {mode: octal, speed: 80MHz}
logger:
api: {encryption: {key: !secret esp_nest_api_key}, reboot_timeout: 0s}
ota: [{platform: esphome, password or encryption per spike}, {platform: web_server}]
web_server: {port: 80, local: true}
safe_mode:
wifi: {ssid: !secret wifi_ssid, password: !secret wifi_password, power_save_mode: none,
       ap: {ssid: ESP Nest Fallback, password: !secret esp_nest_fallback_password}}
captive_portal:
mqtt: {broker: !secret mqtt_broker, username: !secret mqtt_username, password: !secret mqtt_password,
       topic_prefix: null, discovery: false, discover_ip: false, reboot_timeout: 0s,
       clean_session: true, idf_send_async: true}
external_components: [{source: esp-nest-components}]   # local dir; resolves in clone or working tree
esp_nest:
  display: {hw_mirror_x: true}
  topics: {lights: ${topic_lights}, fan: ${topic_fan}, stereo_volume: ${topic_stereo_vol}}
  blinds: {prefix: ${topic_blinds_prefix},
           group_a: {topic_id: ${blinds_group_a}, label: ${blinds_label_a}},
           group_b: {topic_id: ${blinds_group_b}, label: ${blinds_label_b}}}
  screen_brightness: {name: Screen Brightness}
  dim_brightness: {name: Dim Brightness}
  idle_timeout: {name: Idle Timeout}
  led_ring: {name: LED Ring}
sensor / text_sensor / button: [section 5]
```

**Dev and HA entry points**
- **Dev:** `esp-nest-dev.yaml` has top-level `substitutions: !include ../private/esphome/substitutions.yaml` (top-level beats package defaults, verified) and `packages: {esp_nest: !include esp-nest.yaml}`. Secrets come from the gitignored `esphome/secrets.yaml`, written by `scripts/secrets-decrypt.sh` from SOPS.
- **HA:** `substitutions` with the real topics, plus `packages: {esp_nest: github://petronijus/esp_nest/esphome/esp-nest.yaml@vX.Y.Z}`. Pin a tag, never a branch (refresh lag, `E/git.py:760-775`). `!secret` falls back to HA's secrets.yaml, as long as no `esphome/secrets.yaml` is ever committed; `scripts/check.sh` enforces that.

**`__init__.py` FINAL_VALIDATE guards** (hard errors)
1. `esp32.partitions` must be set to a file whose sha256 == `0a9ba0537d395e1c42f7540a69a1d3b0af9cbc854c0a063b834b54452961528a`. This catches the silent `/config/partitions.csv` shadowing. The on-device table is immutable via OTA, so a constant is the correct reference.
2. `esphome.project.version` must be a string equal to the stripped `version.txt` read via `Path(__file__).parents[3]`.
3. Blinds labels may only use glyphs present in `font_helvetica_100.c`. The set is parsed from the font's `--range` Opts header, so the validator follows font regeneration automatically.
4. External topic strings must be pairwise distinct and valid publish/subscribe topics.

**`to_code`**
- IDF builtins: `include_builtin_idf_component` for `esp_lcd`, `esp_driver_ledc`, `esp_driver_pcnt`, `esp_driver_rmt`, `esp_driver_gpio`, `esp_driver_spi`.
- IDF components: `add_idf_component` `lvgl/lvgl@9.6.0` and `espressif/led_strip@3.0.3`.
- LVGL Kconfig = the spike set plus:
  - `LV_FONT_MONTSERRAT_12`;
  - pinned `LV_USE_ARC` / `LABEL` / `IMAGE`, `LV_DRAW_SW_SUPPORT_RGB565` / `A8`, `LV_DRAW_SW_COMPLEX`, `LV_USE_THEME_DEFAULT`;
  - no Montserrat 24/48, no LODEPNG.
- `AUTO_LOAD = ["number", "switch"]`, `DEPENDENCIES = ["esp32", "psram", "mqtt", "wifi"]`.

## 7. Settings persistence

- Default is parity: RAM-only, booting at 100 / 20 / 30 / ON.
- The mechanism is still built properly:
  - **`SettingNumber`:** supports `restore_value: false|true` via `make_entity_preference<float>()`, loaded in `setup`; `save()` only on loopTask (`E/components/esp32/preferences.cpp:72-90`).
  - **`LedRingSwitch`:** calls `get_initial_state_with_restore_mode()` in `setup`, so `restore_mode: RESTORE_DEFAULT_ON` works if chosen (`E/components/switch/switch.cpp:33-41`).
- `initial_value` defaults are set explicitly. ESPHome's template defaults (min_value / ALWAYS_OFF) are never relied on.

## 8. LED ring

- Stays inside `esp_nest`, coupled to UI page, detail and brightness (parity). It is not an HA light.
- `espressif/led_strip` is pinned at `3.0.3` (the native lock version). `esp_driver_rmt` is included explicitly, because ESPHome excludes it by default (`E/components/esp32/__init__.py:239`).
- The task reads only atomics: page, detail, brightness, enabled, feedback seq.

## 9. Verification, CI and release

**`scripts/check.sh`** — local gate, consistent with the "just ci is the CI" standard. It runs:
- `make -C test/host run`;
- `esphome config` for `esp-nest.yaml` (dummy secrets in a temp copy) and for `esp-nest-dev.yaml`;
- `esphome compile esphome/esp-nest.yaml`;
- the partition gate: sha256 of `.esphome/build/<name>/build/partition_table/partition-table.bin` == `81104acc28f8049debaaeff6a586a5eb5e51cf71f835f95c54ccc09889fe6830`;
- `git ls-files '*secrets.yaml'` must be empty;
- a remote-package smoke test: temp HA dir, `url: file://$REPO`, `ref: HEAD` branch, `files: [esphome/esp-nest.yaml]`, `esphome config`.

**Native build during the transition.** It needs IDF 5.5.3. There is no `~/.espressif` on this machine; ESPHome only ships IDF 5.5.5 in `~/.cache/esphome/idf`. Per D12 and the folded finding, verification runs IDF 5.5.3 in the `espressif/idf:v5.5.3` container. The hosted workflow stays tag-only.

The container build must **never** run in the repo's own `build/`:
- `build/` belongs to the documented host workflow (`source export.sh; idf.py build`).
- A container configure writes `/project` paths into `build/CMakeCache.txt` and `build.ninja`, next to the host-configured `build/bootloader` cache. Both the host and the container builds then fail until the directory is cleaned.
- Run as root, it also leaves root-owned files.

The verification build therefore:
- uses a scratch copy of the tree (`rsync -a --exclude /build --exclude /.git --exclude /.cache --exclude .esphome --exclude /private`, including the gitignored `secrets.defaults`), mounted as `/project`;
- runs `docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v <copy>:/project -w /project espressif/idf:v5.5.3 idf.py build`;
- deletes the copy afterwards, because it holds secrets.

An out-of-tree `-B` build dir outside the repo is an acceptable alternative. It still needs `--user`.

**`.release.yml`.** `preflight_extra` gains `scripts/check.sh`. `expect_assets` stays native until cutover. `release.yml` gains an ESPHome compile + partition gate job.

**Releases.** CI images carry no secrets and can never be the migration image. That image is built locally from the release tag with the real secrets, as `firmware.ota.bin`.

## 10. Migration ops and rollback

**Migration order**
1. Release tag `v3.0.0`.
2. Build `firmware.ota.bin` locally from the tag with `esp-nest-dev.yaml`.
3. `scripts/migrate-to-esphome.sh <ip> <bin>`:
   - checks the target serves `ESP Nest OTA Update`;
   - checks the image (esptool `image_info`: esp32s3 app, app-desc project `esp-nest`, size < 0x400000; never `factory.bin`);
   - runs `curl -F firmware=@… /update`;
   - waits for TCP 6053.
4. `scripts/mqtt-legacy-cleanup.sh <device_id>`:
   - discovers retained `homeassistant/+/<id>/+/config` and `espnest/<id>/#` via `mosquitto_sub -W`;
   - dry-run by default;
   - `--apply` publishes empty retained payloads (`-r -n`).
   - This must happen **before** adopting the API device in HA, so the new entity_ids do not get `_2` suffixes.
5. Adopt the ESPHome device in HA (API key).
6. Verify the entity_ids and remap the inventoried automations / dashboards.
7. Create the HA Device Builder YAML (remote package @v3.0.0 + substitutions).
8. Do the first Device Builder OTA.

**Rollback** — `scripts/rollback-to-native.sh <ip>`:
- downloads the v2.0.1 `-ota.bin` via `gh release download` and checks it against the release's `sha256sums.txt`;
- verifies the target is ESPHome web_server;
- POSTs to `/update`.
- Native then republishes its discovery on connect. Disable the ESPHome device in HA.
- **Boot-loop safety net:**
  - ESPHome `safe_mode` after 10 quick reboots (UI never started there, because the safe_mode check runs before component setup);
  - fallback AP + captive portal.
  - There is no bootloader rollback (native IDF 5.5.3 bootloader stays).

## 11. Repo hygiene noticed

- `.cache/` (935 clangd index files) and `test/host/test_light_sync.dSYM` are tracked; untrack them.
- Commit `3dd4597 autosync(main): omarchy` was written into this repo by the ai-config autosync hook and is already on `origin/main` (see decisions).

## Implementation steps

### S01 — Repo hygiene: untrack clangd cache and dSYM

*Depends on:* —  
*Files:* `.gitignore`, `.cache/ (git rm --cached, 935 files)`, `test/host/test_light_sync.dSYM/ (git rm --cached)`

Untrack `.cache/` and `test/host/test_light_sync.dSYM/` with `git rm -r --cached`, keeping the local files. Add the ignore rules `.cache/` and `test/host/*.dSYM/`. No source changes.

**Verification:** `git ls-files .cache test/host | grep -c -e '^.cache/' -e dSYM` prints 0. `make -C test/host run` prints '0 failures'.

### S02 — Move light_sync into the esp_nest component dir, shared by both builds

*Depends on:* S01  
*Files:* `main/light_sync.c -> esphome/esp-nest-components/esp_nest/light_sync.c (git mv)`, `main/light_sync.h -> esphome/esp-nest-components/esp_nest/light_sync.h (git mv)`, `components/esp_nest_shared/CMakeLists.txt (new, native-only wrapper)`, `main/CMakeLists.txt`, `test/host/Makefile`, `test/host/test_light_sync.c`

**Move:** `git mv` both light_sync files. `light_sync.c` stays byte-identical.

**Header:** wrap the declarations in `light_sync.h` with `#ifdef __cplusplus extern "C" { #endif`, since it is now included from C++. No other edits.

**Native wrapper** (`components/esp_nest_shared/CMakeLists.txt`):
- `set(D ${CMAKE_CURRENT_LIST_DIR}/../../esphome/esp-nest-components/esp_nest)`
- `idf_component_register(SRCS ${D}/light_sync.c INCLUDE_DIRS ${D})`
- Use SRCS only, because IDF ignores SRC_DIRS when SRCS is set.

**Native main:** `main/CMakeLists.txt` REQUIRES gains `esp_nest_shared`. `SRC_DIRS` stays `"." "assets"` until S05.

**Host tests:** `test/host/Makefile` sets `MAIN := ../../esphome/esp-nest-components/esp_nest`. `test_light_sync.c` gains a scenario that starts at now = 2^32 − 1000 ms and crosses 2^32. It covers the 300 ms throttle spanning the boundary, a 1.5 s window that drops everything up to its last millisecond, exactly one `/get` when the window closes, the unconditional apply after it, and idle ticks past the boundary. This is the folded finding's 2^32 guard for `light_sync`; `light_sync.c` is unchanged.

The dir has no `__init__.py` yet, so ESPHome ignores it.

**Verification:** - `make -C test/host run` reports 0 failures, all 13 scenarios.
- `git diff -M --stat` shows `light_sync.c` as R100.
- The `light_sync.c` sha256 is unchanged.
- The native build succeeds on IDF 5.5.3 (CI job or petronijus-PC: `idf.py build`).

### S03 — Extract the external-MQTT logic into pure C modules with host tests

*Depends on:* S02  
*Files:* `esphome/esp-nest-components/esp_nest/mqtt_payload.c/.h`, `esphome/esp-nest-components/esp_nest/fan_sync.c/.h`, `esphome/esp-nest-components/esp_nest/blinds_sync.c/.h`, `test/host/check.h`, `test/host/test_mqtt_payload.c`, `test/host/test_fan_sync.c`, `test/host/test_blinds_sync.c`, `test/host/test_light_sync.c`, `test/host/Makefile`, `.gitignore`

Specs come from `main/ha_mqtt.c` v2.0.1. Native `ha_mqtt.c` is NOT modified (frozen).

All three modules take their state first and `int64_t now_ms` second (never a uint32 `millis()`). The signatures below are the ones built; S11 codes against them (section 4 shows how the bridge calls them).

**`mqtt_payload`** (no state, no time)
- `void mqtt_payload_parse_light(const char *data, size_t len, mqtt_light_state_t *out)`:
  - truncate to 255; an embedded NUL ends the payload;
  - key lookup via strstr, first occurrence anywhere;
  - quoted state ≤7 chars; `state_on` = case-insensitive "ON";
  - brightness via `strtol` base 10; `null` means absent (`:99-137, :369-395`);
  - `out` has `have_state`, `state_on`, `state[8]`, `have_brightness`, `brightness` (raw, -1 if absent).
- `int mqtt_payload_parse_fan(const char *data, size_t len)`: truncate to 31, exact case-insensitive `Off`/`low`/`medium`/`high` → 0..3, else -1.
- `bool mqtt_payload_parse_int(const char *data, size_t len, int *out)`: truncate to 15, `strtol` with the `endp==msg` reject, saturated to the int range (32-bit `long` semantics on a 64-bit host).
- Formatting:
  - `const char *mqtt_payload_fan_label(int level)`;
  - `int mqtt_payload_light_set(int pct, char *buf, size_t bufsz)`: OFF sends brightness 0; raw via `light_sync_pct_to_raw`;
  - `const char *mqtt_payload_light_get(void)`;
  - `const char *mqtt_payload_volume(int dir)`;
  - `int mqtt_payload_blinds_command(const blinds_sync_cmd_t *cmd, char *buf, size_t bufsz)`.
  - `MQTT_PAYLOAD_OUT_SIZE` (64) fits every formatted payload.
- The step ↔ position conversions are **not** here; they live in `blinds_sync`.

**`fan_sync`** (`:193-204, :340-367, :643-644`)
- State `fan_sync_t`: `last_pub` (-1 none) and `int64_t last_pub_ms` (0 none). `void fan_sync_init(fan_sync_t *fs)`.
- `void fan_sync_on_connect(fan_sync_t *fs)`: -1 / 0.
- `bool fan_sync_on_local(fan_sync_t *fs, int64_t now_ms, int level)`:
  - returns true when the caller must publish;
  - records `last_pub` and `last_pub_ms` itself.
- `int fan_sync_on_message(fan_sync_t *fs, int64_t now_ms, int parsed_level, int current_level)`:
  - -1 maps to `current_level`;
  - guard `last_pub_ms>0 && now-last_pub_ms<1000 && level==last_pub`;
  - returns the level to apply if it differs from `current_level`, else -1;
  - records an applied level as `last_pub` itself, so the caller never sets it.

**`blinds_sync`** (`:207-217, :219-291, :736-755`)
- State `blinds_sync_t`:
  - `member_pos[2][3]`, init -1;
  - `pending_step[2]`;
  - `int64_t change_ms[2]` and `int64_t block_until_ms[2]`.
  - `void blinds_sync_init(blinds_sync_t *bs)`.
- `bool blinds_sync_on_local(blinds_sync_t *bs, int64_t now_ms, int target, int step)`:
  - no connected check;
  - queues the newest step; `block = now+2500`;
  - false for an invalid target or step.
- `int blinds_sync_tick(blinds_sync_t *bs, int64_t now_ms, bool connected, blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS])`:
  - returns 0 and changes nothing while `!connected`;
  - otherwise flushes every group settled for ≥500 ms into `out[]`, in group order;
  - each command is `{target, kind OPEN/CLOSE/POSITION, position}`;
  - reopens `block = now+2500`;
  - returns the count.
  - One POSITION command stands for all three member topics. The bridge fans it out.
- `int blinds_sync_on_member(blinds_sync_t *bs, int64_t now_ms, int target, int member, int position, int *out_avg)`:
  - always stores the position;
  - returns -1 while blocked or pending, with no known member, or for an invalid target or member;
  - otherwise takes the truncating average over known (≥0) members and returns its step (`*out_avg` receives the average).
- `int blinds_sync_step_to_pos(int step)` / `int blinds_sync_pos_to_step(int pos)` (`:207-217`).

**Tests**
- One scenario per native behaviour, including:
  - nested "state" first match, 255 truncation cutting a key, `127.5`→127;
  - fan echo guard, including the rapid low→medium jump-back, and an unknown fan payload;
  - blinds queued across disconnect, positions stored while blocked, negative member positions ignored, OPEN/CLOSE vs x3.
- One 2^32 scenario per module (folded finding):
  - every interval straddles 2^32 ms;
  - for `blinds_sync`, block windows open before the boundary, with member reports on both sides of it;
  - a uint32-truncated time base must fail these tests.
- Makefile:
  - generic pattern rule, one binary per module;
  - output goes to `test/host/build/`, the only ignored path there;
  - `run` executes all of them.
- Shared `test/host/check.h` (CHECK / CHECK_EQ / SCENARIO / summary); `test_light_sync.c` moves to it as well.
- `-Wall -Wextra -Werror`, no ESP includes.

**Verification:**
- `make -C test/host run` prints 0 failures for all test binaries.
- `grep -L` confirms no `esp_`/`freertos` includes in the new modules.

### S04 — Pure input state machine and backlight policy with host tests

*Depends on:* S03  
*Files:* `esphome/esp-nest-components/esp_nest/input_fsm.c/.h`, `esphome/esp-nest-components/esp_nest/backlight_policy.c/.h`, `test/host/test_input_fsm.c`, `test/host/test_backlight_policy.c`, `test/host/Makefile`

**`input_fsm`** — from `screen.c:465-523`.
- Input: `now_ms`, `count_diff`, `pressed`, `detail`.
- Outputs: `navigate(n)` (detail: raw diff; menu: ±3 accumulation then 600 ms cooldown), `press` (on release, held >50 ms, no long press), `long_press` (once at >1000 ms held), `activity`.
- On release: accumulator reset and a 300 ms cooldown.

**`backlight_policy`** — from `screen.c:526-538, 585-610`.
- State: `active`, `dim`, `timeout_ms`, `dimmed`, `last_activity`.
- `update(now_ms, activity)`: the single per-indev-read entry point; activity and the idle check are mutually exclusive within one read, as in native's `if (activity) … else if (idle elapsed) …`:
  - activity: restart the idle timer; if dimmed, SET(active) instantly; never FADE, even when the timeout elapsed before this read;
  - no activity: when not dimmed and timeout>0 and elapsed ≥ timeout, FADE(dim, 1000).
- `set_active`: clamp 0..100; SET only if not dimmed.
- `set_dim`: clamp; SET if dimmed.
- `set_timeout`: clamp 0..600 s; never resets the activity timer.

Both are pure C with time passed in.

**Verification:** `make -C test/host run` prints 0 failures. The scenarios cover cooldown windows, long press without a short press, 50 ms debounce, timeout 0 = never dim, a read with activity after the timeout elapsed (SET when dimmed, NONE otherwise, never FADE), the dim-while-dimmed rules, and timestamps crossing 2^32 ms.

### S05 — Relocate UI assets (single copy) and design sources

*Depends on:* S02  
*Files:* `main/assets/*.c -> esphome/esp-nest-components/esp_nest/ (git mv, 7 files)`, `main/assets/*.png,*.ttf,*.otf,*.ttc -> assets/ (git mv)`, `components/esp_nest_shared/CMakeLists.txt`, `main/CMakeLists.txt`, `tools/gen_glow_assets.py`, `README.md (asset regeneration notes, if documented there)`

**Compiled assets:** `git mv` the 4 `bg-glow-*.c` and 3 `font_*.c` files flat into `esp_nest/`. ESPHome compiles only top-level sources (`E/loader.py:184-203`).

**Design sources:** move them to repo-root `assets/`.

**Native build:**
- The native wrapper adds the 7 `.c` files to SRCS.
- `main/CMakeLists.txt` SRC_DIRS becomes `"."` only.

**Tooling:** update the input/output paths in `tools/gen_glow_assets.py`. Document the `lv_font_conv` invocations, which keep their existing Opts.

The file contents are unchanged. The Opts comments still name `main/assets`; that is generated metadata and is left alone.

**Verification:** - `git diff -M` shows R100 for all 7 `.c` files.
- The native build succeeds on IDF 5.5.3.
- The native map shows the same rodata sizes for the glow, font and helvetica symbols.
- `python tools/gen_glow_assets.py` regenerates byte-identical files.

### S06 — esp_nest component skeleton, display pipeline and the package YAMLs

*Depends on:* S02  
*Files:* `esphome/esp-nest-components/esp_nest/__init__.py`, `esphome/esp-nest-components/esp_nest/nest_core.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_panel.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_ui_task.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_backlight.h/.cpp`, `esphome/esp-nest.yaml`, `esphome/esp-nest-dev.yaml`, `esphome/example-device.yaml`, `esphome/secrets.example.yaml`, `esphome/README.md`, `esphome/spike-a/ (delete)`, `esphome/spike-b/ (delete)`

**`__init__.py`**
- Schema in this step: `display.hw_mirror_x`.
- FINAL_VALIDATE guards:
  - partition CSV sha256 `0a9ba053…`, required;
  - `project.version` == `version.txt`.
- `to_code`: the IDF builtins, `lvgl/lvgl@9.6.0`, and the LVGL Kconfig set (spike + `MONTSERRAT_12` + pinned widgets; no 24/48/LODEPNG).

**C++ modules**
- `nest_panel.cpp`: spike code with SWRESET + explicit SDIR in both modes, RGB panel, both flush paths.
- `nest_backlight.cpp`: LEDC + `ledc_fade_func_install` + `backlight_policy` (defaults 100/20/30 come from constants until S10).
- `nest_ui_task.cpp`:
  - 16 KB stack on core 1, prio 5;
  - aligned `heap_caps` draw buffers;
  - `lv_tick_set_cb`;
  - loop `lv_timer_handler`;
  - the screen is a black screen until S08.
- `nest_core.cpp`: `EspNest` setup records the loop task handle and starts the UI task; the assertion macros live here.

**Package** (`esp-nest.yaml`, as in architecture section 6, without `mqtt:` and without the esp_nest topics yet):
- `api reboot_timeout 0s`;
- ota esphome + web_server, `web_server port 80`;
- `safe_mode`;
- `wifi` with the AP password secret, `captive_portal`;
- the diagnostic entities: wifi_signal 10 s, uptime 10 s, ip, the Free Heap KB template sensor, the restart button.

**Other YAML and docs**
- `esp-nest-dev.yaml`: top-level substitutions from `private/esphome/substitutions.yaml` plus `packages: !include`.
- `example-device.yaml`: `github://…@vX.Y.Z` + substitutions.
- `esphome/README.md`: keep the spike results and update the paths and build commands.
- Delete both spike dirs in this step; their content is superseded and stays in git history.

**Verification:** - `esphome config` passes for `esphome/esp-nest.yaml` (dummy secrets) and for `esp-nest-dev.yaml`.
- `esphome compile esphome/esp-nest.yaml` succeeds.
- The partition-table.bin sha256 equals `81104acc…6830`.
- **Guard negative tests:** temporarily point `partitions` at a modified copy, and set a mismatched version; `config` must fail with the guard message.
- **Hardware:**
  - flash via `curl /update` from native;
  - backlight on, no garbage frame;
  - API connects, encrypted esphome OTA works;
  - the diagnostic entities update every 10 s;
  - returning to native via web_server `/update` works.

### S07 — Dev tooling: secrets, private substitutions, local gate

*Depends on:* S06  
*Files:* `scripts/secrets-decrypt.sh`, `scripts/check.sh (new)`, `private/config/esphome-secrets.yaml.sops (private repo)`, `private/esphome/substitutions.yaml (private repo)`, `.release.yml (preflight_extra += scripts/check.sh)`

**`secrets-decrypt.sh`**: additionally decrypts `private/config/esphome-secrets.yaml.sops` into the gitignored `esphome/secrets.yaml`, using the same 1Password age key flow. Secret names:
- `wifi_ssid`, `wifi_password`;
- `esp_nest_api_key`, `esp_nest_fallback_password`;
- `mqtt_broker`, `mqtt_username`, `mqtt_password`;
- `web_server` credentials, if decision D10 = auth.

**Private overlay:** `private/esphome/substitutions.yaml` holds the real topics and labels, taken from `secrets.defaults` `CONFIG_TOPIC_*`. Committed in esp_nest-private, which is a separate repo commit.

**`scripts/check.sh`** runs the gate from architecture section 9:
- host tests;
- the native IDF 5.5.3 container build, isolated as section 9 requires: a scratch copy (or `-B` outside the repo), `--user $(id -u):$(id -g)`, never the repo's `build/`; the copy is deleted on exit (trap), since it contains `secrets.defaults`;
- esphome config dev + generic;
- generic compile;
- the partition-table.bin sha gate;
- the `*secrets.yaml` tracked check;
- the `file://` remote-package smoke test in a temp HA-like dir with its own dummy `secrets.yaml`.

**Verification:** - `scripts/check.sh` exits 0 on a clean tree.
- Committing a dummy `esphome/secrets.yaml` to a scratch branch makes it fail.
- The remote-package smoke test resolves `partitions.csv` and the components from the clone (log shows `.esphome/packages/<hash>`).

### S08 — UI port, input and idle dimming on the UI task; UI events reach the loop

*Depends on:* S04, S05, S06  
*Files:* `esphome/esp-nest-components/esp_nest/nest_ui.c/.h`, `esphome/esp-nest-components/esp_nest/nest_input.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_ui_task.cpp`, `esphome/esp-nest-components/esp_nest/nest_backlight.cpp`, `esphome/esp-nest-components/esp_nest/nest_core.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_c.h`, `esphome/esp-nest-components/esp_nest/__init__.py`

**`nest_ui.c`** — a copy of `main/ui.c` (native frozen) with:
- LVGL 9.6 API replacements: `reverse_duration`; per-flag setters for add/clear/has_flag;
- C11 atomics for `s_brightness`, `s_fan_level`, `s_current_page`, `s_detail_mode` and the pending slots; consumption via `atomic_exchange(-1)`;
- blinds labels via `nest_ui_set_blinds_labels`;
- logging through `NEST_LOGx` from `nest_c.h` (C shim to the ESPHome logger, implemented in `nest_core.cpp`): IDF's `ESP_LOGx` in a C file is gated by the IDF log level, ERROR by default, and ESPHome's redirecting `log.h` is C++ only;
- `NEST_C_ASSERT_UI_TASK()` (`nest_c.h`, backed by `nest_on_ui_task()`) at the top of `ui_init`, `ui_set_status`, the `ui_encoder_*` handlers and `ui_process_pending`;
- `led_ring_feedback` replaced by a weak-free C hook declared in `nest_ui.h` and implemented in `nest_led_ring.cpp` (S09). Until then it is implemented in `nest_core.cpp` as the atomic feedback-seq writer, so no stub is needed later.

**`nest_input.cpp`**: PCNT (glitch 1 µs, native edge/level actions), raw GPIO3 with pull-up, indev `read_cb` → `input_fsm` → `ui_encoder_*` + `backlight_policy_update(…, out.activity)` (one call per read, no separate tick).
- **Time base:** the `nest_input` `read_cb` (for both `input_fsm_update` and `backlight_policy_update`) and every `nest_backlight` call take `now_ms` from `(int64_t) esphome::millis_64()` (or `esp_timer_get_time()/1000`), never from `lv_tick_get()` or `millis()`. Both are uint32 and wrap after 49.7 days, and the implicit widening to `int64_t` compiles silently.

**`nest_core`:**
- FreeRTOS queue `UiEvent` (depth 64);
- the `ui_register_change_cb` callback does `xQueueSend(…,0)` + `App.wake_loop_threadsafe()`, with a drop counter;
- `EspNest::loop` drains and logs events at DEBUG (bridge arrives in S11).

**Config:** blinds labels enter the schema here with the glyph validator.

**Verification:** - Compile.
- **Hardware vs native:** photos of every page, detail screen and the status area via the immich-latest-photo flow:
  - orientation and colours match;
  - menu needs 3 counts + 600 ms cooldown;
  - brightness ±2 %/count;
  - fan 3 counts/level;
  - volume 2 counts / ≥120 ms;
  - blinds 2 counts/step;
  - short press, long press;
  - detail auto-return after 60 s;
  - idle dim after 30 s to 20 % with a 1 s fade, instant wake.
  - blinds labels on the v2.0.1 baseline (decision D6: the regenerated font is compensated in `nest_ui.c`; compare against the v2.0.1 release image).
- Logs show one event per action, in order, with zero drops. The UI's own INFO lines (`esp_nest.ui`: "Entered/Exited … mode", "Long press: …", "Blinds … -> …", "Detail screen idle timeout …", "Menu idle timeout …", "UI initialized …") go through the ESPHome logger (`NEST_LOGx`, `nest_c.h`) and are the log evidence for the steps above; `strings` on `firmware.elf` must find them.

### S09 — LED ring module

*Depends on:* S08  
*Files:* `esphome/esp-nest-components/esp_nest/nest_led_ring.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_core.cpp`, `esphome/esp-nest-components/esp_nest/__init__.py`

**Build:** `include_builtin_idf_component('esp_driver_rmt')` and `add_idf_component('espressif/led_strip', ref='3.0.3')`.

**Hardware config:** RMT 10 MHz without DMA; 13× WS2812 GRB on GPIO4; cleared at start.

**Task:** unpinned, prio 3, 3 KB. Logic copied from `led_ring.c`: 20 ms tick, 3 s breathing at 5–45 %, page colours, page-0 detail tracks brightness, 600 ms swell / double-blink feedback.

*Done in S09, deliberate changes:* the task is **pinned to core 0** and creates the strip itself. The RMT driver allocates its interrupt on the core that creates the channel; native created it from `app_main` (core 0), away from the RGB panel's bounce-buffer interrupt on core 1. Under ESPHome, `EspNest::setup()` runs on loopTask (core 1), so creating it there (or from an unpinned task) would move the non-DMA RMT refill interrupt onto the panel's core or make its placement random. The task's own code never logs, but the led_strip/RMT driver calls it makes log each failure with `ESP_LOGE` through the ESPHome logger (`esp_log_set_vprintf`), formatted on the task's 3 KB stack (native ran the same path on 2 KB). After a failed frame the task holds writes back for 1 s, so a persistent fault logs about once a second, not every 20 ms tick. `EspNest::loop` reports its start-up, the stack high-water mark 10 s after start, and strip write errors rate-limited: the first at once, then at most one summary per 10 s (delta and total, plus the current stack high-water mark), with a warning status that clears after 10 s without a new error. A test build with `esphome: build_flags: [-DNEST_LED_INJECT_WRITE_FAULT]` adds one out-of-range pixel per frame, which the driver rejects with an `ESP_LOGE`, to measure the error path's stack use. The breathing phase is kept modulo the 3 s period instead of native's uint32 tick counter, which jumps after 49.7 days. The feedback flavour and request counter are one atomic word (native: two volatiles, could tear). `led_ring_set_enabled()` is the enabled atomic the S10 switch writes; until then the ring is always on (native boot default).

*Second deliberate deviation:* only the order relative to the UI is native. Native started the ring last in `app_main`, after the WiFi connect attempt (up to 30 s), the OTA server and MQTT, so it stayed dark during network bring-up; here `EspNest::setup()` starts it right after the UI task, so it breathes during WiFi/API/MQTT bring-up.

**Inputs:** only from atomics (page, detail, brightness, enabled, feedback seq). The S08 feedback hook writes the atomic that this task consumes.

**Verification:** Compile. On hardware, compare against native:
- breathing colour per page;
- brightness tracking in the light detail;
- long-press toggle feedback (on: swell; off: double blink);
- no flicker during heavy UI animation;
- `uxTaskGetStackHighWaterMark` logged once;
- error path: one test build with `-DNEST_LED_INJECT_WRITE_FAULT` — the first write-error report is immediate, later ones come at most every 10 s, the driver's `ESP_LOGE` about once a second, the warning status is set, and the stack high-water mark in the reports keeps a safe margin (otherwise raise `LED_TASK_STACK_BYTES`).

### S10 — Settings entities: three numbers and the LED ring switch via the native API

*Depends on:* S09  
*Files:* `esphome/esp-nest-components/esp_nest/nest_settings.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_core.cpp`, `esphome/esp-nest-components/esp_nest/nest_backlight.cpp`, `esphome/esp-nest-components/esp_nest/__init__.py`, `esphome/esp-nest.yaml`

**Schema** (sprinkler pattern, `AUTO_LOAD` number/switch):
- `screen_brightness`: 1–100 step 5 %, initial 100;
- `dim_brightness`: 0–100 step 5 %, initial 20;
- `idle_timeout`: 0–600 step 5 s, initial 30;
- all config category, `restore_value` default false;
- `led_ring`: switch, `restore_mode` default `ALWAYS_ON`.

**Entity behaviour:**
- `SettingNumber::control()`, on loopTask: clamp like native → store atomic + bump seq → `publish_state` → `pref.save()` if restoring.
- `LedRingSwitch::write_state()` → atomic → `publish_state`.

**Setup:** `EspNest::setup()` loads the initial or restored values into the atomics before starting the tasks. The switch calls `get_initial_state_with_restore_mode()`.

**UI side:** `nest_ui_task` applies settings changes via `backlight_policy`, with `ledc_fade_stop` before any direct duty set. This fixes the native cross-task LEDC race without changing the observable rules.

**Verification:** - Compile.
- From HA (or `aioesphomeapi`), on hardware:
  - backlight change while active applies instantly;
  - backlight change while dimmed is stored and applied on wake;
  - dim change while dimmed applies instantly;
  - timeout 0 never dims;
  - a timeout change does not reset the timer;
  - LED ring off goes black within 100 ms;
  - after a reboot: 100/20/30/ON (parity default).

### S11 — MQTT bridge for the external devices

*Depends on:* S03, S08  
*Files:* `esphome/esp-nest-components/esp_nest/nest_mqtt_bridge.h/.cpp`, `esphome/esp-nest-components/esp_nest/nest_core.cpp`, `esphome/esp-nest-components/esp_nest/__init__.py`, `esphome/esp-nest.yaml`, `private/esphome/substitutions.yaml (private repo)`

**Package:** add the `mqtt:` block — `topic_prefix: null`, `discovery: false`, `discover_ip: false`, `reboot_timeout: 0s`, `clean_session: true`, `idf_send_async: true`, plus the broker/user/pass secrets.

**Schema:** topics (lights, fan, stereo_volume), blinds prefix and group ids, `cv.use_id(mqtt.MQTTClientComponent)`, and the distinct-topic validator. `DEPENDENCIES` gains `mqtt`.

**`MqttBridge`:**
- 8× `subscribe(…, 1)` in `setup`;
- owns one `light_sync_t`, `fan_sync_t` and `blinds_sync_t` and calls them with the S03 signatures: state first, then `int64_t now_ms = millis_64()` (section 4 lists every call);
- connect-edge detection in `tick()`: `light_sync_init` → `/get` → `fan_sync_on_connect`;
- inbound callbacks → `mqtt_payload` / `light_sync` / `fan_sync` / `blinds_sync` → `ui_set_*`. The bridge keeps no sync state of its own; in particular it never sets the fan `last_pub`, because `fan_sync_on_local` / `fan_sync_on_message` record it;
- outbound from the drained `UiEvent`s per architecture section 4, including the disconnected-drop rules (light, fan and volume dropped; blinds queued);
- tick: `light_sync_tick` only while connected; `blinds_sync_tick(&bs, now_ms, is_connected(), out)` every pass, with each `BLINDS_SYNC_CMD_POSITION` in `out[]` fanned out to the group's three `/position/command` topics and OPEN/CLOSE sent once to `<grp>_group/command`;
- `NEST_ASSERT_LOOP_TASK` on every publish.

**Label defaults:** the default labels must pass the glyph validator. Depending on decision D6, either the font is regenerated or the package labels become required substitutions.

**Verification:** Compile + host tests. On hardware against a **local mosquitto** on the dev machine (dev substitutions point at it, so stopping the broker is safe), with `mosquitto_sub -v -t '#'` recording and scripted `mosquitto_pub` replies:
- /get sent on every connect;
- /set throttled at 300 ms; all state dropped inside 1.5 s windows; one /get after the window;
- OFF sends brightness 0;
- fan retained publish with the 1 s identical-echo guard;
- volume UP/DOWN 1:1;
- blinds settle 500 ms, OPEN/CLOSE vs x3 member commands, 2.5 s block; queued across a broker stop/start, not lost;
- light/fan/volume dropped while disconnected;
- no topic under `esp-nest/…`, `esphome/discover` or `homeassistant/…` ever published.

### S12 — Status line from ESPHome state

*Depends on:* S11  
*Files:* `esphome/esp-nest-components/esp_nest/nest_core.cpp`, `esphome/esp-nest-components/esp_nest/nest_ui_task.cpp`, `esphome/esp-nest-components/esp_nest/nest_ui.c/.h`

**Loop side:** `EspNest::loop` writes the atomics `wifi_connected` (`wifi::global_wifi_component->is_connected()`), the IPv4 address and `mqtt_connected`, and bumps seq on change.

**UI side:** `nest_ui_task` formats the native strings:
- `Starting...` before the first state;
- `WiFi: disconnected | v<ver>`;
- `IP: <ip> | MQTT: offline | v<ver>`;
- `IP: <ip> | MQTT: ok | v<ver>`.

`<ver>` is the project version define. Edge transitions are logged like the native monitor (`WiFi restored/lost`, `MQTT restored/lost`). The status label setter is called only from the UI task.

**Verification:** Compile. On hardware: the strings are identical to native in each state (unplug the broker, AP off), and transitions show within ~1 s.

### S13 — Parity verification and resource tuning on hardware

*Depends on:* S10, S12, S07  
*Files:* `esphome/README.md (results)`, `private/migration/notes.md (trace artefacts, private repo)`, `esphome/esp-nest-components/esp_nest/nest_ui_task.cpp / __init__.py (only if tuning is required)`

Run the full parity checklist side by side: native v2.0.1 rebuilt against the local broker vs ESPHome dev build. The same scripted interaction and reply scenarios run on both. Then diff the normalized MQTT traces: topic, payload, qos, retain and relative timing buckets.

**Measurements:**
- fps with the real UI;
- the maximum gap between `EspNest::loop` calls during the heaviest animation (criterion: under 50 ms; else move `nest_ui` to core 0, then drop it to prio 2, and re-measure);
- internal and PSRAM free memory at peak (criterion: ≥40 KB internal; else switch the LVGL heap configuration and re-measure);
- stack high-water marks for `nest_ui`, `nest_led` and loopTask;
- `idf_send_async` behaviour under a broker stall.

Record the results in `esphome/README.md`.

**Verification:** - Every item on the parity checklist is marked pass with evidence (trace diff empty except the intended removals: discovery, `espnest/*`).
- The acceptance criteria above are met.
- 24 h soak on the dev build without reboot, without `Dropped N inbound MQTT events` and with zero UI event drops.

### S14 — Documentation and cross-machine handover

*Depends on:* S13  
*Files:* `CLAUDE.md`, `README.md`, `esphome/README.md`, `HANDOVER.md`

**`CLAUDE.md`:**
- dual-build architecture during the transition, and the module table for `esp_nest`;
- the threading and hand-off rules (LVGL only on `nest_ui`, ESPHome only on loopTask);
- light-sync rules unchanged, new paths: the `light_sync.c/.h` row currently under "Architecture (`main/`)" moves to the `esp_nest` module table (`esphome/esp-nest-components/esp_nest/`, built natively via `components/esp_nest_shared` until S05), and the "Any change to `light_sync.c` needs a scenario test" rule points at the new path;
- `esphome compile esphome/esp-nest-dev.yaml`; the migration and rollback scripts; `scripts/check.sh`.

**`README.md`:** user-facing build and flash instructions.

**`HANDOVER.md`:** per-OS steps for MacBook, petronijus-PC and Windows: `uv tool install esphome==2026.9.1`, `scripts/secrets-decrypt.sh` producing `esphome/secrets.yaml`, the private overlay pull. Plus one AI-handover Todoist task per affected OS (project `6gv39jPGWHQ4gqjV`; allowed handover case).

**Verification:** Docs review. On a second machine, follow the HANDOVER steps: `scripts/check.sh` passes there.

### S15 — Release v3.0.0 (ESPHome package tag; native assets still shipped)

*Depends on:* S14  
*Files:* `.github/workflows/release.yml (ESPHome compile + partition gate job)`, `.release.yml`, `version.txt / CHANGELOG.md (by repo-release only)`

**`release.yml`:** add an ESPHome job — uv install pinned esphome, dummy secrets, compile `esphome/esp-nest.yaml`, partition sha gate. The native job is unchanged during the transition.

**Cut the release** with the repo-release skill only (no manual tag or CHANGELOG edits). The tag becomes the immutable ref for the HA remote package. `version.txt` bump → the `!include` makes `project.version` follow it automatically; the guard verifies it.

**Verification:** - The repo-release preflight, including `scripts/check.sh`, passes.
- The release workflow is green.
- `esphome config` on `example-device.yaml` with `@v3.0.0` from a scratch HA-like dir resolves the tag.

### S16 — Production migration ops: native to ESPHome on the real device

*Depends on:* S15  
*Files:* `scripts/migrate-to-esphome.sh (new)`, `scripts/rollback-to-native.sh (new)`, `scripts/mqtt-legacy-cleanup.sh (new)`, `private/migration/notes.md (private repo)`, `private/esphome/device.yaml (copy of the HA Device Builder YAML, private repo)`

**Scripts:** written and committed first.

**Before cutover:** inventory the HA references to the old MQTT entity_ids (automations, scripts, dashboards via the HA API; token via `op-cache "Homeassistant API Token" credential`) and record the old→new mapping.

**Cutover:**
1. `git checkout v3.0.0`; `esphome compile esphome/esp-nest-dev.yaml`.
2. `scripts/migrate-to-esphome.sh <ip> esphome/.esphome/build/esp-nest/build/firmware.ota.bin`.
3. `scripts/mqtt-legacy-cleanup.sh esp_xxxxxx` — dry-run, then `--apply`. It clears the 10 retained `homeassistant/*/<id>/*/config` topics and the retained `espnest/<id>/avail`.
4. Confirm the HA MQTT device 'Nest Hub' is gone.
5. Adopt the ESPHome device in HA with the API key.
6. Verify the entity_ids (`sensor.esp_nest_ip_address` etc.) and remap the inventoried references.
7. Create the Device Builder YAML (`example-device.yaml` + real substitutions; secrets in HA `secrets.yaml`).
8. Run one Device Builder OTA to prove the HA build path.

**Rollback:** `scripts/rollback-to-native.sh <ip>`, then disable the ESPHome device in HA (native republishes its discovery).

**Verification:** - **After migration:** all checklist items re-checked on the production broker with real Z2M, fan, stereo and blinds.
- HA shows exactly one ESP Nest device (API), with no MQTT ghost.
- `mosquitto_sub -v -t 'homeassistant/+/esp_xxxxxx/#' -W 5` returns nothing.
- The Device Builder OTA succeeds.
- The rollback script has been dry-run tested against the dev setup.

### S17 — Remove native firmware (separate later step, after parity soak)

*Depends on:* S16  
*Files:* `main/ (delete)`, `components/button, components/mt8901, components/esp_nest_shared (delete)`, `CMakeLists.txt, sdkconfig.defaults, secrets.defaults.example (delete)`, `scripts/ota-update.sh (delete or replace per release decision)`, `scripts/secrets-decrypt.sh (drop secrets.defaults output)`, `.github/workflows/release.yml (drop native job)`, `.release.yml (expect_assets per decision D11)`, `CLAUDE.md, README.md, .gitignore`

Done only after S16 plus an agreed soak period. It is one `refactor!:` commit.

**Keep:** `partitions.csv` at the root (guarded), `version.txt`, `test/host`. **Delete:** the native sources and build files listed above.

**Docs:** rewrite CLAUDE.md for ESPHome only, keeping the light-sync design section verbatim. `esphome/fallback/` stays gitignored. The next release follows the repo-release flow.

**Verification:** - `scripts/check.sh` passes.
- A fresh clone + `esphome config` succeeds.
- `git grep -n 'CONFIG_TOPIC_\|idf.py'` finds only historical CHANGELOG entries.
- Release workflow green.


## Parity checklist

- [ ] **Boot appearance:** panel orientation and colours identical to native, no mirrored image after any prior firmware (SWRESET + explicit SDIR), no garbage frame (backlight off until init).
- [ ] **Boot defaults:** backlight 100 %, dim 20 %, idle timeout 30 s, LED ring ON, UI brightness 30, fan 0, Lights page selected.
- [ ] **Encoder, menu mode:** ±3 counts = one page step, then a 600 ms cooldown; clockwise direction as native.
- [ ] **Encoder, detail mode:** raw diff passed through; brightness ±2 %/count with clockwise increasing; fan 3 counts per level; volume 2 counts per command, at least 120 ms apart, dropped steps also not animated; blinds 2 counts per step, clockwise closes.
- [ ] **Button:** short press fires on release when held more than 50 ms and no long press fired. Long press fires once at more than 1000 ms held. Release resets the accumulator and applies a 300 ms cooldown. Raw GPIO3 level, active low.
- [ ] **Long-press toggles:** brightness 0 ↔ last non-zero (default 100) on menu page 0 and in the brightness detail. Fan 0 ↔ last non-zero (default low). Entering the brightness detail at 0 sets the last non-zero value and publishes.
- [ ] **Blinds detail:** short press switches group A/B, long press exits. Labels come from config (native Kconfig `TOPIC_BLINDS_LABEL_*`) and render with font_helvetica_100, on the same baseline as the v2.0.1 release image (compare against v2.0.1 itself, not a native build from this tree, which draws them 1 px higher — decision D6).
- [ ] **Detail idle:** after 60 s the UI returns to the menu, then to the Lights page; no publish.
- [ ] **Idle dimming:** any encoder count or press restores the active level instantly. After the idle timeout the screen fades to the dim level over 1000 ms. Timeout 0 means never dim.
- [ ] **Setting changes:** a backlight change while dimmed is only stored; a dim-level change while dimmed applies at once; an idle-timeout change does not reset the activity timer. Values are clamped (0–100, 0–600).
- [ ] **LED ring:** colour per page (#FFE600 / #00E5CC / #FB4A04 / #DE04FF), 3 s breathing at 5–45 %, intensity tracks brightness on page 0 detail. 600 ms feedback: swell for on, double blink for off. When disabled, all LEDs are black within 100 ms.
- [ ] **Status line:** `WiFi: disconnected | v<ver>` / `IP: <ip> | MQTT: offline | v<ver>` / `IP: <ip> | MQTT: ok | v<ver>`, where <ver> is the version.txt value.
- [ ] **External MQTT subscriptions:** exactly 8 topics at QoS1 — `<fan>`, `<lights>`, 6× `<prefix>/<grp>_{1,2,3}/position/state`. Clean session. No `homeassistant/status`, no `espnest/#`.
- [ ] **On every MQTT (re)connect:** `light_sync` re-initialised, `<lights>/get` published with `{"state":""}` QoS0, fan last-published state reset to -1 / 0. Blinds state not reset.
- [ ] **Lights /set:** payload `{"state":"ON","brightness":<raw>}` with raw=(pct*255+50)/100, or `{"state":"OFF","brightness":0}`. QoS0, no retain. At most one publish per 300 ms; the newest value is kept pending and flushed by tick. A value equal to synced cancels the pending one.
- [ ] **Authority window:** every publish (and every throttled pending change) opens or extends a 1.5 s window. ALL incoming lights state inside it, or while a value is pending, is dropped. Exactly one `/get` after the window closes; `/get` only follows a publish.
- [ ] **Lights state parsing:** payload truncated to 255 chars. First occurrence of the `"state"` / `"brightness"` keys anywhere wins. State must be a quoted string of at most 7 chars; case-insensitive ON, anything else is OFF. Brightness via strtol (127.5→127); `null` means absent. State wins over brightness; a bare ON at 0 restores 100. synced is updated even when unchanged.
- [ ] **Light change while MQTT is disconnected:** lost, and light_sync is not touched. Z2M state wins after reconnect via /get.
- [ ] **Fan publish:** on a local change with level ≠ last published, publish `Off`/`low`/`medium`/`high` to `<fan>` at QoS0 with **retain**, not throttled. Dropped while disconnected.
- [ ] **Fan inbound:** payload truncated to 31 chars, exact case-insensitive match; an unknown payload maps to the current level. Dropped when last publish was less than 1 s ago (and >0) AND level == last published. Otherwise applied only if it differs from the UI, and last published is set to it. The retained value overrides the UI on reconnect; the rapid-stepping jump-back is preserved as-is.
- [ ] **Volume:** `<stereo>/set` with `UP` / `DOWN`, QoS0, one per UI bump. Dropped while disconnected, but the UI still animates.
- [ ] **Blinds commands:** the newest step is queued with no connected check. Flushed after ≥500 ms of settle, only while connected. Step 0 → `<prefix>/<grp>_group/command` OPEN, step 6 → CLOSE. Steps 1–5 → 83/67/50/33/17 sent to all three `<prefix>/<grp>_N/position/command`, QoS0. block_until = now+2500 both when queued and when flushed. Queued commands survive a disconnect.
- [ ] **Blinds inbound:** payload truncated to 15 chars and parsed with strtol (non-numeric is ignored). The member position is always stored. No UI update while blocked or pending. Otherwise the truncating average of the known (≥0) members gives step = ((100-clamp(avg))*6+50)/100. No resync after the block ends.
- [ ] **Anti-loop:** values applied from MQTT (brightness, fan, blinds) never cause a publish.
- [ ] **HA entities via the native API:** WiFi Signal (dBm, 10 s), IP Address, Uptime (s, 10 s), Free Heap (KB including PSRAM, 10 s), Restart button, Screen Brightness (1–100 step 5 %), Dim Brightness (0–100 step 5 %), Idle Timeout (0–600 step 5 s), LED Ring switch. Settings are not persisted unless decided otherwise.
- [ ] **No reboot on lost connectivity:** the device never reboots because the broker or HA is unreachable (api and mqtt reboot_timeout 0s).
- [ ] **Firmware upload:** `curl -F firmware=@<app>.bin http://<ip>/update` works (web_server OTA), in both directions; partition table byte-identical (CSV sha256 0a9ba053…, table.bin sha256 81104acc…).
- [ ] **Legacy MQTT footprint gone after cutover:** no retained `homeassistant/{sensor,button,number,switch}/<id>/*/config` and no `espnest/<id>/avail`. The firmware publishes no ESPHome birth/will/status/debug/discovery topics.
