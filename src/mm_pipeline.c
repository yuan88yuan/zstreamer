/*=============================================================================
    mm_pipeline.c — Element container with state propagation
=============================================================================*/

#include "mm_pipeline.h"
#include <stdlib.h>

mm_pipeline_t*
mm_pipeline_create(void)
{
    mm_pipeline_t* pipe = calloc(1, sizeof(*pipe));
    if (!pipe) return NULL;

    pipe->elements    = NULL;
    pipe->nb_elements = 0;
    pipe->state       = MM_STATE_NULL;
    pipe->priv        = NULL;

    return pipe;
}

void
mm_pipeline_destroy(mm_pipeline_t* pipe)
{
    if (!pipe) return;

    /* Destroy all elements (in reverse order) */
    for (uint32_t i = pipe->nb_elements; i > 0; i--)
        mm_element_destroy(pipe->elements[i - 1]);

    free(pipe->elements);
    free(pipe);
}

mm_result_t
mm_pipeline_add(mm_pipeline_t* pipe, mm_element_t* el)
{
    if (!pipe || !el) return MM_ERROR;

    mm_element_t** els = realloc(pipe->elements,
                                 (pipe->nb_elements + 1) * sizeof(mm_element_t*));
    if (!els) return MM_ERROR;

    els[pipe->nb_elements++] = el;
    pipe->elements = els;
    return MM_OK;
}

mm_result_t
mm_pipeline_remove(mm_pipeline_t* pipe, mm_element_t* el)
{
    if (!pipe || !el) return MM_ERROR;

    int found = 0;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            /* Shift remaining elements down */
            for (uint32_t j = i; j < pipe->nb_elements - 1; j++)
                pipe->elements[j] = pipe->elements[j + 1];
            pipe->nb_elements--;
            found = 1;
            break;
        }
    }

    return found ? MM_OK : MM_ERROR;
}

mm_result_t
mm_pipeline_set_state(mm_pipeline_t* pipe, mm_state_t state)
{
    if (!pipe) return MM_ERROR;

    mm_result_t ret = MM_OK;

    /* Propagate state to all elements */
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        mm_result_t r = mm_element_set_state(pipe->elements[i], state);
        if (r != MM_OK) ret = r;
    }

    if (ret == MM_OK)
        pipe->state = state;

    return ret;
}

mm_result_t
mm_pipeline_start(mm_pipeline_t* pipe)
{
    return mm_pipeline_set_state(pipe, MM_STATE_PLAYING);
}

mm_result_t
mm_pipeline_stop(mm_pipeline_t* pipe)
{
    return mm_pipeline_set_state(pipe, MM_STATE_NULL);
}
