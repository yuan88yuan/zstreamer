/*=============================================================================
    zst_pipeline.h
=============================================================================*/
#pragma once

#include <pthread.h>
#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zst_pipeline {

    zst_element_t** elements;

    uint32_t nb_elements;

    zst_state_t state;

    void* priv;

    zst_bus_t* bus;
    zst_clock_t* clock;

    /* When true (non-zero), the scheduler will wait until each buffer's PTS
     * is reached before delivering it, enabling real-time pacing and A/V sync.
     * Default: 0 (off) — buffers are delivered as fast as possible. */
    int clock_sync;

    pthread_rwlock_t elements_lock; /* Protects elements array */
};

zst_pipeline_t* zst_pipeline_create(void);

void zst_pipeline_destroy(
    zst_pipeline_t* pipe);

zst_bus_t* zst_pipeline_get_bus(
    zst_pipeline_t* pipe);

void zst_pipeline_set_clock(
    zst_pipeline_t* pipe,
    zst_clock_t* clock);

zst_clock_t* zst_pipeline_get_clock(
    zst_pipeline_t* pipe);

zst_result_t zst_pipeline_add(
    zst_pipeline_t* pipe,
    zst_element_t* el);

zst_result_t zst_pipeline_remove(
    zst_pipeline_t* pipe,
    zst_element_t* el);

zst_result_t zst_pipeline_set_state(
    zst_pipeline_t* pipe,
    zst_state_t state);

zst_result_t zst_pipeline_start(
    zst_pipeline_t* pipe);

zst_result_t zst_pipeline_stop(
    zst_pipeline_t* pipe);

zst_result_t zst_pipeline_set_clock_sync(
    zst_pipeline_t* pipe,
    int enabled);

int zst_pipeline_get_clock_sync(
    zst_pipeline_t* pipe);

void zst_pipeline_topological_sort(
    zst_pipeline_t* pipe);

int zst_pipeline_count_elements_of_type(
    zst_pipeline_t* pipe,
    const char* type_name);

void zst_pipeline_foreach_element(
    zst_pipeline_t* pipe,
    void (*func)(zst_element_t*, void*),
    void* user_data);

void zst_pipeline_update_buffer_pool_sizing(
    zst_pipeline_t* pipe);

#ifdef __cplusplus
}
#endif