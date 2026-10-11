#include "status_handoff.h"

void status_handoff_publish(status_handoff_t *h, status_snapshot_t status)
{
    const uint32_t seq = atomic_load_explicit(&h->seq, memory_order_relaxed);
    atomic_store_explicit(&h->seq, seq + 1, memory_order_relaxed); /* odd: in progress */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&h->wifi, status.wifi_connected, memory_order_relaxed);
    atomic_store_explicit(&h->ip_v4, status.ip_v4, memory_order_relaxed);
    atomic_store_explicit(&h->mqtt, status.mqtt_connected, memory_order_relaxed);
    atomic_store_explicit(&h->seq, seq + 2, memory_order_release); /* even: complete */
}

bool status_handoff_take(status_handoff_t *h, status_snapshot_t *out)
{
    const uint32_t seq = atomic_load_explicit(&h->seq, memory_order_acquire);
    if (seq == h->taken_seq || (seq & 1) != 0)
        return false;
    const status_snapshot_t status = {
        .wifi_connected = atomic_load_explicit(&h->wifi, memory_order_relaxed),
        .ip_v4 = atomic_load_explicit(&h->ip_v4, memory_order_relaxed),
        .mqtt_connected = atomic_load_explicit(&h->mqtt, memory_order_relaxed),
    };
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&h->seq, memory_order_relaxed) != seq)
        return false;
    *out = status;
    h->taken_seq = seq;
    return true;
}
