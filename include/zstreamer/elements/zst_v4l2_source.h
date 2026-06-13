/*=============================================================================
    zst_v4l2_source.h — V4L2 Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_V4L2_SOURCE_FACTORY "v4l2src"

#define ZST_V4L2_SOURCE_PROP_DEVICE   "device"
#define ZST_V4L2_SOURCE_PROP_WIDTH    "width"
#define ZST_V4L2_SOURCE_PROP_HEIGHT   "height"
#define ZST_V4L2_SOURCE_PROP_PIXEL_FORMAT "pixel-format"

zst_element_t* zst_v4l2_source_create(void);

#ifdef __cplusplus
}
#endif
