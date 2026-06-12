/*=============================================================================
    zst_scheduler.c — Drives pipeline execution (single / multi-threaded)
=============================================================================*/

#define _POSIX_C_SOURCE 199309L  /* nanosleep */

#include "zst_scheduler.h"
#include "zst_queue.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
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
    zst_scheduler_t* sched;
    int             worker_id;
} worker_ctx_t;

zst_scheduler_t*
zst_scheduler_create(const zst_scheduler_config_t* cfg)
{
    zst_scheduler_t* sched = calloc(1, sizeof(*sched));
    if (!sched) return NULL;

    if (cfg)
        sched->config = *cfg;
    else {
        sched->config.mode           = ZST_SCHEDULER_SINGLE_THREAD;
        sched->config.worker_threads = 1;
    }

    sched->pipeline = NULL;

    sched_priv_t* p = calloc(1, sizeof(*p));
    if (!p) { free(sched); return NULL; }
    __atomic_store_n(&p->running, 0, __ATOMIC_RELEASE);
    p->threads    = NULL;
    p->nb_threads = 0;
    sched->priv   = p;

    return sched;
}

void
zst_scheduler_destroy(zst_scheduler_t* sched)
{
    if (!sched) return;

    sched_priv_t* p = sched->priv;
    if (p) {
        if (__atomic_load_n(&p->running, __ATOMIC_ACQUIRE))
            zst_scheduler_stop(sched);
        free(p->threads);
        free(p);
    }
    free(sched);
}

zst_result_t
zst_scheduler_attach(zst_scheduler_t* sched, zst_pipeline_t* pipe)
{
    if (!sched || !pipe) return ZST_ERROR;
    sched->pipeline = pipe;
    return ZST_OK;
}

/* ── Worker thread (handles both single and multi-threaded mode) ─────────── */
static void*
worker_loop(void* arg)
{
    worker_ctx_t* ctx = arg;
    zst_scheduler_t* sched = ctx->sched;
    sched_priv_t* p   = sched->priv;
    uint32_t worker_id = ctx->worker_id;
    uint32_t nb_threads = p->nb_threads;

    while (__atomic_load_n(&p->running, __ATOMIC_ACQUIRE)) {
        int activity = 0;
        zst_pipeline_t* pipe = sched->pipeline;
        if (pipe) {
            for (uint32_t i = worker_id; i < pipe->nb_elements; i += nb_threads) {
                zst_element_t* el = pipe->elements[i];
                if (__atomic_load_n(&el->state, __ATOMIC_ACQUIRE) != ZST_STATE_PLAYING) continue;

                if (el->nb_sink_pads == 0) {
                    // Source element: process (produce)
                    zst_buffer_t* out_buf = NULL;
                    zst_result_t ret = el->ops->process(el, NULL, &out_buf);
                    if (ret == ZST_OK && out_buf) {
                        activity = 1;
                        if (el->nb_src_pads > 0) {
                            /* Clock-sync mode: wait until PTS is reached before delivering */
                            if (pipe->clock_sync && el->clock && out_buf->pts > 0
                                && !(out_buf->flags & (ZST_BUFFER_FLAG_EOS | ZST_BUFFER_FLAG_DROP))) {
                                zst_time_t current = zst_clock_get_time(el->clock);
                                if (out_buf->pts > current + 5000000ULL) {
                                    zst_clock_wait(el->clock, out_buf->pts - current);
                                }
                            }
                            zst_pad_push(el->src_pads[0], out_buf);
                            zst_buffer_unref(out_buf);
                            /* Downstream elements can lazily create pools while
                             * processing the pushed buffer. Re-apply topology
                             * sizing so those late pools are fixed before the
                             * next buffer burst. */
                            zst_pipeline_update_buffer_pool_sizing(pipe);
                        }
                    } else if (ret == ZST_EOF) {
                        // Propagate EOS
                        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
                        if (eos_buf) {
                            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                            if (el->nb_src_pads > 0) {
                                zst_pad_push(el->src_pads[0], eos_buf);
                            } else if (el->bus) {
                                zst_bus_post(el->bus, zst_event_new_eos(el));
                            }
                            zst_buffer_unref(eos_buf);
                        }
                        __atomic_store_n(&el->state, ZST_STATE_READY, __ATOMIC_RELEASE);
                    } else if (ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
                        if (el->bus) {
                            zst_event_t* err_ev = zst_event_new_error(el, ret, "Source process failed");
                            zst_bus_post(el->bus, err_ev);
                        }
                    }
                }
            }
        }

        if (!activity) {
            if (sched->pipeline && sched->pipeline->clock) {
                zst_clock_wait(sched->pipeline->clock, 1000000ULL);
            } else {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
                nanosleep(&ts, NULL);
            }
        }
    }

    free(ctx);
    return NULL;
}

zst_result_t
zst_scheduler_run(zst_scheduler_t* sched)
{
    if (!sched) return ZST_ERROR;

    sched_priv_t* p = sched->priv;
    if (!p) return ZST_ERROR;
    if (__atomic_load_n(&p->running, __ATOMIC_ACQUIRE)) return ZST_OK;

    if (sched->pipeline) {
        zst_pipeline_topological_sort(sched->pipeline);
        zst_pipeline_update_buffer_pool_sizing(sched->pipeline);
    }

    __atomic_store_n(&p->running, 1, __ATOMIC_RELEASE);

    uint32_t n = 1;
    if (sched->config.mode == ZST_SCHEDULER_MULTI_THREAD) {
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

    return ZST_OK;
}

zst_result_t
zst_scheduler_stop(zst_scheduler_t* sched)
{
    if (!sched) return ZST_ERROR;

    sched_priv_t* p = sched->priv;
    if (!p) return ZST_ERROR;
    if (!__atomic_load_n(&p->running, __ATOMIC_ACQUIRE)) return ZST_OK;

    __atomic_store_n(&p->running, 0, __ATOMIC_RELEASE);

    if (p->threads) {
        for (uint32_t i = 0; i < p->nb_threads; i++) {
            pthread_join(p->threads[i], NULL);
        }
        free(p->threads);
        p->threads    = NULL;
        p->nb_threads = 0;
    }

    return ZST_OK;
}
