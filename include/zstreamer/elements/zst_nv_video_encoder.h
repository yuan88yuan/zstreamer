/*=============================================================================
    zst_nv_video_encoder.h — NV V4L2 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NVENC_FACTORY "nvenc"

#define ZST_NVENC_PROP_CODEC      "codec"
#define ZST_NVENC_PROP_PRESET     "preset"
#define ZST_NVENC_PROP_BITRATE    "bitrate"
#define ZST_NVENC_PROP_GOP_SIZE   "gop-size"
#define ZST_NVENC_PROP_PROFILE    "profile"

zst_element_t* zst_nv_video_encoder_create(void);

#ifdef __cplusplus
}
#endif
