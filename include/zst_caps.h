/*=============================================================================
    zst_caps.h — Media capabilities structure and negotiation API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_CAPS_VIDEO,
    ZST_CAPS_AUDIO,
    ZST_CAPS_ANY
} zst_caps_type_t;

typedef struct zst_caps_struct {
    char media_type[64];
    zst_caps_type_t type;

    // Video properties
    struct {
        int width;             /* 0 means wildcard/any */
        int height;            /* 0 means wildcard/any */
        double framerate;      /* 0.0 means wildcard/any */
        char pixel_format[32]; /* empty string "" means wildcard/any */
    } video;

    // Audio properties
    struct {
        int channels;          /* 0 means wildcard/any */
        int sample_rate;       /* 0 means wildcard/any */
        char format[32];       /* empty string "" means wildcard/any */
    } audio;

    struct zst_caps_struct* next;
} zst_caps_struct_t;

struct zst_caps {
    zst_caps_struct_t* structs;
};

typedef struct zst_caps zst_caps_t;

/* Lifecycle & builders */
zst_caps_t* zst_caps_create(void);
void zst_caps_destroy(zst_caps_t* caps);
zst_caps_t* zst_caps_copy(const zst_caps_t* caps);

zst_caps_struct_t* zst_caps_struct_create_video(
    const char* media_type,
    int width,
    int height,
    double framerate,
    const char* pixel_format);

zst_caps_struct_t* zst_caps_struct_create_audio(
    const char* media_type,
    int channels,
    int sample_rate,
    const char* format);

void zst_caps_struct_free(zst_caps_struct_t* caps_struct);
zst_caps_struct_t* zst_caps_struct_copy(const zst_caps_struct_t* caps_struct);

/* Manipulation */
zst_result_t zst_caps_append(zst_caps_t* caps, zst_caps_struct_t* caps_struct);

/* Negotiation logic */
zst_caps_t* zst_caps_intersect(const zst_caps_t* caps1, const zst_caps_t* caps2);
int zst_caps_struct_is_fixed(const zst_caps_struct_t* s);
int zst_caps_is_fixed(const zst_caps_t* caps);
zst_result_t zst_caps_fixate(zst_caps_t* caps);

#ifdef __cplusplus
}
#endif
