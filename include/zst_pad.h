/*=============================================================================
    zst_pad.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_PAD_SRC,
    ZST_PAD_SINK
} zst_pad_direction_t;

typedef zst_result_t (*zst_pad_push_fn)(
    zst_pad_t* pad,
    zst_buffer_t* buf);

typedef zst_result_t (*zst_pad_pull_fn)(
    zst_pad_t* pad,
    zst_buffer_t** out);

struct zst_pad {

    const char* name;

    zst_pad_direction_t direction;

    zst_element_t* parent;

    zst_caps_t* caps;          /* Negotiated caps */
    zst_caps_t* template_caps; /* Supported template caps */

    zst_pad_push_fn push;
    zst_pad_pull_fn pull;

    zst_pad_t* peer;

    void* priv;
};

zst_pad_t* zst_pad_create(
    const char* name,
    zst_pad_direction_t direction);

void zst_pad_destroy(
    zst_pad_t* pad);

zst_result_t zst_pad_link(
    zst_pad_t* src,
    zst_pad_t* sink);

void zst_pad_unlink(
    zst_pad_t* pad);

zst_result_t zst_pad_push(
    zst_pad_t* pad,
    zst_buffer_t* buf);

zst_result_t zst_pad_pull(
    zst_pad_t* pad,
    zst_buffer_t** out);

void zst_pad_reset_callbacks(
    zst_pad_t* pad);

zst_result_t zst_pad_set_caps(
    zst_pad_t* pad,
    const zst_caps_t* caps);

zst_caps_t* zst_pad_get_caps(
    zst_pad_t* pad);

zst_result_t zst_pad_set_template_caps(
    zst_pad_t* pad,
    const zst_caps_t* caps);

zst_result_t zst_pad_negotiate(
    zst_pad_t* src,
    zst_pad_t* sink);

#ifdef __cplusplus
}
#endif