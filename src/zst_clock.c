/*=============================================================================
    zst_clock.c
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include "zst_clock.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>

zst_clock_t*
zst_clock_ref(zst_clock_t* clock)
{
    if (!clock) return NULL;
    __sync_fetch_and_add(&clock->refcount, 1);
    return clock;
}

void
zst_clock_unref(zst_clock_t* clock)
{
    if (!clock) return;
    if (__sync_sub_and_fetch(&clock->refcount, 1) > 0)
        return;

    if (clock->destroy)
        clock->destroy(clock);

    free(clock);
}

zst_time_t
zst_clock_get_time(zst_clock_t* clock)
{
    if (!clock || !clock->get_time) return 0;
    return clock->get_time(clock);
}

void
zst_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    if (!clock || !clock->wait) return;
    clock->wait(clock, time);
}

static zst_time_t
system_clock_get_time(zst_clock_t* clock)
{
    (void)clock;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (zst_time_t)ts.tv_sec * 1000000000ULL + (zst_time_t)ts.tv_nsec;
}

static void
system_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    (void)clock;
    struct timespec ts;
    ts.tv_sec = time / 1000000000ULL;
    ts.tv_nsec = time % 1000000000ULL;
    /* Since we wait for a duration, not an absolute time, use relative wait */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
}

static void
system_clock_destroy(zst_clock_t* clock)
{
    (void)clock;
}

zst_clock_t*
zst_clock_system_create(void)
{
    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    clock->refcount = 1;
    clock->get_time = system_clock_get_time;
    clock->wait     = system_clock_wait;
    clock->destroy  = system_clock_destroy;
    clock->priv     = NULL;

    return clock;
}
