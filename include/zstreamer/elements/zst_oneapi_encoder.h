/*=============================================================================
    zst_oneapi_encoder.h — Intel oneAPI/oneVPL video encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ONEAPI_ENCODER_FACTORY "oneapienc"

#define ZST_ONEAPI_ENCODER_PROP_CODEC    "codec"
#define ZST_ONEAPI_ENCODER_PROP_BITRATE  "bitrate"
#define ZST_ONEAPI_ENCODER_PROP_GOP_SIZE "gop-size"
#define ZST_ONEAPI_ENCODER_PROP_FPS      "fps"
#define ZST_ONEAPI_ENCODER_PROP_PROFILE  "profile"
#define ZST_ONEAPI_ENCODER_PROP_LEVEL    "level"

zst_element_t* zst_oneapi_encoder_create(void);

#ifdef __cplusplus
}
#endif
