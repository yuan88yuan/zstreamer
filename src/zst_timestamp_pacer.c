/*=============================================================================
    zst_timestamp_pacer.c — Internal timestamp-based pacing helper
=============================================================================*/

#include "zst_timestamp_pacer.h"

#include <stdlib.h>

static zst_time_t
zst_timestamp_pacer_min_time(zst_time_t a, zst_time_t b)
{
    return a < b ? a : b;
}

static zst_time_t
zst_timestamp_pacer_get_now(zst_timestamp_pacer_t* p, zst_clock_t* clock)
{
    zst_clock_t* selected = clock ? clock : p->fallback_clock;
    if (!selected) return 0;
    return zst_clock_get_time(selected);
}

void
zst_timestamp_pacer_init(zst_timestamp_pacer_t* p)
{
    if (!p) return;

    p->enabled = 0;
    p->started = 0;
    p->base_ts = 0;
    p->base_clock = 0;
    p->last_ts = 0;
    p->tolerance_ns = ZST_TIMESTAMP_PACER_DEFAULT_TOLERANCE_NS;
    p->reset_threshold_ns = ZST_TIMESTAMP_PACER_DEFAULT_RESET_THRESHOLD_NS;
    p->max_lateness_ns = 0;
    p->sleep_quantum_ns = ZST_TIMESTAMP_PACER_DEFAULT_SLEEP_QUANTUM_NS;
    p->paced_count = 0;
    p->dropped_count = 0;
    p->reset_count = 0;
    p->total_wait_ns = 0;
    p->fallback_clock = zst_clock_system_create();
    pthread_mutex_init(&p->lock, NULL);
}

void
zst_timestamp_pacer_reset(zst_timestamp_pacer_t* p)
{
    if (!p) return;

    pthread_mutex_lock(&p->lock);
    p->started = 0;
    p->base_ts = 0;
    p->base_clock = 0;
    p->last_ts = 0;
    p->reset_count++;
    pthread_mutex_unlock(&p->lock);
}

void
zst_timestamp_pacer_deinit(zst_timestamp_pacer_t* p)
{
    if (!p) return;

    pthread_mutex_lock(&p->lock);
    zst_clock_t* fallback = p->fallback_clock;
    p->fallback_clock = NULL;
    p->started = 0;
    pthread_mutex_unlock(&p->lock);

    if (fallback) {
        zst_clock_unref(fallback);
    }

    pthread_mutex_destroy(&p->lock);
}

void
zst_timestamp_pacer_set_enabled(zst_timestamp_pacer_t* p, int enabled)
{
    if (!p) return;

    pthread_mutex_lock(&p->lock);
    p->enabled = enabled ? 1 : 0;
    if (!p->enabled) {
        p->started = 0;
    }
    pthread_mutex_unlock(&p->lock);
}

void
zst_timestamp_pacer_configure(zst_timestamp_pacer_t* p,
                              zst_time_t tolerance_ns,
                              zst_time_t reset_threshold_ns,
                              zst_time_t max_lateness_ns)
{
    if (!p) return;

    pthread_mutex_lock(&p->lock);
    p->tolerance_ns = tolerance_ns;
    p->reset_threshold_ns = reset_threshold_ns;
    p->max_lateness_ns = max_lateness_ns;
    pthread_mutex_unlock(&p->lock);
}

static int
zst_timestamp_pacer_should_reset_locked(zst_timestamp_pacer_t* p,
                                        zst_time_t timestamp_ns)
{
    if (!p->started) return 1;

    /* Backward timestamps indicate seek/flush/discontinuity. */
    if (timestamp_ns < p->last_ts) return 1;

    /* Large forward jumps should not cause multi-second sleeps. */
    if (p->reset_threshold_ns > 0 &&
        timestamp_ns > p->last_ts &&
        (timestamp_ns - p->last_ts) > p->reset_threshold_ns) {
        return 1;
    }

    return 0;
}

static void
zst_timestamp_pacer_start_locked(zst_timestamp_pacer_t* p,
                                 zst_time_t timestamp_ns,
                                 zst_time_t now)
{
    p->started = 1;
    p->base_ts = timestamp_ns;
    p->base_clock = now;
    p->last_ts = timestamp_ns;
}

zst_result_t
zst_timestamp_pacer_wait(zst_timestamp_pacer_t* p,
                         zst_clock_t* clock,
                         zst_time_t timestamp_ns,
                         int* dropped_out)
{
    if (dropped_out) *dropped_out = 0;
    if (!p) return ZST_ERROR;

    for (;;) {
        zst_time_t now = zst_timestamp_pacer_get_now(p, clock);

        pthread_mutex_lock(&p->lock);

        if (!p->enabled) {
            pthread_mutex_unlock(&p->lock);
            return ZST_OK;
        }

        if (!clock && !p->fallback_clock) {
            pthread_mutex_unlock(&p->lock);
            return ZST_OK;
        }

        if (zst_timestamp_pacer_should_reset_locked(p, timestamp_ns)) {
            if (p->started) {
                p->reset_count++;
            }
            zst_timestamp_pacer_start_locked(p, timestamp_ns, now);
            p->paced_count++;
            pthread_mutex_unlock(&p->lock);
            return ZST_OK;
        }

        if (timestamp_ns == p->last_ts) {
            p->paced_count++;
            pthread_mutex_unlock(&p->lock);
            return ZST_OK;
        }

        if (timestamp_ns < p->base_ts) {
            zst_timestamp_pacer_start_locked(p, timestamp_ns, now);
            p->reset_count++;
            p->paced_count++;
            pthread_mutex_unlock(&p->lock);
            return ZST_OK;
        }

        zst_time_t ts_delta = timestamp_ns - p->base_ts;
        zst_time_t target;
        if (UINT64_MAX - p->base_clock < ts_delta) {
            target = UINT64_MAX;
        } else {
            target = p->base_clock + ts_delta;
        }

        if (now > target && p->max_lateness_ns > 0 &&
            (now - target) > p->max_lateness_ns) {
            p->last_ts = timestamp_ns;
            p->dropped_count++;
            if (dropped_out) *dropped_out = 1;
            pthread_mutex_unlock(&p->lock);
            return ZST_AGAIN;
        }

        if (target > now) {
            zst_time_t wait_ns = target - now;
            if (wait_ns > p->tolerance_ns) {
                zst_time_t quantum = p->sleep_quantum_ns > 0
                    ? p->sleep_quantum_ns
                    : ZST_TIMESTAMP_PACER_DEFAULT_SLEEP_QUANTUM_NS;
                zst_time_t sleep_ns = zst_timestamp_pacer_min_time(wait_ns, quantum);
                zst_clock_t* selected_clock = clock ? clock : p->fallback_clock;

                p->total_wait_ns += sleep_ns;
                pthread_mutex_unlock(&p->lock);

                if (selected_clock) {
                    zst_clock_wait(selected_clock, sleep_ns);
                }
                continue;
            }
        }

        p->last_ts = timestamp_ns;
        p->paced_count++;
        pthread_mutex_unlock(&p->lock);
        return ZST_OK;
    }
}
