/*=============================================================================
    zst_pipeline.c — Element container with state propagation
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* clock_gettime */

#include "zst_pipeline.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_buffer_pool.h"
#include "zst_element_factory.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    pthread_rwlock_init(&pipe->elements_lock, NULL);

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

    pthread_rwlock_destroy(&pipe->elements_lock);

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
    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_set_clock(pipe->elements[i], pipe->clock);
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
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

    pthread_rwlock_wrlock(&pipe->elements_lock);
    zst_element_t** els = realloc(pipe->elements,
                                 (pipe->nb_elements + 1) * sizeof(zst_element_t*));
    if (!els) {
        pthread_rwlock_unlock(&pipe->elements_lock);
        return ZST_ERROR;
    }

    els[pipe->nb_elements++] = el;
    pipe->elements = els;
    el->bus = pipe->bus;
    el->pipeline = pipe;
    pthread_rwlock_unlock(&pipe->elements_lock);

    zst_element_set_clock(el, pipe->clock);
    return ZST_OK;
}

zst_result_t
zst_pipeline_remove(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    pthread_rwlock_wrlock(&pipe->elements_lock);
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
    pthread_rwlock_unlock(&pipe->elements_lock);

    if (found) {
        el->bus = NULL;
        el->pipeline = NULL;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static int
pipeline_element_index(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) return (int)i;
    }
    return -1;
}

static const char*
pipeline_element_type_name(zst_element_t* el)
{
    if (!el) return NULL;
    if (el->desc && el->desc->name) return el->desc->name;
    if (el->ops && el->ops->name) return el->ops->name;
    return NULL;
}

static int
pipeline_element_is_type(zst_element_t* el, const char* type_name)
{
    const char* name = pipeline_element_type_name(el);
    return name && type_name && strcmp(name, type_name) == 0;
}

static int
pipeline_count_downstream_type_dfs(zst_pipeline_t* pipe, zst_element_t* el,
                                   const char* type_name, int* visited)
{
    int count = 0;
    if (!pipe || !el || !type_name || !visited) return 0;

    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        zst_pad_t* src_pad = el->src_pads[i];
        zst_element_t* child = (src_pad && src_pad->peer) ? src_pad->peer->parent : NULL;
        int idx = pipeline_element_index(pipe, child);
        if (idx < 0 || visited[idx]) continue;
        visited[idx] = 1;

        if (pipeline_element_is_type(child, type_name)) count++;
        count += pipeline_count_downstream_type_dfs(pipe, child, type_name, visited);
    }

    return count;
}

static int
zst_pipeline_count_downstream_elements_of_type(zst_pipeline_t* pipe,
                                               zst_element_t* start,
                                               const char* type_name)
{
    if (!pipe || !start || !type_name) return 0;
    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) return 0;
    int start_idx = pipeline_element_index(pipe, start);
    if (start_idx >= 0) visited[start_idx] = 1;
    int count = pipeline_count_downstream_type_dfs(pipe, start, type_name, visited);
    free(visited);
    return count;
}

static void
pool_config_default_size_for_queue_count(zst_buffer_pool_config_t* config, int n_queues)
{
    if (!config || n_queues <= 0) return;
    if (config->min_buffers < (uint32_t)(n_queues + 2)) {
        config->min_buffers = n_queues + 2;
        if (config->max_buffers < config->min_buffers) {
            config->max_buffers = config->min_buffers * 2;
        }
    }
}

static void apply_pool_config_cb(zst_element_t* el, void* user_data)
{
    zst_pipeline_t* pipe = user_data;
    zst_buffer_pool_t* pool = zst_element_get_pool(el);
    if (pool) {
        zst_buffer_pool_config_t old_config = zst_buffer_pool_get_config(pool);
        zst_buffer_pool_config_t new_config = old_config;
        int n_queues = zst_pipeline_count_downstream_elements_of_type(pipe, el, "queue");
        pool_config_default_size_for_queue_count(&new_config, n_queues);
        if (new_config.min_buffers != old_config.min_buffers ||
            new_config.max_buffers != old_config.max_buffers) {
            zst_buffer_pool_set_config(pool, &new_config);
        }
    }
}

void
zst_pipeline_update_buffer_pool_sizing(zst_pipeline_t* pipe)
{
    zst_pipeline_foreach_element(pipe, apply_pool_config_cb, pipe);
}

zst_result_t
zst_pipeline_set_state(zst_pipeline_t* pipe, zst_state_t state)
{
    if (!pipe) return ZST_ERROR;

    zst_state_t old_state = pipe->state;
    if (old_state == state) return ZST_OK;

    /* A direct NULL -> PLAYING request needs an intermediate READY pass so
     * elements that create pools in open() expose them before topology sizing
     * and before start() begins worker threads. */
    if (old_state < ZST_STATE_READY && state == ZST_STATE_PLAYING) {
        zst_result_t r = zst_pipeline_set_state(pipe, ZST_STATE_READY);
        if (r != ZST_OK) return r;
        r = zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
        if (r != ZST_OK) {
            zst_pipeline_set_state(pipe, old_state);
        }
        return r;
    }

    if (old_state < ZST_STATE_PLAYING && state == ZST_STATE_PLAYING) {
        zst_pipeline_update_buffer_pool_sizing(pipe);
    }

    /* Transition to PLAYING: Auto-select clock if none exists */
    if (old_state < ZST_STATE_PLAYING && state == ZST_STATE_PLAYING && !pipe->clock) {
        zst_clock_t* master_clock = NULL;
        pthread_rwlock_rdlock(&pipe->elements_lock);
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            zst_element_t* el = pipe->elements[i];
            if (el->ops && el->ops->provide_clock) {
                master_clock = el->ops->provide_clock(el);
                if (master_clock) break;
            }
        }
        pthread_rwlock_unlock(&pipe->elements_lock);

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

        /* Record pipeline base_time: wall-clock snapshot at PLAYING entry.
         * The scheduler uses (now - base_time) as pipeline running-time and
         * compares it against each buffer's PTS for real-time pacing. */
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        pipe->base_time = (zst_time_t)_ts.tv_sec * 1000000000ULL
                        + (zst_time_t)_ts.tv_nsec;
    }

    /* Propagate state to all elements */
    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_result_t r = zst_element_set_state(pipe->elements[i], state);
        if (r != ZST_OK) {
            /* Error rollback: revert previous elements to old_state */
            for (uint32_t j = 0; j < i; j++) {
                zst_element_set_state(pipe->elements[j], old_state);
            }
            pthread_rwlock_unlock(&pipe->elements_lock);
            /* Post ZST_EVENT_ERROR */
            if (pipe->bus) {
                zst_event_t* ev = zst_event_new_error(pipe->elements[i], r, "Element failed to set state");
                zst_bus_post(pipe->bus, ev);
            }
            return r;
        }
    }
    pthread_rwlock_unlock(&pipe->elements_lock);

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

zst_result_t
zst_pipeline_set_clock_sync(zst_pipeline_t* pipe, int enabled)
{
    if (!pipe) return ZST_ERROR;
    pipe->clock_sync = enabled ? 1 : 0;
    return ZST_OK;
}

int
zst_pipeline_get_clock_sync(zst_pipeline_t* pipe)
{
    return pipe ? pipe->clock_sync : 0;
}

void
zst_pipeline_topological_sort(zst_pipeline_t* pipe)
{
    if (!pipe || pipe->nb_elements <= 1) return;

    pthread_rwlock_wrlock(&pipe->elements_lock);
    zst_element_t** temp = malloc(pipe->nb_elements * sizeof(zst_element_t*));
    if (!temp) {
        pthread_rwlock_unlock(&pipe->elements_lock);
        return;
    }

    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) {
        free(temp);
        pthread_rwlock_unlock(&pipe->elements_lock);
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
    pthread_rwlock_unlock(&pipe->elements_lock);
}

int
zst_pipeline_count_elements_of_type(zst_pipeline_t* pipe, const char* type_name)
{
    if (!pipe || !type_name) return 0;

    pthread_rwlock_rdlock(&pipe->elements_lock);
    int count = 0;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_t* el = pipe->elements[i];
        if (pipeline_element_is_type(el, type_name)) {
            count++;
        }
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
    return count;
}

void
zst_pipeline_foreach_element(zst_pipeline_t* pipe, void (*func)(zst_element_t*, void*), void* user_data)
{
    if (!pipe || !func) return;

    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        func(pipe->elements[i], user_data);
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
}
