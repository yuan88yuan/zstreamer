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

zst_element_t* zst_rtsp_server_create(void);

#ifdef __cplusplus
}
#endif
