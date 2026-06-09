/*=============================================================================
    mm_queue_element.c — First-class queue element wrapper for mm_queue_t
=============================================================================*/

#include "mm_queue.h"
#include "mm_element.h"
#include "mm_pad.h"
#include "mm_buffer.h"
#include <stdlib.h>
#include <pthread.h>

typedef struct {
    mm_queue_config_t cfg;
    mm_queue_t*       queue;
    pthread_t         thread;
    volatile int      running;
    mm_pad_t*         sinkpad;
    mm_pad_t*         srcpad;
} queue_el_priv_t;

static void*
queue_el_worker(void* arg)
{
    mm_element_t* el = arg;
    queue_el_priv_t* priv = el->priv;

    while (priv->running) {
        mm_buffer_t* buf = NULL;
        /* Pop with a small timeout (50ms) to allow checking priv->running */
        mm_result_t ret = mm_queue_pop(priv->queue, &buf, 50);
        if (ret == MM_OK && buf) {
            mm_pad_push(priv->srcpad, buf);
            mm_buffer_unref(buf);
        }
    }
    return NULL;
}

static mm_result_t
queue_el_sink_push(mm_pad_t* pad, mm_buffer_t* buf)
{
    mm_element_t* el = pad->parent;
    queue_el_priv_t* priv = el->priv;

    if (!priv->queue) return MM_ERROR;

    /* Push the buffer into the queue (blocking or dropping based on config) */
    return mm_queue_push(priv->queue, buf, UINT32_MAX);
}

static mm_result_t
queue_el_open(mm_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    priv->queue = mm_queue_create(&priv->cfg);
    if (!priv->queue) return MM_ERROR;
    return MM_OK;
}

static mm_result_t
queue_el_close(mm_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    if (priv->queue) {
        mm_queue_destroy(priv->queue);
        priv->queue = NULL;
    }
    return MM_OK;
}

static mm_result_t
queue_el_start(mm_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    priv->running = 1;
    if (pthread_create(&priv->thread, NULL, queue_el_worker, el) != 0) {
        priv->running = 0;
        return MM_ERROR;
    }
    return MM_OK;
}

static mm_result_t
queue_el_stop(mm_element_t* el)
{
    queue_el_priv_t* priv = el->priv;
    priv->running = 0;
    /* Flush the queue to unblock the worker thread */
    if (priv->queue) {
        mm_queue_flush(priv->queue);
    }
    pthread_join(priv->thread, NULL);
    return MM_OK;
}

static mm_element_ops_t queue_el_ops = {
    .name    = "queue",
    .open    = queue_el_open,
    .close   = queue_el_close,
    .start   = queue_el_start,
    .stop    = queue_el_stop,
    .process = NULL
};

mm_element_t*
mm_queue_element_create(const mm_queue_config_t* cfg)
{
    queue_el_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (cfg) {
        priv->cfg = *cfg;
    } else {
        priv->cfg.mode         = MM_QUEUE_SYNC;
        priv->cfg.max_buffers  = 10;
        priv->cfg.max_bytes    = 0;
        priv->cfg.max_duration = 0;
    }

    mm_element_t* el = mm_element_create(&queue_el_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = mm_pad_create("sink", MM_PAD_SINK);
    if (!priv->sinkpad) {
        mm_element_destroy(el);
        return NULL;
    }
    mm_element_add_pad(el, priv->sinkpad);

    priv->srcpad  = mm_pad_create("src", MM_PAD_SRC);
    if (!priv->srcpad) {
        mm_element_destroy(el);
        return NULL;
    }
    mm_element_add_pad(el, priv->srcpad);

    /* Override sinkpad's push callback so that it routes to the queue */
    priv->sinkpad->push = queue_el_sink_push;

    return el;
}
