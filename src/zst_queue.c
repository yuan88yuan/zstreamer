/*=============================================================================
    zst_queue.c — Thread-safe bounded buffer queue (mutex + condvar)
=============================================================================*/

#define _POSIX_C_SOURCE 199309L  /* clock_gettime, CLOCK_REALTIME */

#include "zst_queue.h"
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

typedef struct zst_queue_node {
    struct zst_queue_node* next;
    zst_buffer_t*          buf;
} zst_queue_node_t;

struct zst_queue {
    zst_queue_config_t cfg;

    zst_queue_node_t* head;   /* dequeue from head */
    zst_queue_node_t* tail;   /* enqueue at tail  */
    uint32_t         count;
    uint64_t         bytes;

    pthread_mutex_t  lock;
    pthread_cond_t   not_full;
    pthread_cond_t   not_empty;
};

zst_queue_t*
zst_queue_create(const zst_queue_config_t* cfg)
{
    zst_queue_t* q = calloc(1, sizeof(*q));
    if (!q) return NULL;

    if (cfg) {
        q->cfg = *cfg;
    } else {
        q->cfg.mode        = ZST_QUEUE_SYNC;
        q->cfg.max_buffers = 10;
        q->cfg.max_bytes   = 0;
        q->cfg.max_duration= 0;
    }

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);

    return q;
}

void
zst_queue_destroy(zst_queue_t* q)
{
    if (!q) return;

    /* Free any remaining buffers */
    zst_queue_flush(q);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}

static void
timespec_from_ms(struct timespec* ts, uint32_t timeout_ms)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    ts->tv_sec  = now.tv_sec  + timeout_ms / 1000;
    ts->tv_nsec = now.tv_nsec + (timeout_ms % 1000) * 1000000L;

    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static zst_time_t
zst_queue_get_duration_locked(zst_queue_t* q)
{
    if (!q->head || !q->tail) return 0;
    if (q->tail->buf->pts >= q->head->buf->pts && q->head->buf->pts != 0) {
        return q->tail->buf->pts - q->head->buf->pts;
    }
    /* Fallback: sum of durations */
    zst_time_t sum = 0;
    zst_queue_node_t* n = q->head;
    while (n) {
        sum += n->buf->duration;
        n = n->next;
    }
    return sum;
}

static int
zst_queue_is_full_locked(zst_queue_t* q)
{
    if (q->cfg.max_buffers > 0 && q->count >= q->cfg.max_buffers) {
        return 1;
    }
    if (q->cfg.max_bytes > 0 && q->bytes >= q->cfg.max_bytes) {
        return 1;
    }
    if (q->cfg.max_duration > 0 && zst_queue_get_duration_locked(q) >= q->cfg.max_duration) {
        return 1;
    }
    return 0;
}

zst_result_t
zst_queue_push(zst_queue_t* q, zst_buffer_t* buf, uint32_t timeout_ms)
{
    if (!q || !buf) return ZST_ERROR;

    pthread_mutex_lock(&q->lock);

    /* If async mode and full, drop the buffer */
    if (q->cfg.mode == ZST_QUEUE_ASYNC && zst_queue_is_full_locked(q)) {
        pthread_mutex_unlock(&q->lock);
        return ZST_ERROR;
    }

    /* Wait until there is room */
    if (q->cfg.mode == ZST_QUEUE_SYNC) {
        struct timespec ts;
        int use_timeout = (timeout_ms != UINT32_MAX);

        while (zst_queue_is_full_locked(q)) {
            if (use_timeout) {
                if (timeout_ms == 0) {
                    pthread_mutex_unlock(&q->lock);
                    return ZST_TIMEOUT;
                }
                timespec_from_ms(&ts, timeout_ms);
                int ret = pthread_cond_timedwait(&q->not_full, &q->lock, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&q->lock);
                    return ZST_TIMEOUT;
                }
            } else {
                pthread_cond_wait(&q->not_full, &q->lock);
            }
        }
    }

    /* Enqueue */
    zst_queue_node_t* node = malloc(sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&q->lock);
        return ZST_ERROR;
    }
    node->buf  = zst_buffer_ref(buf);
    node->next = NULL;

    if (q->tail)
        q->tail->next = node;
    else
        q->head = node;
    q->tail = node;
    q->count++;
    q->bytes += buf->memory.size;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return ZST_OK;
}

zst_result_t
zst_queue_pop(zst_queue_t* q, zst_buffer_t** out, uint32_t timeout_ms)
{
    if (!q || !out) return ZST_ERROR;

    pthread_mutex_lock(&q->lock);

    int use_timeout = (timeout_ms != UINT32_MAX);

    while (!q->head) {
        if (use_timeout) {
            if (timeout_ms == 0) {
                pthread_mutex_unlock(&q->lock);
                return ZST_TIMEOUT;
            }
            struct timespec ts;
            timespec_from_ms(&ts, timeout_ms);
            int ret = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&q->lock);
                return ZST_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
    }

    /* Dequeue */
    zst_queue_node_t* node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    if (node->buf) {
        q->bytes -= node->buf->memory.size;
    }

    *out = node->buf;
    free(node);

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return ZST_OK;
}

uint32_t
zst_queue_size(zst_queue_t* q)
{
    if (!q) return 0;

    pthread_mutex_lock(&q->lock);
    uint32_t size = q->count;
    pthread_mutex_unlock(&q->lock);
    return size;
}

void
zst_queue_flush(zst_queue_t* q)
{
    if (!q) return;

    pthread_mutex_lock(&q->lock);

    zst_queue_node_t* node = q->head;
    while (node) {
        zst_queue_node_t* next = node->next;
        zst_buffer_unref(node->buf);
        free(node);
        node = next;
    }
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
    q->bytes = 0;

    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
