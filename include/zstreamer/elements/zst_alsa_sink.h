/*=============================================================================
    zst_alsa_sink.h — Alsa Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ALSA_SINK_FACTORY "alsasink"

#define ZST_ALSA_SINK_PROP_DEVICE      "device"
#define ZST_ALSA_SINK_PROP_SAMPLE_RATE "sample-rate"
#define ZST_ALSA_SINK_PROP_CHANNELS    "channels"

zst_element_t* zst_alsa_sink_create(void);

#ifdef __cplusplus
}
#endif
