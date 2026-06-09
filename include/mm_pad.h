/*=============================================================================
    mm_pad.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_buffer.h"
#include "mm_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_PAD_SRC,
    MM_PAD_SINK
} mm_pad_direction_t;

typedef mm_result_t (*mm_pad_push_fn)(
    mm_pad_t* pad,
    mm_buffer_t* buf);

typedef mm_result_t (*mm_pad_pull_fn)(
    mm_pad_t* pad,
    mm_buffer_t** out);

struct mm_pad {

    const char* name;

    mm_pad_direction_t direction;

    mm_element_t* parent;

    mm_caps_t* caps;          /* Negotiated caps */
    mm_caps_t* template_caps; /* Supported template caps */

    mm_pad_push_fn push;
    mm_pad_pull_fn pull;

    mm_pad_t* peer;

    void* priv;
};

mm_pad_t* mm_pad_create(
    const char* name,
    mm_pad_direction_t direction);

void mm_pad_destroy(
    mm_pad_t* pad);

mm_result_t mm_pad_link(
    mm_pad_t* src,
    mm_pad_t* sink);

void mm_pad_unlink(
    mm_pad_t* pad);

mm_result_t mm_pad_push(
    mm_pad_t* pad,
    mm_buffer_t* buf);

mm_result_t mm_pad_pull(
    mm_pad_t* pad,
    mm_buffer_t** out);

void mm_pad_reset_callbacks(
    mm_pad_t* pad);

mm_result_t mm_pad_set_caps(
    mm_pad_t* pad,
    const mm_caps_t* caps);

mm_caps_t* mm_pad_get_caps(
    mm_pad_t* pad);

mm_result_t mm_pad_set_template_caps(
    mm_pad_t* pad,
    const mm_caps_t* caps);

mm_result_t mm_pad_negotiate(
    mm_pad_t* src,
    mm_pad_t* sink);

#ifdef __cplusplus
}
#endif