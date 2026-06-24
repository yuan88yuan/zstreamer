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

typedef enum {
    ZST_CAPS_FIELD_INT,
    ZST_CAPS_FIELD_UINT,
    ZST_CAPS_FIELD_DOUBLE,
    ZST_CAPS_FIELD_STRING,
    ZST_CAPS_FIELD_FRACTION,
    ZST_CAPS_FIELD_BUFFER
} zst_caps_field_type_t;

typedef struct {
    char key[64];
    zst_caps_field_type_t type;
    union {
        int i_val;
        uint32_t u_val;
        double d_val;
        char* s_val;
        struct {
            int num;
            int denom;
        } f_val;
        struct {
            void* data;
            size_t size;
        } b_val;
    } value;
} zst_caps_field_t;

typedef struct zst_caps_struct {
    char media_type[64];
    zst_caps_type_t type;

    /* Generic key/value fields */
    zst_caps_field_t* fields;
    uint32_t nb_fields;
    uint32_t fields_capacity;

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
zst_caps_t* zst_caps_new_simple(const char* media_type);

zst_result_t zst_caps_set_int(zst_caps_t* caps, const char* key, int value);
zst_result_t zst_caps_set_uint(zst_caps_t* caps, const char* key, uint32_t value);
zst_result_t zst_caps_set_double(zst_caps_t* caps, const char* key, double value);
zst_result_t zst_caps_set_string(zst_caps_t* caps, const char* key, const char* value);
zst_result_t zst_caps_set_fraction(zst_caps_t* caps, const char* key, int num, int denom);
zst_result_t zst_caps_set_buffer(zst_caps_t* caps, const char* key, const void* data, size_t size);

zst_result_t zst_caps_get_int(const zst_caps_t* caps, const char* key, int* value_out);
zst_result_t zst_caps_get_uint(const zst_caps_t* caps, const char* key, uint32_t* value_out);
zst_result_t zst_caps_get_double(const zst_caps_t* caps, const char* key, double* value_out);
zst_result_t zst_caps_get_string(const zst_caps_t* caps, const char* key, const char** value_out);
zst_result_t zst_caps_get_fraction(const zst_caps_t* caps, const char* key, int* num_out, int* denom_out);
zst_result_t zst_caps_get_buffer(const zst_caps_t* caps, const char* key, const void** data_out, size_t* size_out);

#ifdef __cplusplus
}
#endif
