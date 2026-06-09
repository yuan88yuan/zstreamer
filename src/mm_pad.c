/*=============================================================================
    mm_pad.c — Pad creation, linking, and unlinking
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "mm_pad.h"
#include "mm_element.h"
#include "mm_buffer.h"
#include <stdlib.h>
#include <string.h>

static mm_result_t
default_sink_pad_push(mm_pad_t* pad, mm_buffer_t* buf)
{
    mm_element_t* el = pad->parent;
    if (!el || !el->ops) return MM_ERROR;

    if (buf && (buf->flags & MM_BUFFER_FLAG_EOS)) {
        if (el->nb_src_pads > 0) {
            return mm_pad_push(el->src_pads[0], buf);
        }
        return MM_OK;
    }

    mm_buffer_t* out_buf = NULL;
    mm_result_t ret = MM_OK;

    if (el->ops->process) {
        ret = el->ops->process(el, buf, &out_buf);
    }

    if (ret == MM_OK && out_buf) {
        if (el->nb_src_pads > 0) {
            ret = mm_pad_push(el->src_pads[0], out_buf);
            if (out_buf != buf) {
                mm_buffer_unref(out_buf);
            }
        }
    }

    return ret;
}

static mm_result_t
default_src_pad_pull(mm_pad_t* pad, mm_buffer_t** out)
{
    mm_element_t* el = pad->parent;
    if (!el || !el->ops) return MM_ERROR;

    mm_buffer_t* out_buf = NULL;
    mm_result_t ret = MM_OK;

    if (el->nb_sink_pads > 0) {
        mm_buffer_t* in_buf = NULL;
        ret = mm_pad_pull(el->sink_pads[0], &in_buf);
        if (ret != MM_OK) return ret;

        if (in_buf && (in_buf->flags & MM_BUFFER_FLAG_EOS)) {
            *out = in_buf;
            return MM_OK;
        }

        if (el->ops->process) {
            ret = el->ops->process(el, in_buf, &out_buf);
        }
        mm_buffer_unref(in_buf);
    } else {
        if (el->ops->process) {
            ret = el->ops->process(el, NULL, &out_buf);
        }
    }

    if (ret == MM_OK) {
        *out = out_buf;
    }

    return ret;
}

mm_pad_t*
mm_pad_create(const char* name, mm_pad_direction_t direction)
{
    mm_pad_t* pad = calloc(1, sizeof(*pad));
    if (!pad) return NULL;

    pad->name      = name ? strdup(name) : NULL;
    pad->direction = direction;
    pad->parent    = NULL;
    pad->caps      = NULL;
    if (direction == MM_PAD_SRC) {
        pad->pull = default_src_pad_pull;
        pad->push = NULL;
    } else {
        pad->push = default_sink_pad_push;
        pad->pull = NULL;
    }
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

mm_result_t
mm_pad_push(mm_pad_t* pad, mm_buffer_t* buf)
{
    if (!pad || !buf) return MM_ERROR;
    if (pad->direction != MM_PAD_SRC) return MM_ERROR;
    if (!pad->peer) return MM_ERROR;

    if (pad->peer->push) {
        return pad->peer->push(pad->peer, buf);
    }

    return MM_ERROR;
}

mm_result_t
mm_pad_pull(mm_pad_t* pad, mm_buffer_t** out)
{
    if (!pad || !out) return MM_ERROR;
    if (pad->direction != MM_PAD_SINK) return MM_ERROR;
    if (!pad->peer) return MM_ERROR;

    if (pad->peer->pull) {
        return pad->peer->pull(pad->peer, out);
    }

    return MM_ERROR;
}

void
mm_pad_reset_callbacks(mm_pad_t* pad)
{
    if (!pad) return;
    if (pad->direction == MM_PAD_SRC) {
        pad->pull = default_src_pad_pull;
        pad->push = NULL;
    } else {
        pad->push = default_sink_pad_push;
        pad->pull = NULL;
    }
}
