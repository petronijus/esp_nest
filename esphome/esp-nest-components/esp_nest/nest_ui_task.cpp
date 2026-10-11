#include "nest_ui_task.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "backlight_policy.h"
#include "nest_backlight.h"
#include "nest_core.h"
#include "nest_input.h"
#include "nest_panel.h"
#include "nest_settings.h"
#include "nest_ui.h"
#include "status_handoff.h"
#include "status_line.h"

/*
 * LVGL heap (folded review finding "the LVGL heap can only use internal
 * RAM"): ESPHome's psram component sets CONFIG_SPIRAM_USE_CAPS_ALLOC, so a
 * plain malloc() never returns PSRAM. LVGL is therefore built with
 * LV_USE_CUSTOM_MALLOC (see __init__.py) and allocates from PSRAM first,
 * falling back to internal RAM; only the draw buffers below are forced
 * into internal RAM.
 */
#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "esp_nest provides LVGL's allocator: CONFIG_LV_USE_CUSTOM_MALLOC must be set (see __init__.py)"
#endif

/*
 * LVGL asserts (LV_USE_ASSERT_MALLOC / _NULL) must abort, not spin on
 * nest_ui: nest_lv_assert.h, pulled in by lv_conf_internal.h.
 */
#ifndef NEST_LV_ASSERT_HANDLER_INSTALLED
#error "esp_nest's LVGL assert handler is missing: CONFIG_LV_ASSERT_CUSTOM_INCLUDE must name nest_lv_assert.h (see __init__.py)"
#endif

static constexpr uint32_t LVGL_HEAP_PREFERRED = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static constexpr uint32_t LVGL_HEAP_FALLBACK = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

extern "C" {

// LVGL shares the system heap: there is nothing to set up or tear down.
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

// Extra pools are not supported by the system heap (same as LVGL's clib port).
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return nullptr;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) { return heap_caps_malloc_prefer(size, 2, LVGL_HEAP_PREFERRED, LVGL_HEAP_FALLBACK); }

void *lv_realloc_core(void *p, size_t new_size) {
  return heap_caps_realloc_prefer(p, new_size, 2, LVGL_HEAP_PREFERRED, LVGL_HEAP_FALLBACK);
}

void lv_free_core(void *p) { heap_caps_free(p); }

// Reports the byte-addressable system heap (internal + PSRAM) LVGL draws from.
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  const size_t total = info.total_free_bytes + info.total_allocated_bytes;
  mon_p->total_size = total;
  mon_p->free_cnt = info.free_blocks;
  mon_p->free_size = info.total_free_bytes;
  mon_p->free_biggest_size = info.largest_free_block;
  mon_p->used_cnt = info.allocated_blocks;
  mon_p->max_used = total - info.minimum_free_bytes;
  mon_p->used_pct = total > 0 ? static_cast<uint8_t>(100 - info.total_free_bytes * 100 / total) : 0;
  mon_p->frag_pct =
      info.total_free_bytes > 0 ? static_cast<uint8_t>(100 - info.largest_free_block * 100 / info.total_free_bytes) : 0;
}

lv_result_t lv_mem_test_core(void) { return heap_caps_check_integrity_all(true) ? LV_RESULT_OK : LV_RESULT_INVALID; }

}  // extern "C"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.ui";

/// One partial-mode draw buffer of 80 lines in internal DMA-capable RAM
/// (LVGL 9.6 needs LV_DRAW_BUF_ALIGN-aligned buffers, which a static array
/// does not give). Measured on hardware (esphome/README.md): rendering the
/// screen in fewer, taller areas beats double buffering — 2x40 lines cost
/// 16 ms more per page-transition frame — because every object crossing an
/// area boundary is drawn once per area. The flush is a synchronous copy,
/// so a second buffer would not overlap anything; a flush worker on the
/// other core was measured too and gained nothing.
static constexpr int32_t DRAW_BUF_LINES = 80;
static constexpr uint32_t DRAW_BUF_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

/*
 * The firmware version on the status line: the project version, which the
 * component's config validation ties to version.txt (see __init__.py).
 */
#ifndef ESPHOME_PROJECT_VERSION
#error "esp_nest needs 'esphome: project:' for the status line version (see __init__.py)"
#endif

static UiTaskConfig s_config{};
static std::atomic<UiTaskState> s_state{UiTaskState::NOT_STARTED};
/// The settings nest_ui has handed to the backlight (nest_ui only).
static Settings s_applied_settings{};

int64_t ui_now_ms() { return esp_timer_get_time() / 1000; }

UiTaskState ui_task_state() { return s_state.load(std::memory_order_acquire); }

// LVGL's own tick is uint32 and wrap-safe inside LVGL (lv_tick_elaps); the
// pure modules never see it.
static uint32_t lvgl_tick_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

static void lvgl_log_cb(lv_log_level_t level, const char *buf) {
  size_t len = std::strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    len--;
  if (level == LV_LOG_LEVEL_ERROR) {
    ESP_LOGE(TAG, "LVGL: %.*s", static_cast<int>(len), buf);
  } else {
    ESP_LOGW(TAG, "LVGL: %.*s", static_cast<int>(len), buf);
  }
}

// ---------------------------------------------------------------------------
// Render statistics (nest_ui only): frames and refresh durations per window,
// logged at DEBUG while the screen is being redrawn, silent when static.
// ---------------------------------------------------------------------------

static constexpr int64_t RENDER_STATS_WINDOW_US = 2000 * 1000;

struct RenderStats {
  int64_t window_start_us{0};
  int64_t refr_start_us{0};
  int64_t refr_total_us{0};
  int64_t refr_max_us{0};
  uint32_t frames{0};
};
static RenderStats s_render{};

static void render_stats_event_cb(lv_event_t *e) {
  const int64_t now = esp_timer_get_time();
  if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
    if (s_render.frames == 0)
      s_render.window_start_us = now;
    s_render.refr_start_us = now;
    return;
  }
  const int64_t dt = now - s_render.refr_start_us;
  s_render.frames++;
  s_render.refr_total_us += dt;
  if (dt > s_render.refr_max_us)
    s_render.refr_max_us = dt;
}

/// Logs and resets the window once it is full; called from the task loop.
static void render_stats_report() {
  if (s_render.frames == 0)
    return;
  const int64_t elapsed = esp_timer_get_time() - s_render.window_start_us;
  if (elapsed < RENDER_STATS_WINDOW_US)
    return;
  const int64_t flush_us = panel_take_flush_time_us();
  ESP_LOGD(TAG,
           "Render: %u frames in %lld ms (%.1f fps), refresh avg %.1f ms (flush %.1f ms), max %.1f ms, "
           "internal free %u B",
           static_cast<unsigned>(s_render.frames), static_cast<long long>(elapsed / 1000),
           s_render.frames * 1e6f / static_cast<float>(elapsed), s_render.refr_total_us / 1000.0f / s_render.frames,
           flush_us / 1000.0f / s_render.frames, s_render.refr_max_us / 1000.0f,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  s_render = RenderStats{};
}

#ifdef USE_ESP_NEST_UI_BENCHMARK
/// Benchmark mode: one menu step every 1.5 s, always inside the menu.
static void benchmark_timer_cb(lv_timer_t * /*timer*/) {
  if (!ui_is_detail_mode())
    ui_encoder_navigate(1);
}
#endif

/// LVGL core, display and draw buffers.
static esp_err_t lvgl_init() {
  lv_init();
  lv_log_register_print_cb(lvgl_log_cb);
  lv_tick_set_cb(lvgl_tick_ms);

  const size_t buf_bytes = static_cast<size_t>(PANEL_H_RES) * DRAW_BUF_LINES * sizeof(uint16_t);
  void *buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, buf_bytes, DRAW_BUF_CAPS);
  lv_display_t *disp = buf != nullptr ? lv_display_create(PANEL_H_RES, PANEL_V_RES) : nullptr;
  if (disp == nullptr) {
    ESP_LOGE(TAG, "Out of memory for the display: %u B draw buffer, largest free internal DMA block %u B",
             static_cast<unsigned>(buf_bytes), static_cast<unsigned>(heap_caps_get_largest_free_block(DRAW_BUF_CAPS)));
    heap_caps_free(buf);
    lv_deinit();
    return ESP_ERR_NO_MEM;
  }
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, panel_flush_cb);
  lv_display_set_buffers(disp, buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(disp, render_stats_event_cb, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(disp, render_stats_event_cb, LV_EVENT_REFR_READY, nullptr);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Hand-off 4: loop -> UI status (a seqlock)
// ---------------------------------------------------------------------------

// The seqlock itself is the pure module status_handoff (host-tested).
static status_handoff_t s_status = STATUS_HANDOFF_INIT;

void ui_status_publish(const UiStatus &status) {
  NEST_ASSERT_LOOP_TASK();
  status_handoff_publish(&s_status, {status.wifi_connected, status.ip_v4, status.mqtt_connected});
}

/// Shows a newly published status on the status line. A publish in
/// progress, or one that lands while reading, is left for the next pass
/// (a few milliseconds later).
static void apply_status() {
  status_snapshot_t status;
  if (!status_handoff_take(&s_status, &status))
    return;

  char line[STATUS_LINE_SIZE];
  status_line_format(line, sizeof line, status.wifi_connected, status.ip_v4, status.mqtt_connected,
                     ESPHOME_PROJECT_VERSION);
  ui_set_status(line);
}

/// Hand-off 3: applies the settings entities' changes with the native
/// rules, which backlight_policy owns (the active level applies at once
/// unless dimmed, the dim level only while dimmed, a timeout change does
/// not restart the idle timer). The snapshot is consistent; a changed
/// field is a changed setting.
static void apply_settings() {
  const Settings settings = settings_load();
  if (settings == s_applied_settings)
    return;
  if (settings.backlight_pct != s_applied_settings.backlight_pct)
    backlight_set_active(settings.backlight_pct);
  if (settings.dim_pct != s_applied_settings.dim_pct)
    backlight_set_dim(settings.dim_pct);
  if (settings.idle_timeout_s != s_applied_settings.idle_timeout_s)
    backlight_set_timeout(settings.idle_timeout_s);
  s_applied_settings = settings;
}

/// Hardware, LVGL, the knob and the UI, then the first frame and the
/// backlight.
static esp_err_t task_init() {
  // Backlight first and off: init garbage stays invisible (native order).
  esp_err_t err = backlight_init();
  if (err == ESP_OK)
    err = panel_init(s_config.hw_mirror_x);
  if (err == ESP_OK)
    err = lvgl_init();
  if (err == ESP_OK)
    err = input_init();
  if (err != ESP_OK)
    return err;

  ui_init();
  // Native: the first status, until the connection state is known.
  ui_set_status(STATUS_LINE_STARTING);

  err = input_create_indev();
  if (err != ESP_OK)
    return err;

  // Render the first frame into the framebuffer now, so the backlight
  // never shows what the panel held before.
  lv_refr_now(lv_display_get_default());
#ifdef USE_ESP_NEST_UI_BENCHMARK
  ESP_LOGW(TAG, "Render benchmark mode: the menu pages itself every 1.5 s");
  lv_timer_create(benchmark_timer_cb, 1500, nullptr);
#endif

  // The boot values the settings entities resolved in EspNest::setup()
  // (native defaults 100 / 20 / 30 unless restored). Started before the
  // first indev read, which drives the idle policy.
  s_applied_settings = settings_load();
  backlight_start(ui_now_ms(), s_applied_settings.backlight_pct, s_applied_settings.dim_pct,
                  s_applied_settings.idle_timeout_s);
  return ESP_OK;
}

static void ui_task_main(void * /*arg*/) {
  register_ui_task();

  const esp_err_t err = task_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UI initialisation failed: %s", esp_err_to_name(err));
    s_state.store(UiTaskState::FAILED, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "nest_ui running on core %d, %u B internal / %u B PSRAM free", xPortGetCoreID(),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  s_state.store(UiTaskState::RUNNING, std::memory_order_release);

  for (;;) {
    // Values, settings and status from loopTask first (hand-offs 2 to 4), then
    // LVGL: timers, animations, the indev read and rendering.
    apply_settings();
    apply_status();
    ui_process_pending();
    uint32_t next_ms = lv_timer_handler();
    render_stats_report();
    if (next_ms == LV_NO_TIMER_READY)
      next_ms = LV_DEF_REFR_PERIOD;
    if (next_ms < 1)
      next_ms = 1;
    vTaskDelay(pdMS_TO_TICKS(next_ms));
  }
}

void ui_task_start(const UiTaskConfig &config) {
  NEST_ASSERT_LOOP_TASK();
  if (ui_task_state() != UiTaskState::NOT_STARTED) {
    ESP_LOGE(TAG, "nest_ui already started");
    return;
  }

  s_config = config;  // published to the task by xTaskCreate
  s_state.store(UiTaskState::STARTING, std::memory_order_release);
  // Internal-RAM stack (the default for xTaskCreate*), size in bytes on ESP-IDF.
  if (xTaskCreatePinnedToCore(ui_task_main, "nest_ui", UI_TASK_STACK_BYTES, nullptr, UI_TASK_PRIORITY, nullptr,
                              UI_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Could not create the nest_ui task (%u B stack)", static_cast<unsigned>(UI_TASK_STACK_BYTES));
    s_state.store(UiTaskState::FAILED, std::memory_order_release);
  }
}

}  // namespace esphome::esp_nest
