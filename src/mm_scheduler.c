/*=============================================================================
    mm_scheduler.c — Drives pipeline execution (single / multi-threaded)
=============================================================================*/

#define _POSIX_C_SOURCE 199309L  /* nanosleep */

#include "mm_scheduler.h"
#include "mm_queue.h"
#include "mm_pad.h"
#include "mm_element.h"
#include "mm_buffer.h"
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

/* ── Worker thread (handles both single and multi-threaded mode) ─────────── */
static void*
worker_loop(void* arg)
{
    worker_ctx_t* ctx = arg;
    mm_scheduler_t* sched = ctx->sched;
    sched_priv_t* p   = sched->priv;
    uint32_t worker_id = ctx->worker_id;
    uint32_t nb_threads = p->nb_threads;

    while (p->running) {
        int activity = 0;
        mm_pipeline_t* pipe = sched->pipeline;
        if (pipe) {
            for (uint32_t i = worker_id; i < pipe->nb_elements; i += nb_threads) {
                mm_element_t* el = pipe->elements[i];
                if (el->state != MM_STATE_PLAYING) continue;

                if (el->nb_sink_pads == 0) {
                    // Source element: process (produce)
                    mm_buffer_t* out_buf = NULL;
                    mm_result_t ret = el->ops->process(el, NULL, &out_buf);
                    if (ret == MM_OK && out_buf) {
                        activity = 1;
                        if (el->nb_src_pads > 0) {
                            mm_pad_push(el->src_pads[0], out_buf);
                            mm_buffer_unref(out_buf);
                        }
                    } else if (ret == MM_EOF) {
                        // Propagate EOS
                        mm_buffer_t* eos_buf = mm_buffer_create(MM_BUFFER_USER);
                        if (eos_buf) {
                            eos_buf->flags |= MM_BUFFER_FLAG_EOS;
                            if (el->nb_src_pads > 0) {
                                mm_pad_push(el->src_pads[0], eos_buf);
                            }
                            mm_buffer_unref(eos_buf);
                        }
                        el->state = MM_STATE_READY;
                    }
                }
            }
        }

        if (!activity) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
            nanosleep(&ts, NULL);
        }
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

    if (sched->pipeline) {
        mm_pipeline_topological_sort(sched->pipeline);
    }

    p->running = 1;

    uint32_t n = 1;
    if (sched->config.mode == MM_SCHEDULER_MULTI_THREAD) {
        n = sched->config.worker_threads < 1 ? 1 : sched->config.worker_threads;
    }

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

    if (p->threads) {
        for (uint32_t i = 0; i < p->nb_threads; i++) {
            pthread_join(p->threads[i], NULL);
        }
        free(p->threads);
        p->threads    = NULL;
        p->nb_threads = 0;
    }

    return MM_OK;
}
