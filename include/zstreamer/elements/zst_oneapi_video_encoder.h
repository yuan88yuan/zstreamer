/*=============================================================================
    zst_oneapi_video_encoder.h — Intel oneAPI/oneVPL Video Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ONEAPI_VIDEO_ENCODER_FACTORY       "oneapienc"
#define ZST_ONEAPI_VIDEO_ENCODER_FACTORY_ALIAS "oneapi_video_encoder"

#define ZST_ONEAPI_VIDEO_ENCODER_PROP_CODEC      "codec"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_PRESET     "preset"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_BITRATE    "bitrate"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_GOP_SIZE   "gop-size"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_PROFILE    "profile"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_LEVEL      "level"
#define ZST_ONEAPI_VIDEO_ENCODER_PROP_FPS        "fps"

zst_element_t* zst_oneapi_video_encoder_create(void);

#ifdef __cplusplus
}
#endif
