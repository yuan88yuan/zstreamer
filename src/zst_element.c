/*=============================================================================
    zst_element.c - Element lifecycle, state machine, pad management
=============================================================================*/

#include "zst_element.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_clock.h"
#include "zst_buffer.h"
#include "zst_element_factory.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int zst_bin_element_is_bin(zst_element_t* el);
extern zst_result_t zst_bin_element_change_state(zst_element_t* el,
                                                 zst_state_t old_state,
                                                 zst_state_t new_state);
extern int zst_bin_element_destroy(zst_element_t* el);

zst_element_t*
zst_element_create(const zst_element_ops_t* ops, void* priv)
{
    if (!ops) return NULL;

    zst_element_t* el = calloc(1, sizeof(*el));
    if (!el) return NULL;

    el->ops          = ops;
    __atomic_store_n(&el->state, ZST_STATE_NULL, __ATOMIC_RELEASE);
    el->src_pads     = NULL;
    el->nb_src_pads  = 0;
    el->sink_pads    = NULL;
    el->nb_sink_pads = 0;
    el->priv         = priv;
    el->plugin       = NULL;
    el->desc         = NULL;
    el->clock        = NULL;
    el->pipeline     = NULL;

    /* Pre-allocate the scheduling token to optimize dispatch latency.
     * The token's base reference count is managed by the element itself. */
    el->sched_token = zst_buffer_create(ZST_BUFFER_USER);
    if (el->sched_token) {
        el->sched_token->memory.priv = el;
    } else {
        free(el);
        return NULL;
    }

    return el;
}

void
zst_element_destroy(zst_element_t* el)
{
    if (!el) return;

    /* Destroy all pads */
    for (uint32_t i = 0; i < el->nb_src_pads; i++)
        zst_pad_destroy(el->src_pads[i]);
    free(el->src_pads);

    for (uint32_t i = 0; i < el->nb_sink_pads; i++)
        zst_pad_destroy(el->sink_pads[i]);
    free(el->sink_pads);

    if (el->clock) {
        zst_clock_unref(el->clock);
    }

    /* Release the pre-allocated scheduling token */
    if (el->sched_token) {
        zst_buffer_unref(el->sched_token);
        el->sched_token = NULL;
    }

    if (!zst_bin_element_destroy(el)) {
        free(el->priv);
        el->priv = NULL;
    }

    zst_plugin_t* plugin = el->plugin;
    free(el);

    if (plugin) {
        zst_plugin_unref(plugin);
    }
}

zst_result_t
zst_element_set_state(zst_element_t* el, zst_state_t state)
{
    if (!el) return ZST_ERROR;

    if (state != ZST_STATE_NULL && state != ZST_STATE_READY &&
        state != ZST_STATE_PAUSED && state != ZST_STATE_PLAYING) {
        return ZST_ERROR;
    }

    zst_state_t current_state = __atomic_load_n(&el->state, __ATOMIC_ACQUIRE);
    if (current_state == state) return ZST_OK;

    /* Call the appropriate lifecycle hook */
    zst_result_t ret = ZST_OK;

    /* Transition NULL -> READY */
    if (current_state < ZST_STATE_READY && state >= ZST_STATE_READY) {
        if (el->ops->open)
            ret = el->ops->open(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition READY -> PAUSED */
    if (current_state < ZST_STATE_PAUSED && state >= ZST_STATE_PAUSED) {
        /* no default action */
    }

    /* Transition PAUSED -> PLAYING */
    if (current_state < ZST_STATE_PLAYING && state >= ZST_STATE_PLAYING) {
        if (el->ops->start)
            ret = el->ops->start(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition PLAYING -> PAUSED */
    if (current_state >= ZST_STATE_PLAYING && state < ZST_STATE_PLAYING) {
        if (el->ops->stop)
            ret = el->ops->stop(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition PAUSED / READY -> NULL */
    if (current_state >= ZST_STATE_READY && state < ZST_STATE_READY) {
        if (el->ops->close)
            ret = el->ops->close(el);
        if (ret != ZST_OK) return ret;
    }

    if (zst_bin_element_is_bin(el)) {
        ret = zst_bin_element_change_state(el, current_state, state);
        if (ret != ZST_OK) return ret;
    }

    __atomic_store_n(&el->state, state, __ATOMIC_RELEASE);
    if (state != current_state && el->bus) {
        zst_event_t* ev = zst_event_new_state_changed(el, current_state, state);
        zst_bus_post(el->bus, ev);
    }
    return ret;
}

zst_pad_t*
zst_element_get_pad(zst_element_t* el, const char* name)
{
    if (!el || !name) return NULL;

    /* Search source pads */
    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        if (el->src_pads[i]->name &&
            strcmp(el->src_pads[i]->name, name) == 0)
            return el->src_pads[i];
    }

    /* Search sink pads */
    for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
        if (el->sink_pads[i]->name &&
            strcmp(el->sink_pads[i]->name, name) == 0)
            return el->sink_pads[i];
    }

    return NULL;
}

zst_result_t
zst_element_add_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad) return ZST_ERROR;

    pad->parent = el;

    if (pad->direction == ZST_PAD_SRC) {
        zst_pad_t** pads = realloc(el->src_pads,
                                  (el->nb_src_pads + 1) * sizeof(zst_pad_t*));
        if (!pads) return ZST_ERROR;
        pads[el->nb_src_pads++] = pad;
        el->src_pads = pads;
    } else {
        zst_pad_t** pads = realloc(el->sink_pads,
                                  (el->nb_sink_pads + 1) * sizeof(zst_pad_t*));
        if (!pads) return ZST_ERROR;
        pads[el->nb_sink_pads++] = pad;
        el->sink_pads = pads;
    }

    return ZST_OK;
}

void
zst_element_set_clock(zst_element_t* el, zst_clock_t* clock)
{
    if (!el) return;
    if (el->clock == clock) return;
    if (el->clock) {
        zst_clock_unref(el->clock);
    }
    el->clock = clock ? zst_clock_ref(clock) : NULL;
}

zst_result_t
zst_element_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !name || !value) return ZST_ERROR;
    if (!el->ops->set_property) return ZST_ERROR;
    return el->ops->set_property(el, name, value);
}

zst_result_t
zst_element_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !name || !value_out || max_len == 0) return ZST_ERROR;
    if (!el->ops->get_property) return ZST_ERROR;
    return el->ops->get_property(el, name, value_out, max_len);
}

static const zst_property_spec_t*
zst_element_find_property_spec(zst_element_t* el, const char* name)
{
    if (!el || !el->desc || !name) return NULL;
    for (uint32_t i = 0; i < el->desc->nb_properties; i++) {
        const zst_property_spec_t* spec = &el->desc->properties[i];
        if (spec->name && strcmp(spec->name, name) == 0) {
            return spec;
        }
    }
    return NULL;
}

static zst_result_t
zst_element_check_property_type(zst_element_t* el, const char* name,
                                zst_property_type_t expected,
                                uint32_t access_flag)
{
    const zst_property_spec_t* spec = zst_element_find_property_spec(el, name);
    if (!spec) return ZST_OK;
    if ((spec->flags & access_flag) == 0) return ZST_ERROR;
    if (spec->type == expected) return ZST_OK;
    if (expected == ZST_PROPERTY_STRING && spec->type == ZST_PROPERTY_ENUM) return ZST_OK;
    if ((expected == ZST_PROPERTY_INT && spec->type == ZST_PROPERTY_UINT) ||
        (expected == ZST_PROPERTY_UINT && spec->type == ZST_PROPERTY_INT)) {
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_result_t
zst_element_set_property_string(zst_element_t* el, const char* name, const char* value)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_STRING,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_set_property(el, name, value);
}

zst_result_t
zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_INT,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%" PRId64, value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_UINT,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%" PRIu64, value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_double(zst_element_t* el, const char* name, double value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_DOUBLE,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%.17g", value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_bool(zst_element_t* el, const char* name, bool value)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_BOOL,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_set_property(el, name, value ? "true" : "false");
}

zst_result_t
zst_element_get_property_string(zst_element_t* el, const char* name,
                                char* value_out, size_t max_len)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_STRING,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_get_property(el, name, value_out, max_len);
}

zst_result_t
zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out)
{
    char buf[64];
    char* end = NULL;
    long long value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_INT,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtoll(buf, &end, 10);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = (int64_t)value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out)
{
    char buf[64];
    char* end = NULL;
    unsigned long long value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_UINT,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtoull(buf, &end, 10);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = (uint64_t)value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out)
{
    char buf[64];
    char* end = NULL;
    double value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_DOUBLE,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtod(buf, &end);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out)
{
    char buf[16];
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_BOOL,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
        *value_out = true;
        return ZST_OK;
    }
    if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0) {
        *value_out = false;
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_buffer_pool_t*
zst_element_get_pool(zst_element_t* el)
{
    if (!el || !el->ops || !el->ops->get_pool) return NULL;
    return el->ops->get_pool(el);
}

zst_result_t
zst_element_seek(zst_element_t* el, double rate, const zst_segment_t* segment)
{
    if (!el || !segment || rate == 0.0) return ZST_ERROR;
    if (segment->stop != ZST_SEGMENT_STOP_NONE && segment->stop < segment->start) {
        return ZST_ERROR;
    }

    zst_segment_t applied = *segment;
    applied.rate = rate;

    /* Generic source support for byte-addressable elements such as filesrc:
     * if the element exposes offset/length properties, map the segment range
     * onto those properties.  Timestamp-based sources can ignore this and
     * still benefit from downstream segment propagation/clipping. */
    int byte_seek = 0;
    if (el->ops && el->ops->set_property && applied.start <= INT64_MAX) {
        char value[64];
        snprintf(value, sizeof(value), "%" PRIu64, (uint64_t)applied.start);
        byte_seek = (zst_element_set_property(el, "offset", value) == ZST_OK);

        if (byte_seek && applied.stop == ZST_SEGMENT_STOP_NONE) {
            zst_element_set_property(el, "length", "-1");
        } else if (byte_seek && applied.stop >= applied.start &&
                   applied.stop - applied.start <= INT64_MAX) {
            snprintf(value, sizeof(value), "%" PRIu64,
                     (uint64_t)(applied.stop - applied.start));
            zst_element_set_property(el, "length", value);
        }
    }

    if (!byte_seek) {
        for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
            zst_pad_set_segment(el->sink_pads[i], &applied);
        }
        for (uint32_t i = 0; i < el->nb_src_pads; i++) {
            zst_pad_push_segment(el->src_pads[i], &applied);
        }
    }

    if (el->bus) {
        zst_event_t* ev = zst_event_new_segment(el, &applied);
        if (ev) zst_bus_post(el->bus, ev);
    }

    return ZST_OK;
}