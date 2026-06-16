/*=============================================================================
    zst_nv_video_scaler.h — NV V4L2 Video Scaler convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NV_VIDEO_SCALER_FACTORY "nvvideoscaler"

#define ZST_NV_VIDEO_SCALER_PROP_WIDTH        "width"
#define ZST_NV_VIDEO_SCALER_PROP_HEIGHT       "height"
#define ZST_NV_VIDEO_SCALER_PROP_PIXEL_FORMAT "pixel-format"

zst_element_t* zst_nv_video_scaler_create(void);

#ifdef __cplusplus
}
#endif
