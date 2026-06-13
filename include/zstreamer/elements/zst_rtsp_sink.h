/*=============================================================================
    zst_rtsp_sink.h — Rtsp Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SINK_FACTORY "rtspsink"

#define ZST_RTSP_SINK_PROP_URL "url"
#define ZST_RTSP_SINK_PROP_LISTEN_PORT "listen-port"
#define ZST_RTSP_SINK_PROP_MOUNT_POINT "mount-point"
#define ZST_RTSP_SINK_PROP_TRANSPORT "transport"
#define ZST_RTSP_SINK_PROP_MAX_CLIENTS "max-clients"
#define ZST_RTSP_SINK_PROP_RTCP_INTERVAL_MS "rtcp-interval-ms"

zst_element_t* zst_rtsp_sink_create(void);

#ifdef __cplusplus
}
#endif
