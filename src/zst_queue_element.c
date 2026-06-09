/*=============================================================================
    zst_queue_element.c — First-class queue element wrapper for zst_queue_t
=============================================================================*/

#include "zst_queue.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <pthread.h>

typedef struct {
    zst_queue_config_t cfg;
    zst_queue_t*       queue;
    pthread_t         thread;
    volatile int      running;
    zst_pad_t*         sinkpad;
    zst_pad_t*         srcpad;
} queue_el_priv_t;

static void*
queue_el_worker(void* arg)
{
    zst_element_t* el = arg;
    queue_el_priv_t* priv = el->priv;

    while (priv->running) {
        zst_buffer_t* buf = NULL;
        /* Pop with a small timeout (50ms) to allow checking priv->running */
        zst_result_t ret = zst_queue_pop(priv->queue, &buf, 50);
        if (ret == ZST_OK && buf) {
            /* Clock slaving: wait until PTS or drop if late */
            if (el->clock && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
                zst_time_t current = zst_clock_get_time(el->clock);
                /* If it's too early, wait until PTS */
                if (buf->pts > current + 5000000ULL) { /* 5ms early threshold */
                    zst_clock_wait(el->clock, buf->pts - current);
                } else if (buf->pts < current - 100000000ULL) { /* 100ms late threshold */
                    /* Drop late buffer */
                    zst_buffer_unref(buf);
                    continue;
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
    priv->running = 1;
    if (pthread_create(&priv->thread, NULL, queue_el_worker, el) != 0) {
        priv->running = 0;
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
queue_el_stop(zst_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    priv->running = 0;
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
