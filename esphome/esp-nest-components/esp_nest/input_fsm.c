#include "input_fsm.h"

void input_fsm_init(input_fsm_t *fsm)
{
    fsm->accum = 0;
    fsm->cooldown_until_ms = 0;
    fsm->was_pressed = false;
    fsm->press_start_ms = 0;
    fsm->long_press_fired = false;
}

void input_fsm_update(input_fsm_t *fsm, int64_t now_ms,
                      int count_diff, bool pressed, bool detail,
                      input_fsm_out_t *out)
{
    out->navigate = 0;
    out->long_press = false;
    out->press = false;

    /* --- Rotation --- */
    if (now_ms < fsm->cooldown_until_ms) {
        fsm->accum = 0;
    } else if (count_diff != 0) {
        if (detail) {
            out->navigate = count_diff;
        } else {
            fsm->accum += count_diff;
            if (fsm->accum >= INPUT_FSM_MENU_STEP_COUNTS) {
                out->navigate = 1;
                fsm->accum = 0;
                fsm->cooldown_until_ms = now_ms + INPUT_FSM_MENU_COOLDOWN_MS;
            } else if (fsm->accum <= -INPUT_FSM_MENU_STEP_COUNTS) {
                out->navigate = -1;
                fsm->accum = 0;
                fsm->cooldown_until_ms = now_ms + INPUT_FSM_MENU_COOLDOWN_MS;
            }
        }
    }

    /* --- Button: press / long press --- */
    if (pressed && !fsm->was_pressed) {
        fsm->press_start_ms = now_ms;
        fsm->long_press_fired = false;
    }

    if (pressed && !fsm->long_press_fired
        && now_ms - fsm->press_start_ms > INPUT_FSM_LONG_PRESS_MS) {
        out->long_press = true;
        fsm->long_press_fired = true;
    }

    if (!pressed && fsm->was_pressed) {
        if (!fsm->long_press_fired
            && now_ms - fsm->press_start_ms > INPUT_FSM_PRESS_MIN_MS)
            out->press = true;
        fsm->accum = 0;
        fsm->cooldown_until_ms = now_ms + INPUT_FSM_RELEASE_COOLDOWN_MS;
    }

    out->activity = count_diff != 0 || pressed;
    fsm->was_pressed = pressed;
}
