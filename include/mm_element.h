/*=============================================================================
    mm_element.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_STATE_NULL,
    MM_STATE_READY,
    MM_STATE_PAUSED,
    MM_STATE_PLAYING
} mm_state_t;

typedef struct {

    const char* name;

    mm_result_t (*open)(
        mm_element_t* el);

    mm_result_t (*close)(
        mm_element_t* el);

    mm_result_t (*start)(
        mm_element_t* el);

    mm_result_t (*stop)(
        mm_element_t* el);

    mm_result_t (*process)(
        mm_element_t* el,
        mm_buffer_t* in,
        mm_buffer_t** out);

    mm_caps_t* (*get_caps)(
        mm_element_t* el,
        mm_pad_t* pad,
        const mm_caps_t* filter);

} mm_element_ops_t;

struct mm_element {

    const mm_element_ops_t* ops;

    mm_state_t state;

    mm_pad_t** src_pads;
    uint32_t nb_src_pads;

    mm_pad_t** sink_pads;
    uint32_t nb_sink_pads;

    void* priv;
};

mm_element_t* mm_element_create(
    const mm_element_ops_t* ops,
    void* priv);

void mm_element_destroy(
    mm_element_t* el);

mm_result_t mm_element_set_state(
    mm_element_t* el,
    mm_state_t state);

mm_pad_t* mm_element_get_pad(
    mm_element_t* el,
    const char* name);

mm_result_t mm_element_add_pad(
    mm_element_t* el,
    mm_pad_t* pad);

#ifdef __cplusplus
}
#endif