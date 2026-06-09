/*=============================================================================
    mm_buffer.h
=============================================================================*/
#pragma once

#include "mm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_BUFFER_VIDEO_FRAME,
    MM_BUFFER_AUDIO_FRAME,
    MM_BUFFER_VIDEO_PACKET,
    MM_BUFFER_AUDIO_PACKET,
    MM_BUFFER_USER = 0x10000
} mm_buffer_type_t;

typedef enum {
    MM_MEMORY_CPU,
    MM_MEMORY_DMABUF,
    MM_MEMORY_CUDA,
    MM_MEMORY_VULKAN
} mm_memory_type_t;

typedef struct {

    mm_memory_type_t type;

    void*  data;
    size_t size;

    void* priv;

    void (*release)(void* priv);

} mm_memory_t;

typedef struct {

    uint32_t width;
    uint32_t height;
    uint32_t format;

    uint8_t* plane[4];
    uint32_t stride[4];

} mm_video_frame_t;

typedef struct {

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;

    uint32_t nb_samples;

    void* data;

} mm_audio_frame_t;

struct mm_buffer {

    uint32_t type;

    volatile int refcount;

    mm_time_t pts;
    mm_time_t dts;
    mm_time_t duration;

    uint32_t flags;

    mm_memory_t memory;

    void* payload;
    void* metadata;

    void (*destroy)(mm_buffer_t* buf);
};

mm_buffer_t* mm_buffer_create(uint32_t type);

mm_buffer_t* mm_buffer_ref(
    mm_buffer_t* buf);

void mm_buffer_unref(
    mm_buffer_t* buf);

#ifdef __cplusplus
}
#endif