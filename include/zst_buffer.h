/*=============================================================================
    zst_buffer.h
=============================================================================*/
#pragma once

#include "zst_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_BUFFER_VIDEO_FRAME,
    ZST_BUFFER_AUDIO_FRAME,
    ZST_BUFFER_VIDEO_PACKET,
    ZST_BUFFER_AUDIO_PACKET,
    ZST_BUFFER_USER = 0x10000
} zst_buffer_type_t;

typedef enum {
    ZST_MEMORY_CPU,
    ZST_MEMORY_DMABUF,
    ZST_MEMORY_CUDA,
    ZST_MEMORY_VULKAN
} zst_memory_type_t;

#define ZST_BUFFER_FLAG_EOS (1 << 0)

typedef struct {

    zst_memory_type_t type;

    void*  data;
    size_t size;

    void* priv;

    void (*release)(void* priv);

} zst_memory_t;

typedef struct {

    uint32_t width;
    uint32_t height;
    uint32_t format;

    uint8_t* plane[4];
    uint32_t stride[4];

} zst_video_frame_t;

typedef struct {

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;

    uint32_t nb_samples;

    void* data;

} zst_audio_frame_t;

struct zst_buffer {

    uint32_t type;

    volatile int refcount;

    zst_time_t pts;
    zst_time_t dts;
    zst_time_t duration;

    uint32_t flags;

    zst_memory_t memory;

    void* payload;
    void* metadata;

    struct zst_buffer_pool* pool;

    void (*destroy)(zst_buffer_t* buf);
};

zst_buffer_t* zst_buffer_create(uint32_t type);

zst_buffer_t* zst_buffer_create_with_allocator(
    uint32_t type,
    zst_allocator_t* allocator,
    size_t size);

zst_buffer_t* zst_buffer_create_with_pool(
    struct zst_buffer_pool* pool);

zst_buffer_t* zst_buffer_ref(
    zst_buffer_t* buf);

void zst_buffer_unref(
    zst_buffer_t* buf);

#ifdef __cplusplus
}
#endif