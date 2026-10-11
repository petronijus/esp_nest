#include "blinds_sync.h"

void blinds_sync_init(blinds_sync_t *bs)
{
    for (int t = 0; t < BLINDS_SYNC_GROUPS; t++) {
        for (int m = 0; m < BLINDS_SYNC_MEMBERS; m++)
            bs->member_pos[t][m] = -1;
        bs->pending_step[t] = -1;
        bs->change_ms[t] = 0;
        bs->block_until_ms[t] = 0;
    }
}

int blinds_sync_step_to_pos(int step)
{
    return 100 - (step * 100 + BLINDS_SYNC_MAX_STEP / 2) / BLINDS_SYNC_MAX_STEP;
}

int blinds_sync_pos_to_step(int pos)
{
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    return ((100 - pos) * BLINDS_SYNC_MAX_STEP + 50) / 100;
}

static bool valid_target(int target)
{
    return target >= 0 && target < BLINDS_SYNC_GROUPS;
}

bool blinds_sync_on_local(blinds_sync_t *bs, int64_t now_ms,
                          int target, int step)
{
    if (!valid_target(target)) return false;
    if (step < 0 || step > BLINDS_SYNC_MAX_STEP) return false;

    /* Debounced: keep only the newest step; the tick flushes it once the
     * user stops stepping. */
    bs->pending_step[target] = step;
    bs->change_ms[target] = now_ms;
    bs->block_until_ms[target] = now_ms + BLINDS_SYNC_BLOCK_MS;
    return true;
}

int blinds_sync_tick(blinds_sync_t *bs, int64_t now_ms, bool connected,
                     blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS])
{
    if (!connected) return 0;

    int n = 0;
    for (int t = 0; t < BLINDS_SYNC_GROUPS; t++) {
        if (bs->pending_step[t] < 0) continue;
        if (now_ms - bs->change_ms[t] < BLINDS_SYNC_SETTLE_MS) continue;

        int step = bs->pending_step[t];
        blinds_sync_cmd_t *cmd = &out[n++];
        cmd->target = t;
        if (step == 0) {
            cmd->kind = BLINDS_SYNC_CMD_OPEN;
            cmd->position = 100;
        } else if (step == BLINDS_SYNC_MAX_STEP) {
            cmd->kind = BLINDS_SYNC_CMD_CLOSE;
            cmd->position = 0;
        } else {
            cmd->kind = BLINDS_SYNC_CMD_POSITION;
            cmd->position = blinds_sync_step_to_pos(step);
        }

        bs->pending_step[t] = -1;
        bs->block_until_ms[t] = now_ms + BLINDS_SYNC_BLOCK_MS;
    }
    return n;
}

int blinds_sync_on_member(blinds_sync_t *bs, int64_t now_ms,
                          int target, int member, int position,
                          int *out_avg)
{
    if (!valid_target(target)) return -1;
    if (member < 0 || member >= BLINDS_SYNC_MEMBERS) return -1;

    /* Stored even inside the block window, so the average is current as
     * soon as the window ends and the next report arrives. */
    bs->member_pos[target][member] = position;

    if (now_ms < bs->block_until_ms[target] || bs->pending_step[target] >= 0)
        return -1;

    /* Average over the members heard from; negative positions count as
     * unknown. 64-bit sum: three reports near INT_MAX must not overflow. */
    int64_t sum = 0;
    int cnt = 0;
    for (int m = 0; m < BLINDS_SYNC_MEMBERS; m++) {
        if (bs->member_pos[target][m] >= 0) {
            sum += bs->member_pos[target][m];
            cnt++;
        }
    }
    if (!cnt) return -1;

    int avg = (int)(sum / cnt);
    if (out_avg) *out_avg = avg;
    return blinds_sync_pos_to_step(avg);
}
