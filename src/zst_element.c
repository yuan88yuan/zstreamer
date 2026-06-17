/*=============================================================================
    zst_element.c - Cache-aligned structure-splitting & isolated state lock
=============================================================================*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "zst_element.h"
#include "zst_log.h"
#include "zst_bus.h"
#include "zst_buffer_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_MSC_VER)
#define zst_aligned_alloc(al, sz) _aligned_malloc(sz, al)
#define zst_aligned_free(ptr) _aligned_free(ptr)
#else
#define zst_aligned_alloc(al, sz) aligned_alloc(al, ((sz + al - 1) / al) * al)
#define zst_aligned_free(ptr) free(ptr)
#endif

zst_element_t* zst_element_create(const zst_element_ops_t* ops, void* priv) {
    zst_element_t* el = (zst_element_t*)zst_aligned_alloc(ZST_CACHE_LINE_SIZE, sizeof(zst_element_t));
    if (!el) return NULL;

    memset(el, 0, sizeof(zst_element_t));
    el->ops = ops;
    el->priv = priv;
    atomic_init(&el->state, ZST_STATE_NULL);
    atomic_init(&el->is_queued, false);
    el->graph_rank = 0;

    pthread_mutex_init(&el->state_lock, NULL);

    return el;
}

void zst_element_destroy(zst_element_t* el) {
    if (!el) return;

    /* Transition down to NULL state dynamically first to cleanup hardware descriptors */
    zst_element_set_state(el, ZST_STATE_NULL);

    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        zst_pad_destroy(el->src_pads[i]);
    }
    free(el->src_pads);

    for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
        zst_pad_destroy(el->sink_pads[i]);
    }
    free(el->sink_pads);

    pthread_mutex_destroy(&el->state_lock);
    zst_aligned_free(el);
}

zst_result_t zst_element_set_state(zst_element_t* el, zst_state_t state) {
    if (!el) return ZST_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&el->state_lock);
    zst_state_t current = atomic_load_explicit(&el->state, memory_order_relaxed);

    if (current == state) {
        pthread_mutex_unlock(&el->state_lock);
        return ZST_OK;
    }

    zst_result_t result = ZST_OK;

    /* Explicit forward state machine transition operations */
    if (current == ZST_STATE_NULL && state == ZST_STATE_READY) {
        if (el->ops && el->ops->open) {
            result = el->ops->open(el);
        }
    } else if (current == ZST_STATE_READY && state == ZST_STATE_PLAYING) {
        if (el->ops && el->ops->start) {
            result = el->ops->start(el);
        }
    } else if (current == ZST_STATE_PLAYING && state == ZST_STATE_READY) {
        if (el->ops && el->ops->stop) {
            result = el->ops->stop(el);
        }
    } else if (current == ZST_STATE_READY && state == ZST_STATE_NULL) {
        if (el->ops && el->ops->close) {
            result = el->ops->close(el);
        }
    }

    if (result == ZST_OK) {
        atomic_store_explicit(&el->state, state, memory_order_release);
    }

    pthread_mutex_unlock(&el->state_lock);
    return result;
}

zst_pad_t* zst_element_get_pad(zst_element_t* el, const char* name) {
    if (!el || !name) return NULL;

    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        if (strcmp(el->src_pads[i]->name, name) == 0) return el->src_pads[i];
    }
    for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
        if (strcmp(el->sink_pads[i]->name, name) == 0) return el->sink_pads[i];
    }
    return NULL;
}

zst_result_t zst_element_add_pad(zst_element_t* el, zst_pad_t* pad) {
    if (!el || !pad) return ZST_ERROR_INVALID_ARGUMENT;

    pad->parent = el;
    if (pad->direction == ZST_PAD_SRC) {
        el->src_pads = (zst_pad_t**)realloc(el->src_pads, sizeof(zst_pad_t*) * (el->nb_src_pads + 1));
        el->src_pads[el->nb_src_pads++] = pad;
    } else {
        el->sink_pads = (zst_pad_t**)realloc(el->sink_pads, sizeof(zst_pad_t*) * (el->nb_sink_pads + 1));
        el->sink_pads[el->nb_sink_pads++] = pad;
    }
    return ZST_OK;
}

void zst_element_set_clock(zst_element_t* el, zst_clock_t* clock) {
    if (!el) return;
    pthread_mutex_lock(&el->state_lock);
    el->clock = clock;
    pthread_mutex_unlock(&el->state_lock);
}

zst_result_t zst_element_set_property(zst_element_t* el, const char* name, const char* value) {
    if (!el || !el->ops || !el->ops->set_property) return ZST_ERROR_NOT_IMPLEMENTED;
    pthread_mutex_lock(&el->state_lock);
    zst_result_t ret = el->ops->set_property(el, name, value);
    pthread_mutex_unlock(&el->state_lock);
    return ret;
}

zst_result_t zst_element_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    if (!el || !el->ops || !el->ops->get_property) return ZST_ERROR_NOT_IMPLEMENTED;
    pthread_mutex_lock(&el->state_lock);
    zst_result_t ret = el->ops->get_property(el, name, value_out, max_len);
    pthread_mutex_unlock(&el->state_lock);
    return ret;
}

/* Helpers for handling safe atomic conversions from typed attributes */
zst_result_t zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t zst_element_set_property_double(zst_element_t* el, const char* name, double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t zst_element_set_property_bool(zst_element_t* el, const char* name, bool value) {
    return zst_element_set_property(el, name, value ? "true" : "false");
}

zst_result_t zst_element_set_property_string(zst_element_t* el, const char* name, const char* value) {
    return zst_element_set_property(el, name, value);
}

zst_result_t zst_element_get_property_string(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    return zst_element_get_property(el, name, value_out, max_len);
}

zst_result_t zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out) {
    if (!el || !value_out) return ZST_ERROR_INVALID_ARGUMENT;
    char buf[64];
    zst_result_t ret = zst_element_get_property(el, name, buf, sizeof(buf));
    if (ret == ZST_OK) {
        char* end = NULL;
        *value_out = (int64_t)strtoll(buf, &end, 10);
        if (end == buf) return ZST_ERROR;
    }
    return ret;
}

zst_result_t zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out) {
    if (!el || !value_out) return ZST_ERROR_INVALID_ARGUMENT;
    char buf[64];
    zst_result_t ret = zst_element_get_property(el, name, buf, sizeof(buf));
    if (ret == ZST_OK) {
        char* end = NULL;
        *value_out = (uint64_t)strtoull(buf, &end, 10);
        if (end == buf) return ZST_ERROR;
    }
    return ret;
}

zst_result_t zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out) {
    if (!el || !value_out) return ZST_ERROR_INVALID_ARGUMENT;
    char buf[64];
    zst_result_t ret = zst_element_get_property(el, name, buf, sizeof(buf));
    if (ret == ZST_OK) {
        char* end = NULL;
        *value_out = strtod(buf, &end);
        if (end == buf) return ZST_ERROR;
    }
    return ret;
}

zst_result_t zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out) {
    if (!el || !value_out) return ZST_ERROR_INVALID_ARGUMENT;
    char buf[8];
    zst_result_t ret = zst_element_get_property(el, name, buf, sizeof(buf));
    if (ret == ZST_OK) {
        *value_out = (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
    }
    return ret;
}

zst_buffer_pool_t* zst_element_get_pool(zst_element_t* el) {
    if (!el || !el->ops || !el->ops->get_pool) return NULL;
    return el->ops->get_pool(el);
}

zst_result_t zst_element_seek(zst_element_t* el, double rate, const zst_segment_t* segment) {
    if (!el) return ZST_ERROR_INVALID_ARGUMENT;
    zst_segment_t seg;
    if (segment) {
        seg = *segment;
    } else {
        seg = zst_segment_default();
    }
    seg.rate = rate;
    if (el->bus) {
        zst_event_t* ev = zst_event_new_segment(el, &seg);
        if (ev) {
            zst_bus_post(el->bus, ev);
        }
    }
    return ZST_OK;
}