/*=============================================================================
    zst_timestamp_pacer.h — Internal timestamp-based pacing helper
=============================================================================*/
#pragma once

#include <stdint.h>
#include <pthread.h>

#include "zst_types.h"
#include "zst_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TIMESTAMP_PACER_DEFAULT_TOLERANCE_NS        (5ULL * 1000ULL * 1000ULL)
#define ZST_TIMESTAMP_PACER_DEFAULT_RESET_THRESHOLD_NS  (2ULL * 1000ULL * 1000ULL * 1000ULL)
#define ZST_TIMESTAMP_PACER_DEFAULT_SLEEP_QUANTUM_NS    (10ULL * 1000ULL * 1000ULL)

typedef struct zst_timestamp_pacer {
    int enabled;
    int started;

    zst_time_t base_ts;
    zst_time_t base_clock;
    zst_time_t last_ts;

    zst_time_t tolerance_ns;
    zst_time_t reset_threshold_ns;
    zst_time_t max_lateness_ns;     /* 0 = disabled */
    zst_time_t sleep_quantum_ns;

    uint64_t paced_count;
    uint64_t dropped_count;
    uint64_t reset_count;
    uint64_t total_wait_ns;

    zst_clock_t* fallback_clock;
    pthread_mutex_t lock;
} zst_timestamp_pacer_t;

void zst_timestamp_pacer_init(zst_timestamp_pacer_t* p);
void zst_timestamp_pacer_reset(zst_timestamp_pacer_t* p);
void zst_timestamp_pacer_deinit(zst_timestamp_pacer_t* p);

void zst_timestamp_pacer_set_enabled(zst_timestamp_pacer_t* p, int enabled);
void zst_timestamp_pacer_configure(zst_timestamp_pacer_t* p,
                                   zst_time_t tolerance_ns,
                                   zst_time_t reset_threshold_ns,
                                   zst_time_t max_lateness_ns);

/*
 * Wait until timestamp_ns should be emitted according to the pacer's baseline.
 *
 * Returns:
 *   ZST_OK      buffer should be sent
 *   ZST_AGAIN   buffer is too late and should be dropped by caller policy
 *   ZST_ERROR   invalid pacer argument
 */
zst_result_t zst_timestamp_pacer_wait(zst_timestamp_pacer_t* p,
                                      zst_clock_t* clock,
                                      zst_time_t timestamp_ns,
                                      int* dropped_out);

#ifdef __cplusplus
}
#endif
