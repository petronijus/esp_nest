#pragma once

/*
 * LVGL assert handler, included by LVGL's lv_conf_internal.h through
 * CONFIG_LV_ASSERT_USE_CUSTOM_INCLUDE / CONFIG_LV_ASSERT_CUSTOM_INCLUDE
 * (set in __init__.py to this header's absolute path in the build tree).
 *
 * LVGL's default handler is `while(1);`. On nest_ui (core 1, priority 5)
 * that spins forever and starves loopTask (core 1, priority 1). The assert
 * message LVGL logs just before the handler goes through the ESPHome logger,
 * which only queues messages from other tasks until loopTask drains them, so
 * the message would never be printed and the task watchdog would reset the
 * device with no LVGL cause in any log. This handler prints the assert
 * location synchronously and aborts instead: the panic output names it, and
 * ESPHome's crash handler and safe mode see a crash.
 *
 * Included into every LVGL translation unit, assembler sources too, and into
 * C and C++: no includes, and the declaration is hidden from the assembler.
 */

#ifndef __ASSEMBLER__

#ifdef __cplusplus
extern "C" {
#endif

/// Prints "LVGL assertion failed at <file>:<line>" to the console without
/// going through the logger, then calls esp_system_abort().
__attribute__((noreturn)) void nest_lv_assert_failed(const char *file, int line);

#ifdef __cplusplus
}
#endif

/* LVGL expands this as a statement right after LV_LOG_ERROR (lv_assert.h). */
#define LV_ASSERT_HANDLER nest_lv_assert_failed(__FILE__, __LINE__);

/* Lets the component check at compile time that LVGL picked this up. */
#define NEST_LV_ASSERT_HANDLER_INSTALLED 1

#endif /* __ASSEMBLER__ */
