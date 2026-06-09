/*=============================================================================
    zst_element.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_STATE_NULL,
    ZST_STATE_READY,
    ZST_STATE_PAUSED,
    ZST_STATE_PLAYING
} zst_state_t;

typedef struct {

    const char* name;

    zst_result_t (*open)(
        zst_element_t* el);

    zst_result_t (*close)(
        zst_element_t* el);

    zst_result_t (*start)(
        zst_element_t* el);

    zst_result_t (*stop)(
        zst_element_t* el);

    zst_result_t (*process)(
        zst_element_t* el,
        zst_buffer_t* in,
        zst_buffer_t** out);

    zst_caps_t* (*get_caps)(
        zst_element_t* el,
        zst_pad_t* pad,
        const zst_caps_t* filter);

} zst_element_ops_t;

struct zst_element {

    const zst_element_ops_t* ops;

    zst_state_t state;

    zst_pad_t** src_pads;
    uint32_t nb_src_pads;

    zst_pad_t** sink_pads;
    uint32_t nb_sink_pads;

    void* priv;

    zst_bus_t* bus;

    zst_plugin_t* plugin;
};

zst_element_t* zst_element_create(
    const zst_element_ops_t* ops,
    void* priv);

void zst_element_destroy(
    zst_element_t* el);

zst_result_t zst_element_set_state(
    zst_element_t* el,
    zst_state_t state);

zst_pad_t* zst_element_get_pad(
    zst_element_t* el,
    const char* name);

zst_result_t zst_element_add_pad(
    zst_element_t* el,
    zst_pad_t* pad);

#ifdef __cplusplus
}
#endif