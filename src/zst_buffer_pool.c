/*=============================================================================
    zst_buffer_pool.c
=============================================================================*/

#include "zst_buffer_pool.h"
#include "zst_allocator.h"
#include "zst_log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

struct zst_buffer_pool {
    zst_allocator_t* allocator;
    zst_buffer_pool_config_t config;

    zst_buffer_t** buffers;
    uint32_t count;
    uint32_t total_allocated;

    pthread_mutex_t lock;
    pthread_cond_t  cond;

    int active;
};

zst_buffer_pool_t*
zst_buffer_pool_create(zst_allocator_t* allocator, zst_buffer_pool_config_t* config)
{
    if (!config) return NULL;

    zst_buffer_pool_t* pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->allocator = allocator ? zst_allocator_ref(allocator) : zst_allocator_cpu_create();
    pool->config = *config;

    if (pool->config.max_buffers == 0) {
        pool->config.max_buffers = 32; // Default limit
    }

    if (pool->config.min_buffers > pool->config.max_buffers) {
        pool->config.min_buffers = pool->config.max_buffers;
    }

    pool->buffers = calloc(pool->config.max_buffers, sizeof(zst_buffer_t*));
    if (!pool->buffers) {
        zst_allocator_unref(pool->allocator);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->active = 1;
    pool->count = 0;
    pool->total_allocated = 0;

    // Pre-allocate up to min_buffers
    for (uint32_t i = 0; i < pool->config.min_buffers; i++) {
        zst_buffer_t* buf = zst_buffer_create_with_allocator(
            pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
        if (buf) {
            buf->pool = pool;
            pool->buffers[pool->count++] = buf;
            pool->total_allocated++;
        }
    }

    return pool;
}

zst_result_t
zst_buffer_pool_acquire(zst_buffer_pool_t* pool, zst_buffer_t** out_buf, int timeout_ms)
{
    if (!pool || !out_buf) return ZST_ERROR;

    pthread_mutex_lock(&pool->lock);

    while (pool->active && pool->count == 0) {
        if (pool->total_allocated < pool->config.max_buffers) {
            // Allocate a new buffer
            zst_buffer_t* buf = zst_buffer_create_with_allocator(
                pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
            if (buf) {
                buf->pool = pool;
                pool->total_allocated++;
                *out_buf = buf;
                pthread_mutex_unlock(&pool->lock);
                return ZST_OK;
            }
        }

        if (timeout_ms < 0) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        } else if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->lock);
            return ZST_TIMEOUT;
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            int err = pthread_cond_timedwait(&pool->cond, &pool->lock, &ts);
            if (err != 0) {
                pthread_mutex_unlock(&pool->lock);
                return ZST_TIMEOUT;
            }
        }
    }

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return ZST_ERROR;
    }

    // Pop a buffer
    zst_buffer_t* buf = pool->buffers[--pool->count];
    buf->refcount = 1; // It's going to be used
    *out_buf = buf;

    int trigger_low = 0;
    if (pool->config.watermark_cb && pool->count == pool->config.low_watermark) {
        trigger_low = 1;
    }

    pthread_mutex_unlock(&pool->lock);

    if (trigger_low) {
        pool->config.watermark_cb(pool, ZST_POOL_WATERMARK_LOW, pool->config.watermark_user_data);
    }

    return ZST_OK;
}

void
zst_buffer_pool_release(zst_buffer_pool_t* pool, zst_buffer_t* buf)
{
    if (!pool || !buf) return;

    // Reset buffer metadata
    buf->pts = 0;
    buf->dts = 0;
    buf->duration = 0;
    buf->flags = 0;

    pthread_mutex_lock(&pool->lock);

    if (pool->active && pool->count < pool->config.max_buffers) {
        pool->buffers[pool->count++] = buf;
        pthread_cond_signal(&pool->cond);

        int trigger_high = 0;
        if (pool->config.watermark_cb && pool->count == pool->config.high_watermark) {
            trigger_high = 1;
        }

        pthread_mutex_unlock(&pool->lock);

        if (trigger_high) {
            pool->config.watermark_cb(pool, ZST_POOL_WATERMARK_HIGH, pool->config.watermark_user_data);
        }
    } else {
        pthread_mutex_unlock(&pool->lock);

        // Pool is inactive or full, destroy the buffer properly
        buf->pool = NULL;

        // Use normal unref to trigger memory release
        // (but refcount is already 0, so we just do the inner part of unref)
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);

        pthread_mutex_lock(&pool->lock);
        pool->total_allocated--;

        int should_free = (!pool->active && pool->total_allocated == 0);
        pthread_mutex_unlock(&pool->lock);

        if (should_free) {
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->cond);
            free(pool->buffers);
            zst_allocator_unref(pool->allocator);
            free(pool);
        }
    }
}

void
zst_buffer_pool_destroy(zst_buffer_pool_t* pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->active = 0;
    pthread_cond_broadcast(&pool->cond);

    for (uint32_t i = 0; i < pool->count; i++) {
        zst_buffer_t* buf = pool->buffers[i];
        buf->pool = NULL;
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);
        pool->total_allocated--;
    }
    pool->count = 0;
    int should_free = (pool->total_allocated == 0);
    pthread_mutex_unlock(&pool->lock);

    if (should_free) {
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        free(pool->buffers);
        zst_allocator_unref(pool->allocator);
        free(pool);
    }
}

zst_buffer_pool_config_t zst_buffer_pool_get_config(zst_buffer_pool_t* pool) {
    if (!pool) {
        zst_buffer_pool_config_t empty = {0};
        return empty;
    }
    return pool->config;
}

void zst_buffer_pool_prefill(zst_buffer_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    while (pool->total_allocated < pool->config.max_buffers) {
        zst_buffer_t* buf = zst_buffer_create_with_allocator(
            pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
        if (buf) {
            buf->pool = pool;
            pool->buffers[pool->count++] = buf;
            pool->total_allocated++;
        } else {
            break;
        }
    }

    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

void zst_buffer_pool_drain(zst_buffer_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    uint32_t target_allocated = pool->config.min_buffers;

    while (pool->count > 0 && pool->total_allocated > target_allocated) {
        zst_buffer_t* buf = pool->buffers[--pool->count];
        buf->pool = NULL;
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);
        pool->total_allocated--;
    }

    pthread_mutex_unlock(&pool->lock);
}
