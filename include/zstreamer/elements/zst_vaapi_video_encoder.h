/*=============================================================================
    zst_vaapi_video_encoder.h — VA-API Video Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VAAPI_VIDEO_ENCODER_FACTORY       "vaapienc"
#define ZST_VAAPI_VIDEO_ENCODER_FACTORY_ALIAS "vaapi_video_encoder"

#define ZST_VAAPI_VIDEO_ENCODER_PROP_DEVICE       "device"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_CODEC        "codec"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_PRESET       "preset"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_BITRATE      "bitrate"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_GOP_SIZE     "gop-size"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_PROFILE      "profile"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_LEVEL        "level"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_FPS          "fps"
#define ZST_VAAPI_VIDEO_ENCODER_PROP_RATE_CONTROL "rate-control"

zst_element_t* zst_vaapi_video_encoder_create(void);

#ifdef __cplusplus
}
#endif
