/*=============================================================================
    zst_pipeline.c — Element container with state propagation
=============================================================================*/

#include "zst_pipeline.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <string.h>

zst_pipeline_t*
zst_pipeline_create(void)
{
    zst_pipeline_t* pipe = calloc(1, sizeof(*pipe));
    if (!pipe) return NULL;

    pipe->elements    = NULL;
    pipe->nb_elements = 0;
    pipe->state       = ZST_STATE_NULL;
    pipe->priv        = NULL;
    pipe->bus         = zst_bus_create();
    pipe->clock       = NULL;

    return pipe;
}

void
zst_pipeline_destroy(zst_pipeline_t* pipe)
{
    if (!pipe) return;

    /* Destroy all elements (in reverse order) */
    for (uint32_t i = pipe->nb_elements; i > 0; i--)
        zst_element_destroy(pipe->elements[i - 1]);

    if (pipe->bus) {
        zst_bus_destroy(pipe->bus);
    }

    if (pipe->clock) {
        zst_clock_unref(pipe->clock);
    }

    free(pipe->elements);
    free(pipe);
}

zst_bus_t*
zst_pipeline_get_bus(zst_pipeline_t* pipe)
{
    return pipe ? pipe->bus : NULL;
}

void
zst_pipeline_set_clock(zst_pipeline_t* pipe, zst_clock_t* clock)
{
    if (!pipe) return;
    if (pipe->clock == clock) return;
    if (pipe->clock) {
        zst_clock_unref(pipe->clock);
    }
    pipe->clock = clock ? zst_clock_ref(clock) : NULL;

    /* Propagate to all elements */
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_set_clock(pipe->elements[i], pipe->clock);
    }
}

zst_clock_t*
zst_pipeline_get_clock(zst_pipeline_t* pipe)
{
    return pipe ? pipe->clock : NULL;
}

zst_result_t
zst_pipeline_add(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    zst_element_t** els = realloc(pipe->elements,
                                 (pipe->nb_elements + 1) * sizeof(zst_element_t*));
    if (!els) return ZST_ERROR;

    els[pipe->nb_elements++] = el;
    pipe->elements = els;
    el->bus = pipe->bus;
    zst_element_set_clock(el, pipe->clock);
    return ZST_OK;
}

zst_result_t
zst_pipeline_remove(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

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
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_result_t
zst_pipeline_set_state(zst_pipeline_t* pipe, zst_state_t state)
{
    if (!pipe) return ZST_ERROR;

    zst_state_t old_state = pipe->state;
    if (old_state == state) return ZST_OK;

    /* Transition to PLAYING: Auto-select clock if none exists */
    if (old_state < ZST_STATE_PLAYING && state == ZST_STATE_PLAYING && !pipe->clock) {
        zst_clock_t* master_clock = NULL;
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            zst_element_t* el = pipe->elements[i];
            if (el->ops && el->ops->provide_clock) {
                master_clock = el->ops->provide_clock(el);
                if (master_clock) break;
            }
        }

        zst_clock_t* sys_clock = zst_clock_system_create();
        if (master_clock && sys_clock) {
            zst_clock_t* slave = zst_clock_slave_create(master_clock, sys_clock);
            if (slave) {
                zst_pipeline_set_clock(pipe, slave);
                zst_clock_unref(slave);
            } else {
                zst_pipeline_set_clock(pipe, sys_clock);
            }
        } else if (sys_clock) {
            zst_pipeline_set_clock(pipe, sys_clock);
        }

        if (master_clock) zst_clock_unref(master_clock);
        if (sys_clock) zst_clock_unref(sys_clock);
    }

    /* Propagate state to all elements */
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_result_t r = zst_element_set_state(pipe->elements[i], state);
        if (r != ZST_OK) {
            /* Error rollback: revert previous elements to old_state */
            for (uint32_t j = 0; j < i; j++) {
                zst_element_set_state(pipe->elements[j], old_state);
            }
            /* Post ZST_EVENT_ERROR */
            if (pipe->bus) {
                zst_event_t* ev = zst_event_new_error(pipe->elements[i], r, "Element failed to set state");
                zst_bus_post(pipe->bus, ev);
            }
            return r;
        }
    }

    pipe->state = state;
    if (pipe->bus) {
        zst_event_t* ev = zst_event_new_state_changed(NULL, old_state, state);
        zst_bus_post(pipe->bus, ev);
    }
    return ZST_OK;
}

zst_result_t
zst_pipeline_start(zst_pipeline_t* pipe)
{
    return zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
}

zst_result_t
zst_pipeline_stop(zst_pipeline_t* pipe)
{
    return zst_pipeline_set_state(pipe, ZST_STATE_NULL);
}

static void
dfs_sort(zst_element_t* el, zst_element_t** temp, uint32_t* temp_idx, int* visited, zst_pipeline_t* pipe)
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
        zst_pad_t* src_pad = el->src_pads[i];
        if (src_pad->peer && src_pad->peer->parent) {
            dfs_sort(src_pad->peer->parent, temp, temp_idx, visited, pipe);
        }
    }

    temp[--(*temp_idx)] = el;
}

void
zst_pipeline_topological_sort(zst_pipeline_t* pipe)
{
    if (!pipe || pipe->nb_elements <= 1) return;

    zst_element_t** temp = malloc(pipe->nb_elements * sizeof(zst_element_t*));
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

    memcpy(pipe->elements, temp, pipe->nb_elements * sizeof(zst_element_t*));

    free(visited);
    free(temp);
}
