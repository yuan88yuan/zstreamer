/*=============================================================================
    mm_pipeline.c — Element container with state propagation
=============================================================================*/

#include "mm_pipeline.h"
#include "mm_bus.h"
#include <stdlib.h>
#include <string.h>

mm_pipeline_t*
mm_pipeline_create(void)
{
    mm_pipeline_t* pipe = calloc(1, sizeof(*pipe));
    if (!pipe) return NULL;

    pipe->elements    = NULL;
    pipe->nb_elements = 0;
    pipe->state       = MM_STATE_NULL;
    pipe->priv        = NULL;
    pipe->bus         = mm_bus_create();

    return pipe;
}

void
mm_pipeline_destroy(mm_pipeline_t* pipe)
{
    if (!pipe) return;

    /* Destroy all elements (in reverse order) */
    for (uint32_t i = pipe->nb_elements; i > 0; i--)
        mm_element_destroy(pipe->elements[i - 1]);

    if (pipe->bus) {
        mm_bus_destroy(pipe->bus);
    }

    free(pipe->elements);
    free(pipe);
}

mm_bus_t*
mm_pipeline_get_bus(mm_pipeline_t* pipe)
{
    return pipe ? pipe->bus : NULL;
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
    el->bus = pipe->bus;
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

    if (found) {
        el->bus = NULL;
        return MM_OK;
    }
    return MM_ERROR;
}

mm_result_t
mm_pipeline_set_state(mm_pipeline_t* pipe, mm_state_t state)
{
    if (!pipe) return MM_ERROR;

    mm_state_t old_state = pipe->state;
    if (old_state == state) return MM_OK;

    /* Propagate state to all elements */
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        mm_result_t r = mm_element_set_state(pipe->elements[i], state);
        if (r != MM_OK) {
            /* Error rollback: revert previous elements to old_state */
            for (uint32_t j = 0; j < i; j++) {
                mm_element_set_state(pipe->elements[j], old_state);
            }
            /* Post MM_EVENT_ERROR */
            if (pipe->bus) {
                mm_event_t* ev = mm_event_new_error(pipe->elements[i], r, "Element failed to set state");
                mm_bus_post(pipe->bus, ev);
            }
            return r;
        }
    }

    pipe->state = state;
    if (pipe->bus) {
        mm_event_t* ev = mm_event_new_state_changed(NULL, old_state, state);
        mm_bus_post(pipe->bus, ev);
    }
    return MM_OK;
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

static void
dfs_sort(mm_element_t* el, mm_element_t** temp, uint32_t* temp_idx, int* visited, mm_pipeline_t* pipe)
{
    int idx = -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1 || visited[idx]) return;

    visited[idx] = 1;

    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        mm_pad_t* src_pad = el->src_pads[i];
        if (src_pad->peer && src_pad->peer->parent) {
            dfs_sort(src_pad->peer->parent, temp, temp_idx, visited, pipe);
        }
    }

    temp[--(*temp_idx)] = el;
}

void
mm_pipeline_topological_sort(mm_pipeline_t* pipe)
{
    if (!pipe || pipe->nb_elements <= 1) return;

    mm_element_t** temp = malloc(pipe->nb_elements * sizeof(mm_element_t*));
    if (!temp) return;

    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) {
        free(temp);
        return;
    }

    uint32_t temp_idx = pipe->nb_elements;

    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i]->nb_sink_pads == 0) {
            dfs_sort(pipe->elements[i], temp, &temp_idx, visited, pipe);
        }
    }

    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (!visited[i]) {
            dfs_sort(pipe->elements[i], temp, &temp_idx, visited, pipe);
        }
    }

    memcpy(pipe->elements, temp, pipe->nb_elements * sizeof(mm_element_t*));

    free(visited);
    free(temp);
}
