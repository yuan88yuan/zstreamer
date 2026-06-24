/*=============================================================================
    zst_scheduler.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_SCHEDULER_SINGLE_THREAD,
    ZST_SCHEDULER_MULTI_THREAD
} zst_scheduler_mode_t;

typedef struct {

    zst_scheduler_mode_t mode;

    uint32_t worker_threads;

} zst_scheduler_config_t;

struct zst_scheduler {

    zst_scheduler_config_t config;

    zst_pipeline_t* pipeline;

    void* priv;
};

zst_scheduler_t* zst_scheduler_create(
    const zst_scheduler_config_t* cfg);

void zst_scheduler_destroy(
    zst_scheduler_t* sched);

zst_result_t zst_scheduler_attach(
    zst_scheduler_t* sched,
    zst_pipeline_t* pipe);

zst_result_t zst_scheduler_run(
    zst_scheduler_t* sched);

zst_result_t zst_scheduler_stop(
    zst_scheduler_t* sched);

zst_result_t zst_scheduler_wake(
    zst_scheduler_t* sched);

#ifdef __cplusplus
}
#endif