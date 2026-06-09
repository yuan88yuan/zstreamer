/*=============================================================================
    mm_types.h
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  mm_result_t;
typedef uint64_t mm_time_t;

#define MM_OK          0
#define MM_ERROR      -1
#define MM_TIMEOUT    -2
#define MM_AGAIN      -3
#define MM_EOF        -4

typedef struct mm_buffer      mm_buffer_t;
typedef struct mm_queue       mm_queue_t;
typedef struct mm_pad         mm_pad_t;
typedef struct mm_element     mm_element_t;
typedef struct mm_pipeline    mm_pipeline_t;
typedef struct mm_scheduler   mm_scheduler_t;
typedef struct mm_plugin      mm_plugin_t;
typedef struct mm_bus         mm_bus_t;
typedef struct mm_event       mm_event_t;

#ifdef __cplusplus
}
#endif