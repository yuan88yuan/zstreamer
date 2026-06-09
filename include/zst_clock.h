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

zst_clock_t* zst_clock_ref(zst_clock_t* clock);

void zst_clock_unref(zst_clock_t* clock);

zst_time_t zst_clock_get_time(zst_clock_t* clock);

void zst_clock_wait(zst_clock_t* clock, zst_time_t time);

#ifdef __cplusplus
}
#endif
