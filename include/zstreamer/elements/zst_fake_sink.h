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

zst_element_t* zst_fake_sink_create(void);

#ifdef __cplusplus
}
#endif
