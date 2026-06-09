/*=============================================================================
    mm_element.c — Element lifecycle, state machine, pad management
=============================================================================*/

#include "mm_element.h"
#include <stdlib.h>
#include <string.h>

mm_element_t*
mm_element_create(const mm_element_ops_t* ops, void* priv)
{
    if (!ops) return NULL;

    mm_element_t* el = calloc(1, sizeof(*el));
    if (!el) return NULL;

    el->ops          = ops;
    el->state        = MM_STATE_NULL;
    el->src_pads     = NULL;
    el->nb_src_pads  = 0;
    el->sink_pads    = NULL;
    el->nb_sink_pads = 0;
    el->priv         = priv;

    return el;
}

void
mm_element_destroy(mm_element_t* el)
{
    if (!el) return;

    /* Destroy all pads */
    for (uint32_t i = 0; i < el->nb_src_pads; i++)
        mm_pad_destroy(el->src_pads[i]);
    free(el->src_pads);

    for (uint32_t i = 0; i < el->nb_sink_pads; i++)
        mm_pad_destroy(el->sink_pads[i]);
    free(el->sink_pads);

    free(el->priv);
    free(el);
}

mm_result_t
mm_element_set_state(mm_element_t* el, mm_state_t state)
{
    if (!el) return MM_ERROR;

    if (state != MM_STATE_NULL && state != MM_STATE_READY &&
        state != MM_STATE_PAUSED && state != MM_STATE_PLAYING) {
        return MM_ERROR;
    }

    if (el->state == state) return MM_OK;

    /* Call the appropriate lifecycle hook */
    mm_result_t ret = MM_OK;

    /* Transition NULL -> READY */
    if (el->state < MM_STATE_READY && state >= MM_STATE_READY) {
        if (el->ops->open)
            ret = el->ops->open(el);
        if (ret != MM_OK) return ret;
    }

    /* Transition READY -> PAUSED */
    if (el->state < MM_STATE_PAUSED && state >= MM_STATE_PAUSED) {
        /* no default action */
    }

    /* Transition PAUSED -> PLAYING */
    if (el->state < MM_STATE_PLAYING && state >= MM_STATE_PLAYING) {
        if (el->ops->start)
            ret = el->ops->start(el);
        if (ret != MM_OK) return ret;
    }

    /* Transition PLAYING -> PAUSED */
    if (el->state >= MM_STATE_PLAYING && state < MM_STATE_PLAYING) {
        if (el->ops->stop)
            ret = el->ops->stop(el);
        if (ret != MM_OK) return ret;
    }

    /* Transition PAUSED / READY -> NULL */
    if (el->state >= MM_STATE_READY && state < MM_STATE_READY) {
        if (el->ops->close)
            ret = el->ops->close(el);
        if (ret != MM_OK) return ret;
    }

    el->state = state;
    return ret;
}

mm_pad_t*
mm_element_get_pad(mm_element_t* el, const char* name)
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

mm_result_t
mm_element_add_pad(mm_element_t* el, mm_pad_t* pad)
{
    if (!el || !pad) return MM_ERROR;

    pad->parent = el;

    if (pad->direction == MM_PAD_SRC) {
        mm_pad_t** pads = realloc(el->src_pads,
                                  (el->nb_src_pads + 1) * sizeof(mm_pad_t*));
        if (!pads) return MM_ERROR;
        pads[el->nb_src_pads++] = pad;
        el->src_pads = pads;
    } else {
        mm_pad_t** pads = realloc(el->sink_pads,
                                  (el->nb_sink_pads + 1) * sizeof(mm_pad_t*));
        if (!pads) return MM_ERROR;
        pads[el->nb_sink_pads++] = pad;
        el->sink_pads = pads;
    }

    return MM_OK;
}
