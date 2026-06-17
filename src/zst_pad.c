/*=============================================================================
    zst_pad.c - High-performance peer pads with lock-free probe fast-path
=============================================================================*/
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_log.h"
#include <stdlib.h>
#include <string.h>

zst_pad_t* zst_pad_create(const char* name, zst_pad_direction_t direction) {
    zst_pad_t* pad = (zst_pad_t*)malloc(sizeof(zst_pad_t));
    if (!pad) return NULL;

    memset(pad, 0, sizeof(zst_pad_t));
    pad->name = strdup(name);
    pad->direction = direction;
    pad->spillover_policy = ZST_SPILLOVER_BLOCK;
    atomic_init(&pad->has_probes, 0);
    atomic_init(&pad->blocked, 0);

    pthread_mutex_init(&pad->probe_lock, NULL);
    pthread_cond_init(&pad->probe_cond, NULL);

    return pad;
}

void zst_pad_destroy(zst_pad_t* pad) {
    if (!pad) return;

    zst_pad_unlink(pad);

    if (pad->destroy_priv && pad->priv) {
        pad->destroy_priv(pad);
    }

    zst_pad_probe_t* probe = pad->probes;
    while (probe) {
        zst_pad_probe_t* next = probe->next;
        free(probe);
        probe = next;
    }

    if (pad->caps) zst_caps_destroy(pad->caps);
    if (pad->template_caps) zst_caps_destroy(pad->template_caps);

    pthread_mutex_destroy(&pad->probe_lock);
    pthread_cond_destroy(&pad->probe_cond);

    free((void*)pad->name);
    free(pad);
}

zst_result_t zst_pad_link(zst_pad_t* src, zst_pad_t* sink) {
    if (!src || !sink) return ZST_ERROR;
    if (src->direction != ZST_PAD_SRC || sink->direction != ZST_PAD_SINK) {
        return ZST_ERROR;
    }

    pthread_mutex_lock(&src->probe_lock);
    pthread_mutex_lock(&sink->probe_lock);

    src->peer = sink;
    sink->peer = src;

    pthread_mutex_unlock(&sink->probe_lock);
    pthread_mutex_unlock(&src->probe_lock);

    return ZST_OK;
}

void zst_pad_unlink(zst_pad_t* pad) {
    if (!pad || !pad->peer) return;

    zst_pad_t* peer = pad->peer;

    pthread_mutex_lock(&pad->probe_lock);
    pthread_mutex_lock(&peer->probe_lock);

    pad->peer = NULL;
    peer->peer = NULL;

    pthread_mutex_unlock(&peer->probe_lock);
    pthread_mutex_unlock(&pad->probe_lock);
}

static zst_pad_probe_return_t execute_probes(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type) {
    /* Safe fast path check - if no probes have been registered, return immediately */
    if (atomic_load_explicit(&pad->has_probes, memory_order_relaxed) == 0) {
        return ZST_PAD_PROBE_OK;
    }

    zst_pad_probe_return_t ret = ZST_PAD_PROBE_OK;
    pthread_mutex_lock(&pad->probe_lock);
    
    zst_pad_probe_t* curr = pad->probes;
    while (curr) {
        if (curr->types & type) {
            ret = curr->callback(pad, buf, type, curr->user_data);
            if (ret != ZST_PAD_PROBE_OK) {
                break;
            }
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&pad->probe_lock);
    return ret;
}

zst_result_t zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf) {
    if (!pad || !buf) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SRC) return ZST_ERROR;

    /* Lock-free Pre-buffer Probe Check */
    if (execute_probes(pad, buf, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP) {
        zst_buffer_unref(buf);
        return ZST_OK;
    }

    /* Handle dynamic pad blocking */
    if (atomic_load_explicit(&pad->blocked, memory_order_acquire)) {
        pthread_mutex_lock(&pad->probe_lock);
        while (atomic_load_explicit(&pad->blocked, memory_order_relaxed)) {
            if (pad->block_callback && !pad->block_callback_fired) {
                pad->block_callback_fired = 1;
                pad->block_callback(pad, buf, ZST_PAD_PROBE_PRE_BUFFER, pad->block_user_data);
            }
            pthread_cond_wait(&pad->probe_cond, &pad->probe_lock);
        }
        pthread_mutex_unlock(&pad->probe_lock);
    }

    zst_pad_t* peer = pad->peer;
    if (!peer) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }

    zst_result_t result = ZST_OK;
    if (peer->push) {
        result = peer->push(peer, buf);
    } else {
        /* Default upstream spillover configuration handling */
        if (pad->spillover_policy == ZST_SPILLOVER_DROP_NEW) {
            zst_buffer_unref(buf);
            result = ZST_OK; /* Dropped under policy */
        } else {
            result = ZST_ERROR;
        }
    }

    /* Lock-free Post-buffer Probe Check */
    execute_probes(pad, NULL, ZST_PAD_PROBE_POST_BUFFER);

    return result;
}

zst_result_t zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out) {
    if (!pad || !out) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SINK) return ZST_ERROR;

    zst_pad_t* peer = pad->peer;
    if (!peer) return ZST_ERROR;

    if (peer->pull) {
        return peer->pull(peer, out);
    }
    return ZST_ERROR;
}

uint64_t zst_pad_add_probe(zst_pad_t* pad, uint32_t types, zst_pad_probe_fn callback, void* user_data) {
    if (!pad || !callback) return 0;

    pthread_mutex_lock(&pad->probe_lock);

    zst_pad_probe_t* probe = (zst_pad_probe_t*)malloc(sizeof(zst_pad_probe_t));
    probe->id = ++pad->next_probe_id;
    probe->types = types;
    probe->callback = callback;
    probe->user_data = user_data;
    probe->next = pad->probes;
    pad->probes = probe;

    /* Increment the atomic count to enable fast-path checks on incoming buffers */
    atomic_fetch_add_explicit(&pad->has_probes, 1, memory_order_release);

    pthread_mutex_unlock(&pad->probe_lock);
    return probe->id;
}

zst_result_t zst_pad_remove_probe(zst_pad_t* pad, uint64_t probe_id) {
    if (!pad) return ZST_ERROR;

    pthread_mutex_lock(&pad->probe_lock);

    zst_pad_probe_t* prev = NULL;
    zst_pad_probe_t* curr = pad->probes;
    while (curr) {
        if (curr->id == probe_id) {
            if (prev) {
                prev->next = curr->next;
            } else {
                pad->probes = curr->next;
            }
            free(curr);

            /* Decrement atomic count strictly inside critical section lock */
            atomic_fetch_sub_explicit(&pad->has_probes, 1, memory_order_release);
            
            pthread_mutex_unlock(&pad->probe_lock);
            return ZST_OK;
        }
        prev = curr;
        curr = curr->next;
    }

    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_ERROR;
}

zst_result_t zst_pad_block(zst_pad_t* pad) {
    if (!pad) return ZST_ERROR;
    atomic_store_explicit(&pad->blocked, 1, memory_order_release);
    return ZST_OK;
}

zst_result_t zst_pad_unblock(zst_pad_t* pad) {
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    atomic_store_explicit(&pad->blocked, 0, memory_order_release);
    pad->block_callback_fired = 0;
    pthread_cond_broadcast(&pad->probe_cond);
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

int zst_pad_is_blocked(zst_pad_t* pad) {
    return pad ? atomic_load_explicit(&pad->blocked, memory_order_acquire) : 0;
}

zst_result_t zst_pad_set_block_callback(zst_pad_t* pad, zst_pad_probe_fn callback, void* user_data) {
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    pad->block_callback = callback;
    pad->block_user_data = user_data;
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

zst_result_t zst_pad_set_caps(zst_pad_t* pad, const zst_caps_t* caps) {
    if (!pad) return ZST_ERROR_INVALID_ARGUMENT;
    if (pad->caps) zst_caps_destroy(pad->caps);
    pad->caps = caps ? zst_caps_copy(caps) : NULL;
    return ZST_OK;
}

zst_caps_t* zst_pad_get_caps(zst_pad_t* pad) {
    if (!pad) return NULL;
    return pad->caps;
}

zst_result_t zst_pad_set_template_caps(zst_pad_t* pad, const zst_caps_t* caps) {
    if (!pad) return ZST_ERROR_INVALID_ARGUMENT;
    if (pad->template_caps) zst_caps_destroy(pad->template_caps);
    pad->template_caps = caps ? zst_caps_copy(caps) : NULL;
    return ZST_OK;
}

zst_result_t zst_pad_negotiate(zst_pad_t* src, zst_pad_t* sink) {
    if (!src || !sink) return ZST_ERROR_INVALID_ARGUMENT;
    if (src->direction != ZST_PAD_SRC || sink->direction != ZST_PAD_SINK) {
        return ZST_ERROR;
    }
    /* Simple negotiation: if src has template caps, copy to sink's caps */
    if (src->template_caps) {
        zst_caps_t* intersection = zst_caps_intersect(src->template_caps, sink->template_caps);
        if (intersection && intersection->structs) {
            zst_pad_set_caps(src, intersection);
            zst_pad_set_caps(sink, intersection);
            zst_caps_destroy(intersection);
            return ZST_OK;
        }
        if (intersection) zst_caps_destroy(intersection);
        /* Fall through to template caps check */
        zst_pad_set_caps(src, src->template_caps);
    }
    if (sink->template_caps) {
        zst_pad_set_caps(sink, sink->template_caps);
    }
    return ZST_OK;
}

void zst_pad_reset_callbacks(zst_pad_t* pad) {
    if (!pad) return;
    pthread_mutex_lock(&pad->probe_lock);
    pad->push = NULL;
    pad->pull = NULL;
    pad->block_callback = NULL;
    pad->block_user_data = NULL;
    pad->block_callback_fired = 0;
    /* Clear all probes */
    zst_pad_probe_t* probe = pad->probes;
    while (probe) {
        zst_pad_probe_t* next = probe->next;
        free(probe);
        probe = next;
    }
    pad->probes = NULL;
    atomic_store_explicit(&pad->has_probes, 0, memory_order_release);
    pthread_mutex_unlock(&pad->probe_lock);
}

zst_result_t zst_pad_set_segment(zst_pad_t* pad, const zst_segment_t* segment) {
    if (!pad || !segment) return ZST_ERROR_INVALID_ARGUMENT;
    pad->segment = *segment;
    pad->has_segment = 1;
    return ZST_OK;
}

zst_result_t zst_pad_get_segment(zst_pad_t* pad, zst_segment_t* segment_out) {
    if (!pad || !segment_out) return ZST_ERROR_INVALID_ARGUMENT;
    if (!pad->has_segment) return ZST_ERROR;
    *segment_out = pad->segment;
    return ZST_OK;
}

void zst_pad_clear_segment(zst_pad_t* pad) {
    if (!pad) return;
    pad->has_segment = 0;
    memset(&pad->segment, 0, sizeof(pad->segment));
}

zst_result_t zst_pad_push_segment(zst_pad_t* src, const zst_segment_t* segment) {
    if (!src || !segment) return ZST_ERROR_INVALID_ARGUMENT;
    zst_pad_t* peer = src->peer;
    if (!peer) return ZST_ERROR;
    return zst_pad_set_segment(peer, segment);
}