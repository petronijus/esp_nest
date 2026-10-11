#pragma once

/*
 * Hand-off 4: the connection state from loopTask (writer) to nest_ui
 * (reader), as a seqlock.
 *
 * One writer, one reader. `seq` is 0 until the first publish, odd while a
 * publish is in progress and even (and non-zero) once a state is complete;
 * it moves on every publish, so the reader takes each published state once
 * and never a torn mix of two. The fields are atomics only to make the
 * racing access well-defined; their consistency comes from `seq`.
 *
 * Pure C11, no ESP-IDF / LVGL / ESPHome dependencies, host-tested in
 * test/host/ (including a two-thread stress run).
 */

#include <stdbool.h>
#include <stdint.h>

/* The same layout seen from C11 (the module) and C++ (nest_ui_task). */
#ifdef __cplusplus
#include <atomic>
#define STATUS_ATOMIC(T) std::atomic<T>
#else
#include <stdatomic.h>
#define STATUS_ATOMIC(T) _Atomic(T)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool wifi_connected;
    uint32_t ip_v4; /* host byte order */
    bool mqtt_connected;
} status_snapshot_t;

typedef struct {
    STATUS_ATOMIC(uint32_t) seq;
    STATUS_ATOMIC(bool) wifi;
    STATUS_ATOMIC(uint32_t) ip_v4;
    STATUS_ATOMIC(bool) mqtt;
    uint32_t taken_seq; /* the seq of the state the reader has taken (reader only) */
} status_handoff_t;

/* Static initializer: nothing published, nothing taken. */
#define STATUS_HANDOFF_INIT { 0, false, 0, false, 0 }

/* Writer side (a single thread). */
void status_handoff_publish(status_handoff_t *h, status_snapshot_t status);

/* Reader side (a single thread). Returns true and fills `out` when a state
 * that has not been taken yet is complete. A publish in progress, or one
 * that lands while reading, returns false: the state is left for the next
 * call, which sees it complete. */
bool status_handoff_take(status_handoff_t *h, status_snapshot_t *out);

#ifdef __cplusplus
}
#endif
