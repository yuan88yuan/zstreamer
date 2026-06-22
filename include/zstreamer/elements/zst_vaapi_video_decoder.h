/*=============================================================================
    zst_vaapi_video_decoder.h — VA-API Video Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VAAPI_VIDEO_DECODER_FACTORY       "vaapidec"
#define ZST_VAAPI_VIDEO_DECODER_FACTORY_ALIAS "vaapi_video_decoder"

#define ZST_VAAPI_VIDEO_DECODER_PROP_DEVICE       "device"
#define ZST_VAAPI_VIDEO_DECODER_PROP_CODEC        "codec"
#define ZST_VAAPI_VIDEO_DECODER_PROP_MEMORY_TYPE  "memory-type"

zst_element_t* zst_vaapi_video_decoder_create(void);

#ifdef __cplusplus
}
#endif
