/*=============================================================================
    mm_caps.h — Media capabilities structure and negotiation API
=============================================================================*/
#pragma once

#include "mm_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_CAPS_VIDEO,
    MM_CAPS_AUDIO,
    MM_CAPS_ANY
} mm_caps_type_t;

typedef struct mm_caps_struct {
    char media_type[64];
    mm_caps_type_t type;

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

    struct mm_caps_struct* next;
} mm_caps_struct_t;

typedef struct {
    mm_caps_struct_t* structs;
} mm_caps_t;

/* Lifecycle & builders */
mm_caps_t* mm_caps_create(void);
void mm_caps_destroy(mm_caps_t* caps);
mm_caps_t* mm_caps_copy(const mm_caps_t* caps);

mm_caps_struct_t* mm_caps_struct_create_video(
    const char* media_type,
    int width,
    int height,
    double framerate,
    const char* pixel_format);

mm_caps_struct_t* mm_caps_struct_create_audio(
    const char* media_type,
    int channels,
    int sample_rate,
    const char* format);

void mm_caps_struct_free(mm_caps_struct_t* caps_struct);
mm_caps_struct_t* mm_caps_struct_copy(const mm_caps_struct_t* caps_struct);

/* Manipulation */
mm_result_t mm_caps_append(mm_caps_t* caps, mm_caps_struct_t* caps_struct);

/* Negotiation logic */
mm_caps_t* mm_caps_intersect(const mm_caps_t* caps1, const mm_caps_t* caps2);
int mm_caps_struct_is_fixed(const mm_caps_struct_t* s);
int mm_caps_is_fixed(const mm_caps_t* caps);
mm_result_t mm_caps_fixate(mm_caps_t* caps);

#ifdef __cplusplus
}
#endif
