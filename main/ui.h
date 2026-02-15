#ifndef UI_H
#define UI_H

#include "lvgl.h"

void ui_init(void);
void update_status_label(void);
void update_light_button_ui(void);

// For network updates
void ui_set_fan_state(const char *state);
void ui_set_light_level(int level);

#endif // UI_H
