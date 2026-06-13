/*=============================================================================
    zst_aac_encoder.h — Aac Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_ENCODER_FACTORY "aacenc"

#define ZST_AAC_ENCODER_PROP_BITRATE     "bitrate"
#define ZST_AAC_ENCODER_PROP_SAMPLE_RATE "sample-rate"
#define ZST_AAC_ENCODER_PROP_CHANNELS    "channels"

zst_element_t* zst_aac_encoder_create(void);

#ifdef __cplusplus
}
#endif
