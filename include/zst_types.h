/*=============================================================================
    zst_types.h
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  zst_result_t;
typedef uint64_t zst_time_t;

#define ZST_OK          0
#define ZST_ERROR      -1
#define ZST_TIMEOUT    -2
#define ZST_AGAIN      -3
#define ZST_EOF        -4

typedef struct zst_allocator   zst_allocator_t;
typedef struct zst_clock       zst_clock_t;
typedef struct zst_buffer      zst_buffer_t;
typedef struct zst_buffer_pool zst_buffer_pool_t;
typedef struct zst_queue       zst_queue_t;
typedef struct zst_pad         zst_pad_t;
typedef struct zst_element     zst_element_t;
typedef struct zst_pipeline    zst_pipeline_t;
typedef struct zst_scheduler   zst_scheduler_t;
typedef struct zst_plugin      zst_plugin_t;
typedef struct zst_bus         zst_bus_t;
typedef struct zst_event       zst_event_t;

#ifdef __cplusplus
}
#endif