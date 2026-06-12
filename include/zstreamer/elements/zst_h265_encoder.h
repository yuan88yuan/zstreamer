/*=============================================================================
    zst_h265_encoder.h — H265 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H265_ENCODER_FACTORY "h265enc"

#define ZST_H265_ENCODER_PROP_PRESET    "preset"
#define ZST_H265_ENCODER_PROP_TUNE      "tune"
#define ZST_H265_ENCODER_PROP_CRF       "crf"
#define ZST_H265_ENCODER_PROP_BITRATE   "bitrate"
#define ZST_H265_ENCODER_PROP_GOP_SIZE  "gop-size"
#define ZST_H265_ENCODER_PROP_KEYINT_MIN "keyint-min"
#define ZST_H265_ENCODER_PROP_PROFILE   "profile"
#define ZST_H265_ENCODER_PROP_LEVEL     "level"

zst_element_t* zst_h265_encoder_create(void);

#ifdef __cplusplus
}
#endif
