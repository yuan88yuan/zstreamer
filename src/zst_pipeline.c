/*=============================================================================
    zst_pipeline.c - Incremental rank-based topological sorting
=============================================================================*/
#include "zst_pipeline.h"
#include "zst_log.h"
#include "zst_bus.h"
#include "zst_element_factory.h"
#include <stdlib.h>
#include <string.h>

zst_pipeline_t* zst_pipeline_create(void) {
    zst_pipeline_t* pipe = (zst_pipeline_t*)malloc(sizeof(zst_pipeline_t));
    if (!pipe) return NULL;

    memset(pipe, 0, sizeof(zst_pipeline_t));
    pipe->capacity = 16;
    pipe->elements = (zst_element_t**)malloc(sizeof(zst_element_t*) * pipe->capacity);
    atomic_init(&pipe->state, ZST_STATE_NULL);

    pthread_rwlock_init(&pipe->elements_lock, NULL);

    return pipe;
}

void zst_pipeline_destroy(zst_pipeline_t* pipe) {
    if (!pipe) return;

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);

    pthread_rwlock_wrlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_destroy(pipe->elements[i]);
    }
    free(pipe->elements);
    pthread_rwlock_unlock(&pipe->elements_lock);

    pthread_rwlock_destroy(&pipe->elements_lock);
    free(pipe);
}

zst_result_t zst_pipeline_add(zst_pipeline_t* pipe, zst_element_t* el) {
    if (!pipe || !el) return ZST_ERROR;

    pthread_rwlock_wrlock(&pipe->elements_lock);

    if (pipe->nb_elements >= pipe->capacity) {
        pipe->capacity *= 2;
        pipe->elements = (zst_element_t**)realloc(pipe->elements, sizeof(zst_element_t*) * pipe->capacity);
    }

    pipe->elements[pipe->nb_elements++] = el;
    el->pipeline = pipe;

    /* Re-evaluate topological sorting incrementally starting from the new element */
    zst_pipeline_update_ranks_from(pipe, el);

    pthread_rwlock_unlock(&pipe->elements_lock);

    return ZST_OK;
}

zst_result_t zst_pipeline_remove(zst_pipeline_t* pipe, zst_element_t* el) {
    if (!pipe || !el) return ZST_ERROR;

    pthread_rwlock_wrlock(&pipe->elements_lock);

    int index = -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            index = i;
            break;
        }
    }

    if (index == -1) {
        pthread_rwlock_unlock(&pipe->elements_lock);
        return ZST_ERROR;
    }

    /* Shift remaining elements */
    for (uint32_t i = index; i < pipe->nb_elements - 1; i++) {
        pipe->elements[i] = pipe->elements[i + 1];
    }
    pipe->nb_elements--;
    el->pipeline = NULL;

    /* Recalculate rank structures */
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_pipeline_update_ranks_from(pipe, pipe->elements[i]);
    }

    pthread_rwlock_unlock(&pipe->elements_lock);
    return ZST_OK;
}

/* Localized rank recurrence to update dependency hierarchy without a global sorting sweep */
void zst_pipeline_update_ranks_from(zst_pipeline_t* pipe, zst_element_t* start_el) {
    if (!start_el) return;

    for (uint32_t i = 0; i < start_el->nb_src_pads; i++) {
        zst_pad_t* src_pad = start_el->src_pads[i];
        if (src_pad->peer && src_pad->peer->parent) {
            zst_element_t* child = src_pad->peer->parent;
            if (child->graph_rank <= start_el->graph_rank) {
                child->graph_rank = start_el->graph_rank + 1;
                /* Propagate rankings recursively through the downstream dependency path */
                zst_pipeline_update_ranks_from(pipe, child);
            }
        }
    }
}

/* Quick topological sorting helper using ranks */
static int compare_elements_by_rank(const void* a, const void* b) {
    const zst_element_t* el_a = *(const zst_element_t**)a;
    const zst_element_t* el_b = *(const zst_element_t**)b;
    return (int)el_a->graph_rank - (int)el_b->graph_rank;
}

void zst_pipeline_topological_sort(zst_pipeline_t* pipe) {
    if (!pipe) return;
    pthread_rwlock_wrlock(&pipe->elements_lock);
    
    /* Sorting takes O(N log N) using local pre-calculated ranks instead of full DFS coloring */
    qsort(pipe->elements, pipe->nb_elements, sizeof(zst_element_t*), compare_elements_by_rank);
    
    pthread_rwlock_unlock(&pipe->elements_lock);
}

zst_result_t zst_pipeline_set_state(zst_pipeline_t* pipe, zst_state_t state) {
    if (!pipe) return ZST_ERROR;

    pthread_rwlock_wrlock(&pipe->elements_lock);
    zst_result_t result = ZST_OK;

    /* Sort the graph to transition upstream-to-downstream or downstream-to-upstream safely */
    qsort(pipe->elements, pipe->nb_elements, sizeof(zst_element_t*), compare_elements_by_rank);

    if (state >= atomic_load_explicit(&pipe->state, memory_order_relaxed)) {
        /* Forward state transition: Sort and open from source (upstream) to sink (downstream) */
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            result = zst_element_set_state(pipe->elements[i], state);
            if (result != ZST_OK) break;
        }
    } else {
        /* Teardown state transition: reverse order from sink back to source */
        for (int32_t i = (int32_t)pipe->nb_elements - 1; i >= 0; i--) {
            result = zst_element_set_state(pipe->elements[i], state);
            if (result != ZST_OK) break;
        }
    }

    if (result == ZST_OK) {
        atomic_store_explicit(&pipe->state, state, memory_order_release);
    }

    pthread_rwlock_unlock(&pipe->elements_lock);
    return result;
}

zst_bus_t* zst_pipeline_get_bus(zst_pipeline_t* pipe) {
    return pipe ? pipe->bus : NULL;
}

void zst_pipeline_set_clock(zst_pipeline_t* pipe, zst_clock_t* clock) {
    if (!pipe) return;
    pthread_rwlock_wrlock(&pipe->elements_lock);
    pipe->clock = clock;
    pthread_rwlock_unlock(&pipe->elements_lock);
}

zst_clock_t* zst_pipeline_get_clock(zst_pipeline_t* pipe) {
    return pipe ? pipe->clock : NULL;
}

zst_result_t zst_pipeline_start(zst_pipeline_t* pipe) {
    return zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
}

zst_result_t zst_pipeline_stop(zst_pipeline_t* pipe) {
    return zst_pipeline_set_state(pipe, ZST_STATE_READY);
}

zst_result_t zst_pipeline_set_clock_sync(zst_pipeline_t* pipe, int enabled) {
    if (!pipe) return ZST_ERROR;
    pipe->clock_sync = enabled;
    return ZST_OK;
}

int zst_pipeline_get_clock_sync(zst_pipeline_t* pipe) {
    return pipe ? pipe->clock_sync : 0;
}

int zst_pipeline_count_elements_of_type(zst_pipeline_t* pipe, const char* type_name) {
    if (!pipe || !type_name) return 0;
    int count = 0;
    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_t* el = pipe->elements[i];
        if (el->desc && el->desc->name && strcmp(el->desc->name, type_name) == 0) {
            count++;
        }
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
    return count;
}

void zst_pipeline_foreach_element(
    zst_pipeline_t* pipe,
    void (*func)(zst_element_t*, void*),
    void* user_data)
{
    if (!pipe || !func) return;
    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        func(pipe->elements[i], user_data);
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
}

void zst_pipeline_update_buffer_pool_sizing(zst_pipeline_t* pipe) {
    /* Safe placeholder to align with standard buffer pool updates */
    (void)pipe;
}