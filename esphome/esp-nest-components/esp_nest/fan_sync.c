#include "fan_sync.h"

void fan_sync_init(fan_sync_t *fs)
{
    fan_sync_on_connect(fs);
}

void fan_sync_on_connect(fan_sync_t *fs)
{
    fs->last_pub = -1;
    fs->last_pub_ms = 0;
}

bool fan_sync_on_local(fan_sync_t *fs, int64_t now_ms, int level)
{
    if (level < 0 || level > FAN_SYNC_LEVEL_MAX) return false;
    if (level == fs->last_pub) return false;

    fs->last_pub = level;
    fs->last_pub_ms = now_ms;
    return true;
}

int fan_sync_on_message(fan_sync_t *fs, int64_t now_ms,
                        int parsed_level, int current_level)
{
    int new_level = parsed_level >= 0 ? parsed_level : current_level;

    /* Echo suppression: the same value we published moments ago.
     * last_pub_ms == 0 means nothing published since (re)connect. */
    if (fs->last_pub_ms > 0
        && now_ms - fs->last_pub_ms < FAN_SYNC_ECHO_MS
        && new_level == fs->last_pub)
        return -1;

    if (new_level == current_level)
        return -1;

    fs->last_pub = new_level;
    return new_level;
}
