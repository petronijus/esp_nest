#include "light_sync.h"

void light_sync_init(light_sync_t *ls)
{
    /* Start with the throttle window already elapsed so the very first
     * local change publishes immediately (now_ms may legitimately be 0). */
    ls->last_pub_ms = -(int64_t)LIGHT_SYNC_THROTTLE_MS;
    ls->block_until_ms = 0;
    ls->synced_brightness = -1;
    ls->pending_brightness = -1;
    ls->resync_pending = false;
}

int light_sync_pct_to_raw(int pct)
{
    if (pct <= 0) return 0;
    if (pct > 100) pct = 100;
    int raw = (pct * 255 + 50) / 100;
    return raw > 255 ? 255 : raw;
}

int light_sync_raw_to_pct(int raw)
{
    if (raw <= 0) return 0;
    if (raw > 255) raw = 255;
    return (raw * 100 + 127) / 255;
}

static void mark_published(light_sync_t *ls, int64_t now_ms, int brightness)
{
    ls->last_pub_ms = now_ms;
    ls->block_until_ms = now_ms + LIGHT_SYNC_BLOCK_MS;
    ls->synced_brightness = brightness;
    ls->pending_brightness = -1;
    ls->resync_pending = true;
}

light_sync_action_t light_sync_on_local_change(light_sync_t *ls,
                                               int64_t now_ms,
                                               int brightness)
{
    if (brightness == ls->synced_brightness) {
        /* User dialed back to the already-published value: nothing to
         * send, and any queued intermediate value is now obsolete. */
        ls->pending_brightness = -1;
        return LIGHT_SYNC_ACT_NONE;
    }

    if (now_ms - ls->last_pub_ms >= LIGHT_SYNC_THROTTLE_MS) {
        mark_published(ls, now_ms, brightness);
        return LIGHT_SYNC_ACT_PUBLISH;
    }

    ls->pending_brightness = brightness;
    /* Keep the authority window open while changes queue up, so a slow
     * flush can't let stale echoes slip in between. */
    ls->block_until_ms = now_ms + LIGHT_SYNC_BLOCK_MS;
    return LIGHT_SYNC_ACT_NONE;
}

light_sync_action_t light_sync_tick(light_sync_t *ls,
                                    int64_t now_ms,
                                    int *out_brightness)
{
    if (ls->pending_brightness >= 0) {
        if (now_ms - ls->last_pub_ms >= LIGHT_SYNC_THROTTLE_MS) {
            *out_brightness = ls->pending_brightness;
            mark_published(ls, now_ms, *out_brightness);
            return LIGHT_SYNC_ACT_PUBLISH;
        }
        return LIGHT_SYNC_ACT_NONE;
    }

    if (ls->resync_pending && now_ms >= ls->block_until_ms) {
        ls->resync_pending = false;
        return LIGHT_SYNC_ACT_RESYNC;
    }

    return LIGHT_SYNC_ACT_NONE;
}

int light_sync_on_state_msg(light_sync_t *ls, int64_t now_ms,
                            bool have_state, bool state_on,
                            bool have_br, int raw,
                            int current_brightness)
{
    /* Authority window: drop everything. The /get resync issued after
     * the window closes repairs any divergence this may cause.
     * (pending_brightness >= 0 implies the window is open, but keep the
     * explicit check in case the tunables ever make BLOCK < THROTTLE.) */
    if (now_ms < ls->block_until_ms || ls->pending_brightness >= 0)
        return -1;

    int new_level = current_brightness;

    if (have_br)
        new_level = light_sync_raw_to_pct(raw);

    if (have_state) {
        if (!state_on)
            new_level = 0;          /* OFF wins over the stored brightness */
        else if (!have_br && new_level == 0)
            new_level = 100;        /* bare "ON" while we show off */
    }

    /* Z2M is the source of truth past this point: remember what it
     * reported so a no-op local change doesn't republish it. */
    ls->synced_brightness = new_level;

    if (new_level == current_brightness)
        return -1;

    return new_level;
}
