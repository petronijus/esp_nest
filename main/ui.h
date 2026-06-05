#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    UI_EVT_BRIGHTNESS,
    UI_EVT_FAN,
    UI_EVT_VOLUME,      /* value: +1 = up, -1 = down (stateless, relative) */
    UI_EVT_BLINDS,      /* value: (target << 8) | step; target 0=Klavir 1=DJ */
} ui_event_type_t;

typedef void (*ui_change_cb_t)(ui_event_type_t type, int value);

void ui_init(void);
void ui_set_status(const char *text);
void ui_encoder_navigate(int diff);
void ui_encoder_press(void);
void ui_encoder_long_press(void);

int  ui_get_current_page(void);
bool ui_is_detail_mode(void);
int  ui_get_brightness(void);
int  ui_get_fan_level(void);

void ui_set_brightness(int brightness);
void ui_set_fan_level(int level);
void ui_set_blinds_pos(int target, int step);   /* target 0=Klavir 1=DJ, step 0-6 */

void ui_register_change_cb(ui_change_cb_t cb);
void ui_process_pending(void);
