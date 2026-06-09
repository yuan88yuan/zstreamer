/*=============================================================================
    mm_queue.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_QUEUE_SYNC,
    MM_QUEUE_ASYNC
} mm_queue_mode_t;

typedef struct {

    mm_queue_mode_t mode;

    uint32_t max_buffers;
    uint64_t max_bytes;

    mm_time_t max_duration;

} mm_queue_config_t;

mm_queue_t* mm_queue_create(
    const mm_queue_config_t* cfg);

void mm_queue_destroy(
    mm_queue_t* q);

mm_result_t mm_queue_push(
    mm_queue_t* q,
    mm_buffer_t* buf,
    uint32_t timeout_ms);

mm_result_t mm_queue_pop(
    mm_queue_t* q,
    mm_buffer_t** out,
    uint32_t timeout_ms);

uint32_t mm_queue_size(
    mm_queue_t* q);

void mm_queue_flush(
    mm_queue_t* q);

#ifdef __cplusplus
}
#endif