#pragma once

/*
 * The component's runtime services for its C translation units (nest_ui.c),
 * implemented in nest_core.cpp.
 *
 * Logging: IDF's ESP_LOGx macros in a C file go to the IDF log, whose level
 * ESPHome sets from `esp32: framework: log_level` (default ERROR), so
 * anything below an error is compiled out. ESPHome's esphome/core/log.h,
 * which routes ESP_LOGx to the ESPHome logger at the `logger:` level, is
 * C++ only. NEST_LOGx is its C counterpart: the same compile-time gating on
 * ESPHOME_LOG_LEVEL (a build flag, visible to C as well) and the same
 * logger, so C and C++ lines share one level, one format and one output.
 *
 * Task ownership: NEST_C_ASSERT_UI_TASK is the C form of nest_core.h's
 * NEST_ASSERT_UI_TASK (docs/esphome-migration-plan.md, section 3).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Logs one line through the ESPHome logger (any task; lines from tasks
/// other than loopTask are queued by the logger). `level` is one of the
/// ESPHOME_LOG_LEVEL_* values. Use the NEST_LOGx macros instead.
void nest_log_c(int level, const char *tag, int line, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

/// True on the recorded nest_ui task (false before it is recorded).
bool nest_on_ui_task(void);

#ifdef __cplusplus
}
#endif

/*
 * The ESPHome log levels, for C, where esphome/core/log.h cannot be
 * included. The build flag is symbolic (-DESPHOME_LOG_LEVEL=
 * ESPHOME_LOG_LEVEL_DEBUG); without these names the preprocessor would read
 * it as 0 and silently drop every line. Same values as log.h, which
 * nest_core.cpp checks with static_asserts; an identical redefinition there
 * is legal.
 */
#ifndef ESPHOME_LOG_LEVEL_NONE
#define ESPHOME_LOG_LEVEL_NONE 0
#endif
#ifndef ESPHOME_LOG_LEVEL_ERROR
#define ESPHOME_LOG_LEVEL_ERROR 1
#endif
#ifndef ESPHOME_LOG_LEVEL_WARN
#define ESPHOME_LOG_LEVEL_WARN 2
#endif
#ifndef ESPHOME_LOG_LEVEL_INFO
#define ESPHOME_LOG_LEVEL_INFO 3
#endif
#ifndef ESPHOME_LOG_LEVEL_CONFIG
#define ESPHOME_LOG_LEVEL_CONFIG 4
#endif
#ifndef ESPHOME_LOG_LEVEL_DEBUG
#define ESPHOME_LOG_LEVEL_DEBUG 5
#endif
#ifndef ESPHOME_LOG_LEVEL_VERBOSE
#define ESPHOME_LOG_LEVEL_VERBOSE 6
#endif
#ifndef ESPHOME_LOG_LEVEL_VERY_VERBOSE
#define ESPHOME_LOG_LEVEL_VERY_VERBOSE 7
#endif

/* log.h's default when no logger is configured. */
#ifndef ESPHOME_LOG_LEVEL
#define ESPHOME_LOG_LEVEL ESPHOME_LOG_LEVEL_NONE
#endif

/* Below the configured level a line compiles to nothing, its arguments
 * unevaluated (as esph_log_x in log.h). */
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_ERROR
#define NEST_LOGE(tag, format, ...) \
    nest_log_c(ESPHOME_LOG_LEVEL_ERROR, tag, __LINE__, format, ##__VA_ARGS__)
#else
#define NEST_LOGE(tag, format, ...) do { } while (0)
#endif

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_WARN
#define NEST_LOGW(tag, format, ...) \
    nest_log_c(ESPHOME_LOG_LEVEL_WARN, tag, __LINE__, format, ##__VA_ARGS__)
#else
#define NEST_LOGW(tag, format, ...) do { } while (0)
#endif

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_INFO
#define NEST_LOGI(tag, format, ...) \
    nest_log_c(ESPHOME_LOG_LEVEL_INFO, tag, __LINE__, format, ##__VA_ARGS__)
#else
#define NEST_LOGI(tag, format, ...) do { } while (0)
#endif

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
#define NEST_LOGD(tag, format, ...) \
    nest_log_c(ESPHOME_LOG_LEVEL_DEBUG, tag, __LINE__, format, ##__VA_ARGS__)
#else
#define NEST_LOGD(tag, format, ...) do { } while (0)
#endif

/*
 * Refuse a call made outside the nest_ui task: log an error and return.
 * Expands inside a function that has a `TAG` in scope; the optional
 * argument is the value returned on refusal (empty for void functions).
 */
#define NEST_C_ASSERT_UI_TASK(...) \
    do { \
        if (!nest_on_ui_task()) { \
            NEST_LOGE(TAG, "%s: called outside the nest_ui task, refused", __func__); \
            return __VA_ARGS__; \
        } \
    } while (0)
