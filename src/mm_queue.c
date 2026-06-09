/*=============================================================================
    mm_queue.c — Thread-safe bounded buffer queue (mutex + condvar)
=============================================================================*/

#define _POSIX_C_SOURCE 199309L  /* clock_gettime, CLOCK_REALTIME */

#include "mm_queue.h"
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

typedef struct mm_queue_node {
    struct mm_queue_node* next;
    mm_buffer_t*          buf;
} mm_queue_node_t;

struct mm_queue {
    mm_queue_config_t cfg;

    mm_queue_node_t* head;   /* dequeue from head */
    mm_queue_node_t* tail;   /* enqueue at tail  */
    uint32_t         count;
    uint64_t         bytes;

    pthread_mutex_t  lock;
    pthread_cond_t   not_full;
    pthread_cond_t   not_empty;
};

mm_queue_t*
mm_queue_create(const mm_queue_config_t* cfg)
{
    mm_queue_t* q = calloc(1, sizeof(*q));
    if (!q) return NULL;

    if (cfg) {
        q->cfg = *cfg;
    } else {
        q->cfg.mode        = MM_QUEUE_SYNC;
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
mm_queue_destroy(mm_queue_t* q)
{
    if (!q) return;

    /* Free any remaining buffers */
    mm_queue_flush(q);

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

mm_result_t
mm_queue_push(mm_queue_t* q, mm_buffer_t* buf, uint32_t timeout_ms)
{
    if (!q || !buf) return MM_ERROR;

    pthread_mutex_lock(&q->lock);

    /* Wait until there is room */
    if (q->cfg.mode == MM_QUEUE_SYNC && q->cfg.max_buffers > 0) {
        struct timespec ts;
        int use_timeout = (timeout_ms != UINT32_MAX);

        while (q->count >= q->cfg.max_buffers) {
            if (use_timeout && timeout_ms > 0) {
                timespec_from_ms(&ts, timeout_ms);
                int ret = pthread_cond_timedwait(&q->not_full, &q->lock, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&q->lock);
                    return MM_TIMEOUT;
                }
            } else {
                pthread_cond_wait(&q->not_full, &q->lock);
            }
        }
    }

    /* Enqueue */
    mm_queue_node_t* node = malloc(sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&q->lock);
        return MM_ERROR;
    }
    node->buf  = mm_buffer_ref(buf);
    node->next = NULL;

    if (q->tail)
        q->tail->next = node;
    else
        q->head = node;
    q->tail = node;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return MM_OK;
}

mm_result_t
mm_queue_pop(mm_queue_t* q, mm_buffer_t** out, uint32_t timeout_ms)
{
    if (!q || !out) return MM_ERROR;

    pthread_mutex_lock(&q->lock);

    int use_timeout = (timeout_ms != UINT32_MAX);

    while (!q->head) {
        if (use_timeout && timeout_ms > 0) {
            struct timespec ts;
            timespec_from_ms(&ts, timeout_ms);
            int ret = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&q->lock);
                return MM_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
    }

    /* Dequeue */
    mm_queue_node_t* node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;

    *out = node->buf;
    free(node);

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return MM_OK;
}

uint32_t
mm_queue_size(mm_queue_t* q)
{
    if (!q) return 0;

    pthread_mutex_lock(&q->lock);
    uint32_t size = q->count;
    pthread_mutex_unlock(&q->lock);
    return size;
}

void
mm_queue_flush(mm_queue_t* q)
{
    if (!q) return;

    pthread_mutex_lock(&q->lock);

    mm_queue_node_t* node = q->head;
    while (node) {
        mm_queue_node_t* next = node->next;
        mm_buffer_unref(node->buf);
        free(node);
        node = next;
    }
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;

    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
