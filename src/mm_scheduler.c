/*=============================================================================
    mm_scheduler.c — Drives pipeline execution (single / multi-threaded)
=============================================================================*/

#define _POSIX_C_SOURCE 199309L  /* nanosleep */

#include "mm_scheduler.h"
#include "mm_queue.h"
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/* ── Private scheduler state (stored in ->priv) ──────────────────────── */
typedef struct {
    volatile int running;
    pthread_t*    threads;
    uint32_t      nb_threads;
} sched_priv_t;

typedef struct worker_context {
    mm_scheduler_t* sched;
    int             worker_id;
} worker_ctx_t;

mm_scheduler_t*
mm_scheduler_create(const mm_scheduler_config_t* cfg)
{
    mm_scheduler_t* sched = calloc(1, sizeof(*sched));
    if (!sched) return NULL;

    if (cfg)
        sched->config = *cfg;
    else {
        sched->config.mode           = MM_SCHEDULER_SINGLE_THREAD;
        sched->config.worker_threads = 1;
    }

    sched->pipeline = NULL;

    sched_priv_t* p = calloc(1, sizeof(*p));
    if (!p) { free(sched); return NULL; }
    p->running    = 0;
    p->threads    = NULL;
    p->nb_threads = 0;
    sched->priv   = p;

    return sched;
}

void
mm_scheduler_destroy(mm_scheduler_t* sched)
{
    if (!sched) return;

    sched_priv_t* p = sched->priv;
    if (p) {
        if (p->running)
            mm_scheduler_stop(sched);
        free(p->threads);
        free(p);
    }
    free(sched);
}

mm_result_t
mm_scheduler_attach(mm_scheduler_t* sched, mm_pipeline_t* pipe)
{
    if (!sched || !pipe) return MM_ERROR;
    sched->pipeline = pipe;
    return MM_OK;
}

/* ── Worker thread (multi-threaded mode) ──────────────────────────────── */
static void*
worker_loop(void* arg)
{
    worker_ctx_t* ctx = arg;
    sched_priv_t* p   = ctx->sched->priv;

    while (p->running) {
        /* yield to avoid busy-loop */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    free(ctx);
    return NULL;
}

mm_result_t
mm_scheduler_run(mm_scheduler_t* sched)
{
    if (!sched) return MM_ERROR;

    sched_priv_t* p = sched->priv;
    if (!p) return MM_ERROR;
    if (p->running) return MM_OK;

    p->running = 1;

    if (sched->config.mode == MM_SCHEDULER_SINGLE_THREAD) {
        /* Single-threaded: process everything inline.
           A full implementation would walk the element graph and call
           process() on each element in topological order. */
        return MM_OK;
    }

    /* Multi-threaded: spawn worker pool */
    uint32_t n = sched->config.worker_threads < 1
                     ? 1
                     : sched->config.worker_threads;

    p->threads    = calloc(n, sizeof(pthread_t));
    p->nb_threads = n;

    for (uint32_t i = 0; i < n; i++) {
        worker_ctx_t* ctx = malloc(sizeof(*ctx));
        if (!ctx) continue;
        ctx->sched    = sched;
        ctx->worker_id = (int)i;

        pthread_create(&p->threads[i], NULL, worker_loop, ctx);
    }

    return MM_OK;
}

mm_result_t
mm_scheduler_stop(mm_scheduler_t* sched)
{
    if (!sched) return MM_ERROR;

    sched_priv_t* p = sched->priv;
    if (!p) return MM_ERROR;
    if (!p->running) return MM_OK;

    p->running = 0;

    /* Join worker threads */
    if (sched->config.mode == MM_SCHEDULER_MULTI_THREAD && p->threads) {
        for (uint32_t i = 0; i < p->nb_threads; i++) {
            pthread_join(p->threads[i], NULL);
        }
        free(p->threads);
        p->threads    = NULL;
        p->nb_threads = 0;
    }

    return MM_OK;
}
