/*=============================================================================
    zst_rtsp_sink.h — Rtsp Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SINK_FACTORY "rtspsink"

zst_element_t* zst_rtsp_sink_create(void);

#ifdef __cplusplus
}
#endif
