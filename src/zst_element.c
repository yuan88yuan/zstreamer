/*=============================================================================
    zst_element.c — Element lifecycle, state machine, pad management
=============================================================================*/

#include "zst_element.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <string.h>

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
    el->clock        = NULL;

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

    free(el->priv);
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
