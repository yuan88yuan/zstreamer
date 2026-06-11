/*=============================================================================
    zst_rtmp_source.h — Rtmp Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTMP_SOURCE_FACTORY "rtmpsrc"
#define ZST_RTMP_SOURCE_PROP_URL "url"

zst_element_t* zst_rtmp_source_create(const char* url);

#ifdef __cplusplus
}
#endif
