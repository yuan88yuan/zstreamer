/*=============================================================================
    zst_pad.c — Pad creation, linking, and unlinking
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static zst_result_t
default_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    if (!el || !el->ops) return ZST_ERROR;

    /* Handle drop flag (propagate downstream or skip immediately) */
    if (buf && (buf->flags & ZST_BUFFER_FLAG_DROP)) {
        if (el->nb_src_pads > 0) {
            return zst_pad_push(el->src_pads[0], buf);
        }
        return ZST_OK;
    }

    if (buf && (buf->flags & ZST_BUFFER_FLAG_EOS)) {
        if (el->nb_src_pads > 0) {
            return zst_pad_push(el->src_pads[0], buf);
        }
        /* Sink element receiving EOS */
        if (el->bus) {
            zst_event_t* eos_ev = zst_event_new_eos(el);
            zst_bus_post(el->bus, eos_ev);
        }
        return ZST_OK;
    }

    /* Sink element clock synchronization and QoS dropping */
    if (el->nb_src_pads == 0 && el->clock && buf && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_time_t current = zst_clock_get_time(el->clock);
        if (buf->pts > current + 5000000ULL) { /* 5ms early threshold */
            if (buf->pts - current < 5000000000ULL) { /* 5s safeguard */
                zst_clock_wait(el->clock, buf->pts - current);
            }
        } else if (buf->pts < current - 100000000ULL) { /* 100ms late threshold */
            if (current - buf->pts < 5000000000ULL) { /* 5s safeguard */
                /* Drop late buffer to catch up (QoS) */
                buf->flags |= ZST_BUFFER_FLAG_DROP;
                if (el->bus) {
                    zst_event_t* qos_ev = zst_event_new_warning(el, ZST_ERROR, "QoS: Frame dropped (too late)");
                    zst_bus_post(el->bus, qos_ev);
                }
                return ZST_OK;
            }
        }
    }

    zst_buffer_t* out_buf = NULL;
    zst_result_t ret = ZST_OK;

    if (el->ops->process) {
        ret = el->ops->process(el, buf, &out_buf);
    }

    if (ret == ZST_OK && out_buf) {
        if (el->nb_src_pads > 0) {
            ret = zst_pad_push(el->src_pads[0], out_buf);
            if (out_buf != buf) {
                zst_buffer_unref(out_buf);
            } else if (out_buf->refcount > 1) {
                /* Some in-place elements return zst_buffer_ref(in) so direct
                   process() callers own an output reference.  Drop that extra
                   reference in the pad-driven path while preserving the
                   upstream caller's original reference. */
                zst_buffer_unref(out_buf);
            }
        }
    }

    if (ret != ZST_OK && ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
        if (el->bus) {
            zst_event_t* err_ev = zst_event_new_error(el, ret, "Element push processing failed");
            zst_bus_post(el->bus, err_ev);
        }
    }

    return ret;
}

static zst_result_t
default_src_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    zst_element_t* el = pad->parent;
    if (!el || !el->ops) return ZST_ERROR;

    zst_buffer_t* out_buf = NULL;
    zst_result_t ret = ZST_OK;

    if (el->nb_sink_pads > 0) {
        zst_buffer_t* in_buf = NULL;
        ret = zst_pad_pull(el->sink_pads[0], &in_buf);
        if (ret != ZST_OK) {
            if (ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
                if (el->bus) {
                    zst_event_t* err_ev = zst_event_new_error(el, ret, "Upstream pull failed");
                    zst_bus_post(el->bus, err_ev);
                }
            }
            return ret;
        }

        if (in_buf && (in_buf->flags & ZST_BUFFER_FLAG_EOS)) {
            *out = in_buf;
            return ZST_OK;
        }

        if (el->ops->process) {
            ret = el->ops->process(el, in_buf, &out_buf);
        }
        zst_buffer_unref(in_buf);
    } else {
        if (el->ops->process) {
            ret = el->ops->process(el, NULL, &out_buf);
        }
    }

    if (ret == ZST_OK) {
        *out = out_buf;
    } else {
        if (ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
            if (el->bus) {
                zst_event_t* err_ev = zst_event_new_error(el, ret, "Element pull processing failed");
                zst_bus_post(el->bus, err_ev);
            }
        }
    }

    return ret;
}

zst_pad_t*
zst_pad_create(const char* name, zst_pad_direction_t direction)
{
    zst_pad_t* pad = calloc(1, sizeof(*pad));
    if (!pad) return NULL;

    pad->name          = name ? strdup(name) : NULL;
    pad->direction     = direction;
    pad->parent        = NULL;
    pad->caps          = NULL;
    pad->template_caps = NULL;
    if (direction == ZST_PAD_SRC) {
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
zst_pad_destroy(zst_pad_t* pad)
{
    if (!pad) return;

    /* Unlink from peer if still connected */
    if (pad->peer)
        zst_pad_unlink(pad);

    free((void*)pad->name);
    zst_caps_destroy(pad->caps);
    zst_caps_destroy(pad->template_caps);
    free(pad);
}

zst_result_t
zst_pad_link(zst_pad_t* src, zst_pad_t* sink)
{
    if (!src || !sink)
        return ZST_ERROR;

    if (src->direction != ZST_PAD_SRC)
        return ZST_ERROR;

    if (sink->direction != ZST_PAD_SINK)
        return ZST_ERROR;

    /* Refuse if either pad is already linked */
    if (src->peer || sink->peer)
        return ZST_ERROR;

    /* Negotiate caps first */
    zst_result_t ret = zst_pad_negotiate(src, sink);
    if (ret != ZST_OK) {
        return ret;
    }

    src->peer = sink;
    sink->peer = src;

    return ZST_OK;
}

void
zst_pad_unlink(zst_pad_t* pad)
{
    if (!pad) return;

    zst_pad_t* peer = pad->peer;
    if (!peer) return;

    /* Break the link from both sides */
    pad->peer = NULL;
    peer->peer = NULL;
}

zst_result_t
zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SRC) return ZST_ERROR;
    if (!pad->peer) return ZST_ERROR;

    if (pad->peer->push) {
        return pad->peer->push(pad->peer, buf);
    }

    return ZST_ERROR;
}

zst_result_t
zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    if (!pad || !out) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SINK) return ZST_ERROR;
    if (!pad->peer) return ZST_ERROR;

    if (pad->peer->pull) {
        return pad->peer->pull(pad->peer, out);
    }

    return ZST_ERROR;
}

void
zst_pad_reset_callbacks(zst_pad_t* pad)
{
    if (!pad) return;
    if (pad->direction == ZST_PAD_SRC) {
        pad->pull = default_src_pad_pull;
        pad->push = NULL;
    } else {
        pad->push = default_sink_pad_push;
        pad->pull = NULL;
    }
}

zst_result_t
zst_pad_set_caps(zst_pad_t* pad, const zst_caps_t* caps)
{
    if (!pad) return ZST_ERROR;
    
    if (pad->caps) {
        zst_caps_destroy(pad->caps);
        pad->caps = NULL;
    }
    
    if (caps) {
        pad->caps = zst_caps_copy(caps);
        if (!pad->caps) return ZST_ERROR;
    }
    
    return ZST_OK;
}

zst_caps_t*
zst_pad_get_caps(zst_pad_t* pad)
{
    if (!pad) return NULL;
    
    if (pad->caps) {
        return zst_caps_copy(pad->caps);
    }
    
    zst_element_t* el = pad->parent;
    if (el && el->ops && el->ops->get_caps) {
        zst_caps_t* caps = el->ops->get_caps(el, pad, NULL);
        if (caps) return caps;
    }
    
    if (pad->template_caps) {
        return zst_caps_copy(pad->template_caps);
    }
    
    return NULL;
}

zst_result_t
zst_pad_set_template_caps(zst_pad_t* pad, const zst_caps_t* caps)
{
    if (!pad) return ZST_ERROR;
    
    if (pad->template_caps) {
        zst_caps_destroy(pad->template_caps);
        pad->template_caps = NULL;
    }
    
    if (caps) {
        pad->template_caps = zst_caps_copy(caps);
        if (!pad->template_caps) return ZST_ERROR;
    }
    
    return ZST_OK;
}

zst_result_t
zst_pad_negotiate(zst_pad_t* src, zst_pad_t* sink)
{
    if (!src || !sink) return ZST_ERROR;
    
    zst_caps_t* src_caps = zst_pad_get_caps(src);
    zst_caps_t* sink_caps = zst_pad_get_caps(sink);
    
    if (!src_caps || !sink_caps) {
        if (src_caps) zst_caps_destroy(src_caps);
        if (sink_caps) zst_caps_destroy(sink_caps);
        return ZST_OK;
    }
    
    zst_caps_t* intersect = zst_caps_intersect(src_caps, sink_caps);
    zst_caps_destroy(src_caps);
    zst_caps_destroy(sink_caps);
    
    if (!intersect) {
        return ZST_ERROR;
    }
    
    if (!intersect->structs) {
        zst_caps_destroy(intersect);
        return ZST_ERROR;
    }
    
    zst_result_t ret = zst_caps_fixate(intersect);
    if (ret != ZST_OK) {
        zst_caps_destroy(intersect);
        return ZST_ERROR;
    }
    
    ret = zst_pad_set_caps(src, intersect);
    if (ret == ZST_OK) {
        ret = zst_pad_set_caps(sink, intersect);
    }
    
    zst_caps_destroy(intersect);
    return ret;
}
