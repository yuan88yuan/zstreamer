/*=============================================================================
    mm_buffer.c — Reference-counted buffer with typed memory payload
=============================================================================*/

#include "mm_buffer.h"
#include <stdlib.h>
#include <string.h>

mm_buffer_t*
mm_buffer_create(uint32_t type)
{
    mm_buffer_t* buf = calloc(1, sizeof(*buf));
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
    buf->destroy   = NULL;

    return buf;
}

mm_buffer_t*
mm_buffer_ref(mm_buffer_t* buf)
{
    if (!buf) return NULL;

    __sync_fetch_and_add(&buf->refcount, 1);
    return buf;
}

void
mm_buffer_unref(mm_buffer_t* buf)
{
    if (!buf) return;

    if (__sync_sub_and_fetch(&buf->refcount, 1) > 0)
        return;

    /* Release attached memory */
    if (buf->memory.release && buf->memory.priv)
        buf->memory.release(buf->memory.priv);

    /* Custom destructor */
    if (buf->destroy)
        buf->destroy(buf);

    free(buf);
}
