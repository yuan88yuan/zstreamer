/*=============================================================================
    zst_fake_sink.h — Fake sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_FAKE_SINK_FACTORY                    "fakesink"
#define ZST_FAKE_SINK_PROP_DROP_PROBABILITY      "drop-probability"
#define ZST_FAKE_SINK_PROP_TOTAL_BUFFERS         "total-buffers"
#define ZST_FAKE_SINK_PROP_TOTAL_BYTES           "total-bytes"

typedef struct {
    size_t struct_size;
    double drop_probability;
} zst_fake_sink_config_t;

zst_element_t* zst_fake_sink_create(void);
zst_element_t* zst_fake_sink_create_with_config(const zst_fake_sink_config_t* config);

#ifdef __cplusplus
}
#endif
