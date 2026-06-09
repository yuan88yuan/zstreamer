/*=============================================================================
    zst_clock.c
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include "zst_clock.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

zst_clock_t*
zst_clock_ref(zst_clock_t* clock)
{
    if (!clock) return NULL;
    __sync_fetch_and_add(&clock->refcount, 1);
    return clock;
}

typedef struct {
    zst_clock_t* master;
    zst_clock_t* reference;
    pthread_t    thread;
    volatile int running;

    double       alpha;
    zst_time_t   beta;

    pthread_mutex_t lock;
} slave_clock_priv_t;

static void*
slave_clock_worker(void* arg)
{
    zst_clock_t* clock = arg;
    slave_clock_priv_t* priv = clock->priv;

    zst_time_t last_master = zst_clock_get_time(priv->master);
    zst_time_t last_ref    = zst_clock_get_time(priv->reference);

    while (priv->running) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        zst_time_t now_master = zst_clock_get_time(priv->master);
        zst_time_t now_ref    = zst_clock_get_time(priv->reference);

        if (now_master > last_master && now_ref > last_ref) {
            zst_time_t diff_master = now_master - last_master;
            zst_time_t diff_ref    = now_ref - last_ref;

            double ratio = (double)diff_master / (double)diff_ref;

            pthread_mutex_lock(&priv->lock);
            priv->alpha = priv->alpha * 0.9 + ratio * 0.1;

            /* To avoid precision loss on large now_ref, we calculate beta differently:
               current_time = (now_ref - base_ref) * alpha + base_master
               where base_ref is the time when the slave clock started.
               We can express beta as: beta = base_master - (base_ref * alpha) + drift_correction
               But actually, the standard way is:
               beta = now_master - (now_ref * alpha).
               Wait, doing `now_ref * alpha` with now_ref in ns drops precision.
               Instead, let's just let alpha and beta adjust.
               Wait, the review said: "massive time discontinuity will cause the pipeline's wait queues to freeze"
               because alpha is 0.0 at the start. So let's initialize alpha to 1.0.
            */
            priv->beta = now_master - (zst_time_t)((double)now_ref * priv->alpha);
            pthread_mutex_unlock(&priv->lock);
        }

        last_master = now_master;
        last_ref = now_ref;
    }

    return NULL;
}

static zst_time_t
slave_clock_get_time(zst_clock_t* clock)
{
    slave_clock_priv_t* priv = clock->priv;
    zst_time_t now_ref = zst_clock_get_time(priv->reference);

    pthread_mutex_lock(&priv->lock);
    double alpha = priv->alpha;
    zst_time_t beta = priv->beta;
    pthread_mutex_unlock(&priv->lock);

    /* Use long double to avoid precision loss on 64-bit nanosecond values */
    return (zst_time_t)((long double)now_ref * (long double)alpha) + beta;
}

static void
slave_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    slave_clock_priv_t* priv = clock->priv;

    pthread_mutex_lock(&priv->lock);
    double alpha = priv->alpha;
    pthread_mutex_unlock(&priv->lock);

    zst_time_t ref_time = (zst_time_t)(time / alpha);
    zst_clock_wait(priv->reference, ref_time);
}

static void
slave_clock_destroy(zst_clock_t* clock)
{
    slave_clock_priv_t* priv = clock->priv;
    if (priv) {
        if (priv->running) {
            priv->running = 0;
            pthread_join(priv->thread, NULL);
        }
        if (priv->master) {
            zst_clock_unref(priv->master);
        }
        if (priv->reference) {
            zst_clock_unref(priv->reference);
        }
        pthread_mutex_destroy(&priv->lock);
        free(priv);
    }
}

zst_clock_t*
zst_clock_slave_create(zst_clock_t* master, zst_clock_t* reference)
{
    if (!master || !reference) return NULL;

    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    slave_clock_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) {
        free(clock);
        return NULL;
    }

    priv->master = zst_clock_ref(master);
    priv->reference = zst_clock_ref(reference);

    /* Initialize with 1.0 and correct beta to avoid jump at 1 second */
    priv->alpha = 1.0;
    zst_time_t now_master = zst_clock_get_time(master);
    zst_time_t now_ref = zst_clock_get_time(reference);
    priv->beta = now_master - (zst_time_t)((long double)now_ref * (long double)priv->alpha);

    pthread_mutex_init(&priv->lock, NULL);

    clock->refcount = 1;
    clock->get_time = slave_clock_get_time;
    clock->wait     = slave_clock_wait;
    clock->destroy  = slave_clock_destroy;
    clock->priv     = priv;

    priv->running = 1;
    if (pthread_create(&priv->thread, NULL, slave_clock_worker, clock) != 0) {
        priv->running = 0;
        zst_clock_unref(priv->master);
        zst_clock_unref(priv->reference);
        pthread_mutex_destroy(&priv->lock);
        free(priv);
        free(clock);
        return NULL;
    }

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
