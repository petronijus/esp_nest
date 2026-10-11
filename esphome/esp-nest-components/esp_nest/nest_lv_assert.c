#include "nest_lv_assert.h"

#include <stddef.h>

#include "esp_rom_sys.h"
#include "esp_system.h"

/*
 * Runs on whichever task hit the LVGL assert (normally nest_ui), possibly
 * after a failed allocation: no heap, no logger, no locks, no newlib stdio.
 * The location goes straight to the console through the ROM printf
 * (synchronous); esp_system_abort() then panics with a fixed reason, and the
 * panic handler adds the backtrace that ESPHome's crash handler keeps for
 * the next boot.
 */
void nest_lv_assert_failed(const char *file, int line)
{
    esp_rom_printf("LVGL assertion failed at %s:%d\n", file != NULL ? file : "?", line);
    esp_system_abort("LVGL assertion failed");
}
