/*=============================================================================
    zst_rtsp_source.h — Rtsp Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SOURCE_FACTORY "rtspsrc"

zst_element_t* zst_rtsp_source_create(const char* url);

#ifdef __cplusplus
}
#endif
