/*=============================================================================
    zst_buffer_pool.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_POOL_WATERMARK_LOW,
    ZST_POOL_WATERMARK_HIGH
} zst_pool_watermark_level_t;

typedef void (*zst_buffer_pool_watermark_cb)(
    zst_buffer_pool_t* pool,
    zst_pool_watermark_level_t level,
    void* user_data);

typedef struct {
    uint32_t min_buffers;
    uint32_t max_buffers;
    size_t   buffer_size;
    uint32_t buffer_type;

    uint32_t low_watermark;
    uint32_t high_watermark;
    zst_buffer_pool_watermark_cb watermark_cb;
    void*    watermark_user_data;
} zst_buffer_pool_config_t;

#define ZST_POOL_ACQUIRE_NONBLOCK (1 << 0)

zst_buffer_pool_t* zst_buffer_pool_create(
    zst_allocator_t* allocator,
    zst_buffer_pool_config_t* config);

zst_result_t zst_buffer_pool_acquire(
    zst_buffer_pool_t* pool,
    zst_buffer_t** out_buf,
    int timeout_ms,
    uint32_t flags);

void zst_buffer_pool_release(
    zst_buffer_pool_t* pool,
    zst_buffer_t* buf);

void zst_buffer_pool_destroy(
    zst_buffer_pool_t* pool);

zst_buffer_pool_config_t zst_buffer_pool_get_config(zst_buffer_pool_t* pool);

zst_buffer_pool_config_t zst_buffer_pool_config_from_caps(const zst_caps_t* caps);

void zst_buffer_pool_prefill(zst_buffer_pool_t* pool);

void zst_buffer_pool_drain(zst_buffer_pool_t* pool);

#ifdef __cplusplus
}
#endif
