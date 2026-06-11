/*=============================================================================
    zst_rtmp_sink.h — Rtmp Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTMP_SINK_FACTORY "rtmpsink"
#define ZST_RTMP_SINK_PROP_URL "url"

zst_element_t* zst_rtmp_sink_create(void);

#ifdef __cplusplus
}
#endif
