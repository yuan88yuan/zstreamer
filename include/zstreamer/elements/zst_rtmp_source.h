/*=============================================================================
    zst_rtmp_source.h — Rtmp Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTMP_SOURCE_FACTORY "rtmpsrc"

#define ZST_RTMP_SRC_PROP_URL                    "url"
#define ZST_RTMP_SRC_PROP_RTMP_URL               "rtmp_url"
#define ZST_RTMP_SRC_PROP_LIVE                   "live"
#define ZST_RTMP_SRC_PROP_BUFFER_TIME            "buffer-time"
#define ZST_RTMP_SRC_PROP_SWF_URL                "swf-url"
#define ZST_RTMP_SRC_PROP_RECONNECT              "reconnect"
#define ZST_RTMP_SRC_PROP_RECONNECT_DELAY_MS     "reconnect-delay-ms"
#define ZST_RTMP_SRC_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"
#define ZST_RTMP_SOURCE_PROP_URL "url"
#define ZST_RTMP_SOURCE_PROP_RTMP_URL "rtmp_url"
#define ZST_RTMP_SOURCE_PROP_LIVE "live"
#define ZST_RTMP_SOURCE_PROP_BUFFER_TIME "buffer-time"
#define ZST_RTMP_SOURCE_PROP_SWF_URL "swf-url"
#define ZST_RTMP_SOURCE_PROP_RECONNECT "reconnect"
#define ZST_RTMP_SOURCE_PROP_RECONNECT_DELAY_MS "reconnect-delay-ms"
#define ZST_RTMP_SOURCE_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"

zst_element_t* zst_rtmp_source_create(const char* url);

#ifdef __cplusplus
}
#endif
