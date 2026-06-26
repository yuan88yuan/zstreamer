/*=============================================================================
    zst_rtsp_server.h — Rtsp Server convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SERVER_FACTORY "rtsp_server"

#define ZST_RTSP_SERVER_PROP_LISTEN_PORT         "listen-port"
#define ZST_RTSP_SERVER_PROP_LISTEN_PORT_ALIAS   "listen_port"
#define ZST_RTSP_SERVER_PROP_SESSION_COUNT       "session_count"
#define ZST_RTSP_SERVER_PROP_CLIENT_COUNT        "client_count"
#define ZST_RTSP_SERVER_PROP_FORCE_TCP           "force-tcp"
#define ZST_RTSP_SERVER_PROP_MULTICAST_ADDRESS   "multicast-address"
#define ZST_RTSP_SERVER_PROP_MULTICAST_PORT_BASE "multicast-port-base"
#define ZST_RTSP_SERVER_PROP_MULTICAST_TTL       "multicast-ttl"
#define ZST_RTSP_SERVER_PROP_UDP_TIMESTAMP_PACING "udp-timestamp-pacing"
#define ZST_RTSP_SERVER_PROP_UDP_PACING_TOLERANCE_MS "udp-pacing-tolerance-ms"
#define ZST_RTSP_SERVER_PROP_UDP_PACING_RESET_THRESHOLD_MS "udp-pacing-reset-threshold-ms"
#define ZST_RTSP_SERVER_PROP_UDP_MAX_LATENESS_MS "udp-max-lateness-ms"

zst_element_t* zst_rtsp_server_create(void);

#ifdef __cplusplus
}
#endif
