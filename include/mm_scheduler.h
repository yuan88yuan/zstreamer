/*=============================================================================
    mm_scheduler.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_SCHEDULER_SINGLE_THREAD,
    MM_SCHEDULER_MULTI_THREAD
} mm_scheduler_mode_t;

typedef struct {

    mm_scheduler_mode_t mode;

    uint32_t worker_threads;

} mm_scheduler_config_t;

struct mm_scheduler {

    mm_scheduler_config_t config;

    mm_pipeline_t* pipeline;

    void* priv;
};

mm_scheduler_t* mm_scheduler_create(
    const mm_scheduler_config_t* cfg);

void mm_scheduler_destroy(
    mm_scheduler_t* sched);

mm_result_t mm_scheduler_attach(
    mm_scheduler_t* sched,
    mm_pipeline_t* pipe);

mm_result_t mm_scheduler_run(
    mm_scheduler_t* sched);

mm_result_t mm_scheduler_stop(
    mm_scheduler_t* sched);

#ifdef __cplusplus
}
#endif