/*=============================================================================
    zst_queue_element.c — First-class queue element wrapper for zst_queue_t
=============================================================================*/

#include "zst_queue.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_bus.h"
#include <string.h>
#include "zst_buffer_pool.h"
#include <stdlib.h>
#include <pthread.h>

typedef struct {
    zst_queue_config_t cfg;
    zst_queue_t*       queue;
    pthread_t         thread;
    volatile int      running;
    zst_pad_t*         sinkpad;
    zst_pad_t*         srcpad;
    zst_buffer_pool_t* pool;
} queue_el_priv_t;

static void*
queue_el_worker(void* arg)
{
    zst_element_t* el = arg;
    queue_el_priv_t* priv = el->priv;

    while (__atomic_load_n(&priv->running, __ATOMIC_ACQUIRE)) {
        zst_buffer_t* buf = NULL;
        /* Pop with a small timeout (50ms) to allow checking priv->running */
        zst_result_t ret = zst_queue_pop(priv->queue, &buf, 50);
        if (ret == ZST_OK && buf) {
            /* Clock slaving: wait until PTS or drop if late */
            if (el->clock && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
                zst_time_t current = zst_clock_get_time(el->clock);
                /* If it's too early, wait until PTS */
                if (buf->pts > current + 5000000ULL) { /* 5ms early threshold */
                    if (buf->pts - current < 5000000000ULL) { /* 5s safeguard */
                        zst_clock_wait(el->clock, buf->pts - current);
                    }
                } else if (buf->pts < current - 100000000ULL) { /* 100ms late threshold */
                    if (current - buf->pts < 5000000000ULL) { /* 5s safeguard */
                        /* Drop late buffer (QoS) */
                        buf->flags |= ZST_BUFFER_FLAG_DROP;
                        if (el->bus) {
                            zst_event_t* qos_ev = zst_event_new_warning(el, ZST_ERROR, "QoS: Queue element dropped late frame");
                            zst_bus_post(el->bus, qos_ev);
                        }
                        zst_buffer_unref(buf);
                        continue;
                    }
                }
            }

            zst_pad_push(priv->srcpad, buf);
            zst_buffer_unref(buf);
        }
    }
    return NULL;
}


static zst_result_t
queue_el_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    queue_el_priv_t* priv = el->priv;

    if (!priv->queue) return ZST_ERROR;

    /* Skip/discard immediately if flagged as drop (QoS) */
    if (buf && (buf->flags & ZST_BUFFER_FLAG_DROP)) {
        return ZST_OK;
    }

    /* Optionally attach pool: if a pool is configured and it's not an EOS buffer,
       we acquire a buffer from the pool, copy the incoming buffer's data,
       and push the pooled buffer instead. */
    if (priv->pool && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_buffer_t* pool_buf = NULL;
        /* Block until a buffer is available */
        if (zst_buffer_pool_acquire(priv->pool, &pool_buf, -1, 0) == ZST_OK) {
            /* Copy metadata */
            pool_buf->pts = buf->pts;
            pool_buf->dts = buf->dts;
            pool_buf->duration = buf->duration;
            pool_buf->flags = buf->flags;
            pool_buf->type = buf->type;
            pool_buf->payload = buf->payload;
            pool_buf->metadata = buf->metadata;

            /* Copy payload memory if fits */
            if (buf->memory.size > 0) {
                if (pool_buf->memory.size < buf->memory.size) {
                    /* Buffer too small, error out */
                    zst_buffer_unref(pool_buf);
                    return ZST_ERROR;
                }
                memcpy(pool_buf->memory.data, buf->memory.data, buf->memory.size);
            }

            zst_result_t ret = zst_queue_push(priv->queue, pool_buf, UINT32_MAX);

            /* push adds a reference, so we unref our local reference to return it to the queue's ownership */
            zst_buffer_unref(pool_buf);
            return ret;
        }
    }

    /* Push the buffer into the queue (blocking or dropping based on config) */
    return zst_queue_push(priv->queue, buf, UINT32_MAX);
}

static zst_result_t
queue_el_open(zst_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    priv->queue = zst_queue_create(&priv->cfg);
    if (!priv->queue) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t
queue_el_close(zst_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    if (priv->queue) {
        zst_queue_destroy(priv->queue);
        priv->queue = NULL;
    }
    return ZST_OK;
}

static zst_result_t
queue_el_start(zst_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    __atomic_store_n(&priv->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&priv->thread, NULL, queue_el_worker, el) != 0) {
        __atomic_store_n(&priv->running, 0, __ATOMIC_RELEASE);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
queue_el_stop(zst_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    __atomic_store_n(&priv->running, 0, __ATOMIC_RELEASE);
    /* Flush the queue to unblock the worker thread */
    if (priv->queue) {
        zst_queue_flush(priv->queue);
    }
    pthread_join(priv->thread, NULL);
    return ZST_OK;
}

static zst_element_ops_t queue_el_ops = {
    .name    = "queue",
    .open    = queue_el_open,
    .close   = queue_el_close,
    .start   = queue_el_start,
    .stop    = queue_el_stop,
    .process = NULL
};

zst_element_t*
zst_queue_element_create(const zst_queue_config_t* cfg)
{
    queue_el_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (cfg) {
        priv->cfg = *cfg;
    } else {
        priv->cfg.mode         = ZST_QUEUE_SYNC;
        priv->cfg.max_buffers  = 10;
        priv->cfg.max_bytes    = 0;
        priv->cfg.max_duration = 0;
    }

    zst_element_t* el = zst_element_create(&queue_el_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    if (!priv->sinkpad) {
        zst_element_destroy(el);
        return NULL;
    }
    zst_element_add_pad(el, priv->sinkpad);

    priv->srcpad  = zst_pad_create("src", ZST_PAD_SRC);
    if (!priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    zst_element_add_pad(el, priv->srcpad);

    /* Override sinkpad's push callback so that it routes to the queue */
    priv->sinkpad->push = queue_el_sink_push;

    return el;
}

zst_result_t
zst_queue_element_set_pool(zst_element_t* el, zst_buffer_pool_t* pool)
{
    if (!el || !el->ops) return ZST_ERROR;
    queue_el_priv_t* priv = el->priv;
    priv->pool = pool;
    return ZST_OK;
}
