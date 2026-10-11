#include "backlight_policy.h"

static int clamp_pct(int pct)
{
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

backlight_policy_action_t backlight_policy_init(backlight_policy_t *bp,
                                                int64_t now_ms,
                                                int active_pct, int dim_pct,
                                                int timeout_s, int *out_pct)
{
    bp->active_pct = clamp_pct(active_pct);
    bp->dim_pct = clamp_pct(dim_pct);
    backlight_policy_set_timeout(bp, timeout_s);
    bp->dimmed = false;
    bp->last_activity_ms = now_ms;

    *out_pct = bp->active_pct;
    return BACKLIGHT_POLICY_ACT_SET;
}

backlight_policy_action_t backlight_policy_update(backlight_policy_t *bp,
                                                  int64_t now_ms,
                                                  bool activity,
                                                  int *out_pct)
{
    if (activity) {
        bp->last_activity_ms = now_ms;
        if (!bp->dimmed) return BACKLIGHT_POLICY_ACT_NONE;

        bp->dimmed = false;
        *out_pct = bp->active_pct;
        return BACKLIGHT_POLICY_ACT_SET;
    }

    if (bp->dimmed || bp->timeout_ms <= 0) return BACKLIGHT_POLICY_ACT_NONE;
    if (now_ms - bp->last_activity_ms < bp->timeout_ms)
        return BACKLIGHT_POLICY_ACT_NONE;

    bp->dimmed = true;
    *out_pct = bp->dim_pct;
    return BACKLIGHT_POLICY_ACT_FADE;
}

backlight_policy_action_t backlight_policy_set_active(backlight_policy_t *bp,
                                                      int pct, int *out_pct)
{
    bp->active_pct = clamp_pct(pct);
    if (bp->dimmed) return BACKLIGHT_POLICY_ACT_NONE;

    *out_pct = bp->active_pct;
    return BACKLIGHT_POLICY_ACT_SET;
}

backlight_policy_action_t backlight_policy_set_dim(backlight_policy_t *bp,
                                                   int pct, int *out_pct)
{
    bp->dim_pct = clamp_pct(pct);
    if (!bp->dimmed) return BACKLIGHT_POLICY_ACT_NONE;

    *out_pct = bp->dim_pct;
    return BACKLIGHT_POLICY_ACT_SET;
}

void backlight_policy_set_timeout(backlight_policy_t *bp, int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > BACKLIGHT_POLICY_TIMEOUT_MAX_S)
        seconds = BACKLIGHT_POLICY_TIMEOUT_MAX_S;
    bp->timeout_ms = seconds * 1000;
}
