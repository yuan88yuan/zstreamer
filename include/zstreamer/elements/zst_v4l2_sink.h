/*=============================================================================
    zst_v4l2_sink.h — V4L2 Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_V4L2_SINK_FACTORY "v4l2sink"

#define ZST_V4L2_SINK_PROP_DEVICE       "device"
#define ZST_V4L2_SINK_PROP_WIDTH        "width"
#define ZST_V4L2_SINK_PROP_HEIGHT       "height"
#define ZST_V4L2_SINK_PROP_PIXEL_FORMAT "pixel-format"
#define ZST_V4L2_SINK_PROP_MEMORY_TYPE  "memory-type"

zst_element_t* zst_v4l2_sink_create(void);

#ifdef __cplusplus
}
#endif
