/*=============================================================================
    zst_buffer.c - Thread-safe, lock-free refcounted buffers with alignment
=============================================================================*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_allocator.h"
#include "zst_log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    /* Keep fd as the first member so ZST_MEMORY_DMABUF users can treat
     * memory.priv as int* when passing buffers to V4L2_MEMORY_DMABUF APIs. */
    int fd;
    zst_allocator_t* allocator;
    void* ptr;
} zst_buffer_alloc_ctx_t;

static void
zst_buffer_alloc_release(void* priv)
{
    zst_buffer_alloc_ctx_t* ctx = (zst_buffer_alloc_ctx_t*)priv;
    if (!ctx) return;

    if (ctx->allocator && ctx->ptr) {
        zst_allocator_free(ctx->allocator, ctx->ptr);
        zst_allocator_unref(ctx->allocator);
    }
    free(ctx);
}

#if defined(_MSC_VER)
#define zst_aligned_alloc(al, sz) _aligned_malloc(sz, al)
#define zst_aligned_free(ptr) _aligned_free(ptr)
#else
#define zst_aligned_alloc(al, sz) aligned_alloc(al, ((sz + al - 1) / al) * al)
#define zst_aligned_free(ptr) free(ptr)
#endif

zst_buffer_t* zst_buffer_create(uint32_t type) {
    zst_buffer_t* buf = (zst_buffer_t*)zst_aligned_alloc(ZST_CACHE_LINE_SIZE, sizeof(zst_buffer_t));
    if (!buf) return NULL;

    memset(buf, 0, sizeof(zst_buffer_t));
    atomic_init(&buf->refcount, 1);
    buf->type = type;
    
    return buf;
}

zst_buffer_t* zst_buffer_create_with_allocator(
    uint32_t type,
    zst_allocator_t* allocator,
    size_t size) 
{
    zst_buffer_t* buf = zst_buffer_create(type);
    if (!buf) return NULL;

    if (allocator && size > 0) {
        void* data = zst_allocator_alloc(allocator, size);
        if (!data) {
            zst_aligned_free(buf);
            return NULL;
        }

        zst_buffer_alloc_ctx_t* ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            zst_allocator_free(allocator, data);
            zst_aligned_free(buf);
            return NULL;
        }

        ctx->allocator = zst_allocator_ref(allocator);
        ctx->ptr = data;
        ctx->fd = zst_allocator_dmabuf_get_fd(allocator, data);

        buf->memory.type = (ctx->fd >= 0) ? ZST_MEMORY_DMABUF : ZST_MEMORY_CPU;
        buf->memory.data = data;
        buf->memory.size = size;
        buf->memory.priv = ctx;
        buf->memory.release = zst_buffer_alloc_release;
    }
    return buf;
}

zst_buffer_t* zst_buffer_create_with_pool(struct zst_buffer_pool* pool) {
    if (!pool) return NULL;
    zst_buffer_t* buf = NULL;
    if (zst_buffer_pool_acquire(pool, &buf, 0, 0) == ZST_OK) {
        return buf;
    }
    return NULL;
}

void zst_buffer_unref(zst_buffer_t* buf) {
    if (!buf) return;

    /* Lock-free fetch and decrement with release-acquire semantics for thread visibility */
    if (atomic_fetch_sub_explicit(&buf->refcount, 1, memory_order_acq_rel) == 1) {
        if (buf->pool) {
            /* Recycle instead of freeing memory */
            zst_buffer_pool_release(buf->pool, buf);
            return;
        }

        if (buf->memory.release && buf->memory.data) {
            buf->memory.release(buf->memory.priv);
        }

        if (buf->destroy) {
            buf->destroy(buf);
        }

        zst_aligned_free(buf);
    }
}
