/*=============================================================================
    zst_clock.h
=============================================================================*/
#pragma once

#include "zst_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zst_clock {
    volatile int refcount;

    zst_time_t (*get_time)(zst_clock_t* clock);
    void       (*wait)(zst_clock_t* clock, zst_time_t time);
    void       (*destroy)(zst_clock_t* clock);

    void* priv;
};

zst_clock_t* zst_clock_system_create(void);

zst_clock_t* zst_clock_slave_create(zst_clock_t* master, zst_clock_t* reference);

zst_clock_t* zst_clock_ref(zst_clock_t* clock);

void zst_clock_unref(zst_clock_t* clock);

zst_time_t zst_clock_get_time(zst_clock_t* clock);

void zst_clock_wait(zst_clock_t* clock, zst_time_t time);

zst_result_t zst_clock_get_sync_stats(zst_clock_t* clock, double* jitter_sec_out, double* max_error_sec_out);

#ifdef __cplusplus
}
#endif
