#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    UI_EVT_BRIGHTNESS,
    UI_EVT_FAN,
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

void ui_register_change_cb(ui_change_cb_t cb);
