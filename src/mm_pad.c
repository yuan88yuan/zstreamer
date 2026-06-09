/*=============================================================================
    mm_pad.c — Pad creation, linking, and unlinking
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "mm_pad.h"
#include <stdlib.h>
#include <string.h>

mm_pad_t*
mm_pad_create(const char* name, mm_pad_direction_t direction)
{
    mm_pad_t* pad = calloc(1, sizeof(*pad));
    if (!pad) return NULL;

    pad->name      = name ? strdup(name) : NULL;
    pad->direction = direction;
    pad->parent    = NULL;
    pad->caps      = NULL;
    pad->push      = NULL;
    pad->pull      = NULL;
    pad->peer      = NULL;
    pad->priv      = NULL;

    return pad;
}

void
mm_pad_destroy(mm_pad_t* pad)
{
    if (!pad) return;

    /* Unlink from peer if still connected */
    if (pad->peer)
        mm_pad_unlink(pad);

    free((void*)pad->name);
    free(pad->caps);
    free(pad);
}

mm_result_t
mm_pad_link(mm_pad_t* src, mm_pad_t* sink)
{
    if (!src || !sink)
        return MM_ERROR;

    if (src->direction != MM_PAD_SRC)
        return MM_ERROR;

    if (sink->direction != MM_PAD_SINK)
        return MM_ERROR;

    /* Refuse if either pad is already linked */
    if (src->peer || sink->peer)
        return MM_ERROR;

    src->peer = sink;
    sink->peer = src;

    return MM_OK;
}

void
mm_pad_unlink(mm_pad_t* pad)
{
    if (!pad) return;

    mm_pad_t* peer = pad->peer;
    if (!peer) return;

    /* Break the link from both sides */
    pad->peer = NULL;
    peer->peer = NULL;
}
