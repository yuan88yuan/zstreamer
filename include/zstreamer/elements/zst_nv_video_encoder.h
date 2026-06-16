/*=============================================================================
    zst_nv_video_encoder.h — NV V4L2 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NVENV_FACTORY "nvenv"

#define ZST_NVENV_PROP_CODEC      "codec"
#define ZST_NVENV_PROP_PRESET     "preset"
#define ZST_NVENV_PROP_BITRATE    "bitrate"
#define ZST_NVENV_PROP_GOP_SIZE   "gop-size"
#define ZST_NVENV_PROP_PROFILE    "profile"

zst_element_t* zst_nv_video_encoder_create(void);

#ifdef __cplusplus
}
#endif
