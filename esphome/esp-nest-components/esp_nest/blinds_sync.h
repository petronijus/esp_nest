#pragma once

/*
 * Blinds synchronization: UI steps <-> the ESPHome RF cover bridge.
 *
 * The bridge exposes two groups of three time_based member covers
 * (position 0 = closed .. 100 = open, dead reckoning) plus one
 * broadcast-only group cover per group. The UI shows 7 steps per group
 * (0 = open .. 6 = closed). Behaviour of the native v2.0.1 firmware
 * (main/ha_mqtt.c):
 *
 *   - Local steps are debounced: the newest step is queued and only sent
 *     once the user has stopped stepping for BLINDS_SYNC_SETTLE_MS (the
 *     RF link dislikes command bursts). Queuing does not depend on the
 *     MQTT connection; the flush happens only while connected, so a
 *     queued step survives a disconnect.
 *   - Endpoint steps go out as one OPEN / CLOSE group broadcast (one RF
 *     burst moves all three motors and the end-stops re-home the dead
 *     reckoning); intermediate steps as the same position to all three
 *     members.
 *   - Queuing and flushing both (re)open a BLINDS_SYNC_BLOCK_MS window in
 *     which incoming member positions are stored but not shown, so the
 *     covers' travel reports do not drag the UI around. No resync follows.
 *   - The UI position is the truncating average of the members heard from
 *     so far (retained messages usually deliver all three on connect).
 *
 * Pure C, no ESP-IDF / ESPHome dependencies: time is passed in by the
 * caller as int64 milliseconds (never a wrapping uint32) and the module is
 * host-tested in test/host/.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLINDS_SYNC_GROUPS     2     /* 0 = group A, 1 = group B (ui.c order) */
#define BLINDS_SYNC_MEMBERS    3     /* member covers per group */
#define BLINDS_SYNC_MAX_STEP   6     /* UI steps 0 (open) .. 6 (closed) */
#define BLINDS_SYNC_SETTLE_MS  500   /* wait for the user to stop stepping */
#define BLINDS_SYNC_BLOCK_MS   2500  /* ignore member reports after a command */

typedef enum {
    BLINDS_SYNC_CMD_NONE = 0,
    BLINDS_SYNC_CMD_OPEN,      /* "OPEN" to the group command topic */
    BLINDS_SYNC_CMD_CLOSE,     /* "CLOSE" to the group command topic */
    BLINDS_SYNC_CMD_POSITION,  /* `position` to all member command topics */
} blinds_sync_cmd_kind_t;

typedef struct {
    int                    target;    /* group index */
    blinds_sync_cmd_kind_t kind;
    int                    position;  /* 0-100, BLINDS_SYNC_CMD_POSITION only */
} blinds_sync_cmd_t;

typedef struct {
    int     member_pos[BLINDS_SYNC_GROUPS][BLINDS_SYNC_MEMBERS]; /* -1 unknown */
    int     pending_step[BLINDS_SYNC_GROUPS];   /* queued UI step, -1 none */
    int64_t change_ms[BLINDS_SYNC_GROUPS];      /* time of the last local step */
    int64_t block_until_ms[BLINDS_SYNC_GROUPS]; /* member reports ignored until */
} blinds_sync_t;

void blinds_sync_init(blinds_sync_t *bs);

/* Local UI step for `target`. Queues it (replacing any queued step) and
 * opens the block window. Returns false, changing nothing, for a target
 * or step outside the valid range. */
bool blinds_sync_on_local(blinds_sync_t *bs, int64_t now_ms,
                          int target, int step);

/* Main-loop tick. While `connected`, every group whose queued step has
 * settled is flushed into out[] (at most BLINDS_SYNC_GROUPS commands, in
 * group order) and its block window is reopened. Returns the number of
 * commands written; 0 and no state change while disconnected. */
int blinds_sync_tick(blinds_sync_t *bs, int64_t now_ms, bool connected,
                     blinds_sync_cmd_t out[BLINDS_SYNC_GROUPS]);

/* Member position report (already parsed from the payload). The position
 * is always stored. Returns the UI step to show, or -1 while the group is
 * blocked or has a queued step, when no member position is known, or for
 * an invalid target / member. When a step is returned and `out_avg` is
 * non-NULL, it receives the averaged position the step came from. */
int blinds_sync_on_member(blinds_sync_t *bs, int64_t now_ms,
                          int target, int member, int position,
                          int *out_avg);

/* 0-6 UI step <-> 0-100 cover position (100 = open = step 0) */
int blinds_sync_step_to_pos(int step);
int blinds_sync_pos_to_step(int pos);

#ifdef __cplusplus
}
#endif
