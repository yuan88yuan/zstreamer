/*=============================================================================
    zst_buffer.c — Reference-counted buffer with typed memory payload
=============================================================================*/

#include "zst_buffer.h"
#include "zst_allocator.h"
#include "zst_buffer_pool.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zst_allocator_t* allocator;
    void* ptr;
} zst_buffer_alloc_ctx_t;

static void
zst_buffer_alloc_release(void* priv)
{
    zst_buffer_alloc_ctx_t* ctx = (zst_buffer_alloc_ctx_t*)priv;
    if (ctx) {
        if (ctx->allocator && ctx->ptr) {
            zst_allocator_free(ctx->allocator, ctx->ptr);
            zst_allocator_unref(ctx->allocator);
        }
        free(ctx);
    }
}

zst_buffer_t*
zst_buffer_create_with_allocator(uint32_t type, zst_allocator_t* allocator, size_t size)
{
    if (!allocator) return NULL;

    zst_buffer_t* buf = zst_buffer_create(type);
    if (!buf) return NULL;

    void* ptr = zst_allocator_alloc(allocator, size);
    if (!ptr) {
        zst_buffer_unref(buf);
        return NULL;
    }

    zst_buffer_alloc_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        zst_allocator_free(allocator, ptr);
        zst_buffer_unref(buf);
        return NULL;
    }

    ctx->allocator = zst_allocator_ref(allocator);
    ctx->ptr = ptr;

    buf->memory.data = ptr;
    buf->memory.size = size;
    buf->memory.priv = ctx;
    buf->memory.release = zst_buffer_alloc_release;

    return buf;
}

zst_buffer_t*
zst_buffer_create_with_pool(zst_buffer_pool_t* pool)
{
    if (!pool) return NULL;

    zst_buffer_t* buf = NULL;
    if (zst_buffer_pool_acquire(pool, &buf, 0) != ZST_OK) {
        return NULL;
    }

    return buf;
}

zst_buffer_t*
zst_buffer_create(uint32_t type)
{
    zst_buffer_t* buf = calloc(1, sizeof(*buf));
    if (!buf) return NULL;

    buf->type      = type;
    buf->refcount  = 1;
    buf->pts       = 0;
    buf->dts       = 0;
    buf->duration  = 0;
    buf->flags     = 0;
    buf->memory.data = NULL;
    buf->memory.size = 0;
    buf->payload   = NULL;
    buf->metadata  = NULL;
    buf->pool      = NULL;
    buf->destroy   = NULL;

    return buf;
}

zst_buffer_t*
zst_buffer_ref(zst_buffer_t* buf)
{
    if (!buf) return NULL;

    __sync_fetch_and_add(&buf->refcount, 1);
    return buf;
}

void
zst_buffer_unref(zst_buffer_t* buf)
{
    if (!buf) return;

    if (__sync_sub_and_fetch(&buf->refcount, 1) > 0)
        return;

    if (buf->pool) {
        zst_buffer_pool_release(buf->pool, buf);
        return;
    }

    /* Release attached memory */
    if (buf->memory.release && buf->memory.priv)
        buf->memory.release(buf->memory.priv);

    /* Custom destructor */
    if (buf->destroy)
        buf->destroy(buf);

    free(buf);
}
