/*=============================================================================
    zst_queue.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_QUEUE_SYNC,
    ZST_QUEUE_ASYNC
} zst_queue_mode_t;

typedef struct {

    zst_queue_mode_t mode;

    uint32_t max_buffers;
    uint64_t max_bytes;

    zst_time_t max_duration;

} zst_queue_config_t;

zst_queue_t* zst_queue_create(
    const zst_queue_config_t* cfg);

void zst_queue_destroy(
    zst_queue_t* q);

zst_result_t zst_queue_push(
    zst_queue_t* q,
    zst_buffer_t* buf,
    uint32_t timeout_ms);

zst_result_t zst_queue_pop(
    zst_queue_t* q,
    zst_buffer_t** out,
    uint32_t timeout_ms);

uint32_t zst_queue_size(
    zst_queue_t* q);

void zst_queue_flush(
    zst_queue_t* q);

zst_element_t* zst_queue_element_create(
    const zst_queue_config_t* cfg);

#ifdef __cplusplus
}
#endif